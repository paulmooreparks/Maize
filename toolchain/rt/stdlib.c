/* toolchain/rt/stdlib.c -- process termination + the malloc family over sbrk
 * (maize-76, decisions 7340 / 7344 / 7346).
 *
 * TERMINATION FUNNEL
 *   _Exit(code) -> _exit(code)                (raw SYS $3C; bypasses atexit/flush)
 *   exit(code)  -> run atexit handlers LIFO, then _exit(code) (stdio is unbuffered
 *                                              at M1, so nothing to flush; the
 *                                              buffered-stdio flush-on-exit hook is
 *                                              maize-120 via atexit, not here)
 *   abort()     -> _exit(134)                 (128 + SIGABRT(6); Maize has no
 *                                              signals, an honest deviation;
 *                                              bypasses atexit per the C standard)
 * crt0 routes main's return value through exit() (decision 7346).
 *
 * ALLOCATOR (decision 7340): address-ordered first-fit free list with boundary
 * coalescing over sbrk.
 *
 *   - 8-byte header per block: header = total_size | inuse(bit0). total_size is
 *     the whole block INCLUDING the header, rounded up to 16, so its low 4 bits
 *     are always 0 and bit0 is free for the in-use flag.
 *   - Payloads are 16-byte aligned. The heap break is padded once at init so the
 *     first header lands at an address == 8 (mod 16); since every block size is a
 *     multiple of 16, all headers stay at == 8 (mod 16) and all payloads at 0
 *     (mod 16).
 *   - Free blocks are threaded into an ASCENDING-ADDRESS singly linked list; the
 *     "next free" pointer lives in the first 8 bytes of a free block's payload.
 *     Minimum block is 16 bytes (8 header + 8 for that pointer).
 *   - malloc grows the heap via sbrk in max(request, 64 KiB) chunks; a fresh chunk
 *     is inserted as one free block and coalesced with the previous tail if free.
 */
#include "stdlib.h"
#include "ctype.h"    /* isspace, isdigit (maize-142 numeric conversions) */
#include "errno.h"
#include "string.h"   /* memcpy, memset, strlen */
#include "syscall.h"  /* sys_brk, _exit, sys_clock_ms, open */
#include "fcntl.h"    /* maize-94: O_* flags for mkstemp */

/* Libc-internal stdio flush (body in stdio.c). exit() calls it directly AFTER the
 * atexit chain (maize-276) so buffered streams flush in C-standard order; forward-
 * declared here to avoid pulling all of <stdio.h> (FILE et al.) into stdlib.c. */
void __stdio_flush_all(void);

/* --- termination -------------------------------------------------------------- */
/* These are noreturn: _exit issues SYS $3C, which halts the VM, so control never
 * reaches the fall-through `return`. The `_Noreturn` keyword is honest here: cproc
 * emits a `hlt` block terminator that the qbe -t maize backend now lowers to HALT
 * (maize-102); _exit carries `_Noreturn` for the same reason. See stdlib.h. */

/* atexit registry: a fixed 32-slot table (ATEXIT_MAX is the C-mandated minimum).
   Handlers push here and run in LIFO order from exit(). No dedup, no NULL guard
   beyond storing the pointer. */
#define ATEXIT_MAX 32

static void (*g_atexit[ATEXIT_MAX])(void);
static int g_atexit_n;

int
atexit(void (*func)(void))
{
    if (g_atexit_n >= ATEXIT_MAX)
        return -1;   /* table full */
    g_atexit[g_atexit_n] = func;
    g_atexit_n++;
    return 0;
}

_Noreturn void
_Exit(int status)
{
    _exit(status);   /* raw SYS $3C; bypasses atexit/flush */
}

_Noreturn void
exit(int status)
{
    /* Run registered handlers in LIFO order, then flush stdio, then hand off to
       _exit. The counter is decremented before each indirect call, so a handler
       that itself calls exit() resumes with the remaining handlers rather than
       looping. Per C (7.22.4.4), atexit handlers run first and open streams are
       flushed AFTER them; a handler that prints to buffered stdout must have its
       output flushed, so the flush is a direct call here, NOT an atexit entry
       (maize-276: registering the flush via atexit made LIFO order run it before a
       handler registered earlier, silently dropping that handler's output).
       _Exit/abort bypass both the handlers and this flush. */
    while (g_atexit_n > 0) {
        void (*fn)(void) = g_atexit[--g_atexit_n];
        fn();
    }
    __stdio_flush_all();
    _exit(status);
}

_Noreturn void
abort(void)
{
    _exit(134);   /* 128 + SIGABRT(6); no signal delivery on Maize */
}

/* --- sbrk wrapper ------------------------------------------------------------- */

void *
sbrk(long incr)
{
    unsigned long cur = (unsigned long)sys_brk((void *)0);       /* query */
    unsigned long req = cur + (unsigned long)incr;
    unsigned long got = (unsigned long)sys_brk((void *)req);     /* set   */
    if (got != req) {
        errno = ENOMEM;
        return (void *)-1L;
    }
    return (void *)cur;   /* the OLD break */
}

/* --- allocator ---------------------------------------------------------------- */

#define ALIGN      16UL
#define HDR         8UL
#define MIN_BLOCK  16UL
#define CHUNK      (64UL * 1024UL)
#define SIZEMASK   (~15UL)   /* clears the low 4 bits: in-use flag + alignment slack */
#define INUSE       1UL

/* maize-348: quesOS's guest kernel page size (os/quesos/quesos.c:146). Rounding each
 * bigalloc grant up to this size before calling sys_bigalloc keeps the kernel's internal
 * page rounding (do_bigalloc: n = ceil(size/PAGE_SIZE)) from ever diverging from the amount
 * bigalloc_grow tracks, so grant-to-grant VA contiguity can be detected past the first chunk.
 * This is a quesOS-specific constant baked into rt on purpose; see the SYSCALL-ABI.md note. */
#define BIGALLOC_PAGE 0x1000UL

/* A block header is one 8-byte word at the block's base:
 *   word[0] = header  (total size | inuse)
 *   word[1] = free-list "next" pointer  (only meaningful while the block is free)
 * word[] indexes step by 8 bytes, so word[1] is exactly the payload's first word.
 */
typedef unsigned long word;

static word *g_free_head = 0;   /* lowest-address free block, or NULL */
static char *g_heap_end  = 0;   /* the break we manage; 0 until first init */

/* maize-348: address of the next contiguous bigalloc header slot, or 0 before the first
 * successful grant. Once set it is always == 8 (mod 16), so a block header placed at it keeps
 * payloads 16-aligned. bigalloc_grow uses it to detect when a fresh sys_bigalloc grant landed
 * exactly where the previous grant ended, so the two blocks tile and free_insert can coalesce
 * them. It is an ordinary process global: fork's eager copy carries a stale value into the
 * child, but the child's first grant returns the reset window base BIGALLOC_BASE, which never
 * equals the inherited g_bigalloc_end + HDR, so the contiguity check correctly falls back to
 * the independently padded construction. No fork hook is needed. */
static char *g_bigalloc_end = 0;

static unsigned long
round_block(unsigned long payload)
{
    unsigned long total = payload + HDR;
    total = (total + (ALIGN - 1)) & SIZEMASK;
    if (total < MIN_BLOCK)
        total = MIN_BLOCK;
    return total;
}

static int
ensure_init(void)
{
    unsigned long base, pad;

    if (g_heap_end)
        return 0;

    base = (unsigned long)sbrk(0);
    if (base == (unsigned long)-1L)
        return -1;

    /* Pad so the first header sits at == 8 (mod 16), giving 16-aligned payloads. */
    pad = (8UL - (base & 15UL)) & 15UL;
    if (pad) {
        if (sbrk((long)pad) == (void *)-1L)
            return -1;
    }
    g_heap_end = (char *)(base + pad);
    g_free_head = 0;
    return 0;
}

/* Insert free block b into the ascending-address free list, coalescing with a
 * physically adjacent free predecessor and/or successor. b[0] already holds the
 * block size with the in-use bit cleared. */
static void
free_insert(word *b)
{
    word *prev = 0;
    word *cur = g_free_head;
    unsigned long bsz;

    while (cur && cur < b) {
        prev = cur;
        cur = (word *)cur[1];
    }

    /* Link b between prev and cur. */
    b[1] = (word)cur;
    if (prev)
        prev[1] = (word)b;
    else
        g_free_head = b;

    /* Coalesce forward: b + b.size == cur ? */
    if (cur) {
        bsz = b[0] & SIZEMASK;
        if ((char *)b + bsz == (char *)cur) {
            b[0] = bsz + (cur[0] & SIZEMASK);
            b[1] = cur[1];
        }
    }
    /* Coalesce backward: prev + prev.size == b ? */
    if (prev) {
        unsigned long psz = prev[0] & SIZEMASK;
        if ((char *)prev + psz == (char *)b) {
            prev[0] = psz + (b[0] & SIZEMASK);
            prev[1] = b[1];
        }
    }
}

/* Grow the heap by at least `total` bytes (rounded to a chunk), append the new
 * region as a free block, coalesce, and return 0 on success / -1 on failure. */
static int
grow(unsigned long total)
{
    unsigned long amount = total > CHUNK ? total : CHUNK;
    char *start = (char *)sbrk((long)amount);
    word *b;

    if (start == (char *)-1L)
        return -1;

    /* start == g_heap_end (we are the only sbrk caller and track the break). */
    b = (word *)g_heap_end;
    b[0] = amount;            /* free (inuse bit clear); amount is 16-aligned */
    g_heap_end += amount;
    free_insert(b);
    return 0;
}

/* maize-251: fall back to sys_bigalloc (SYS $FF) when ordinary sbrk growth is refused. quesOS
 * caps a process's sbrk heap at USER_BRK_MAX (~960 KiB), far below DOOM's ~6 MiB zone-heap
 * malloc; sys_bigalloc serves that from a separate 16 MiB per-process VA window. A result <= 0
 * is failure: an -errno band value, OR the transparent BARE-VM no-op (a bare-VM SYS $FF hits
 * the native table's unmatched-case `return 0`, treated as failure here), so this needs ZERO
 * world-probe in stdlib.c -- under bare-VM it simply no-ops and malloc falls through to NULL.
 * The returned VA is page-aligned (>= 16-aligned); pad the block header to 8 (mod 16) so
 * payloads stay 16-aligned exactly like the sbrk path, then thread the region into the free
 * list. bigalloc regions sit at a high VA far from the sbrk heap, so free_insert never
 * false-coalesces them with sbrk blocks. Only reached when grow() fails, so every small
 * allocation and the whole bare-VM build are byte-for-byte unaffected. */
static int
bigalloc_grow(unsigned long total)
{
    unsigned long raw = (total + ALIGN) > CHUNK ? (total + ALIGN) : CHUNK;
    /* maize-348: round up to the guest kernel page size so rt's tracked `amount` matches the
     * kernel's own n*PAGE_SIZE mapping exactly; without this only the first, page-aligned
     * CHUNK-sized grants would ever be detected as contiguous. */
    unsigned long amount = (raw + (BIGALLOC_PAGE - 1)) & ~(BIGALLOC_PAGE - 1);
    long va = sys_bigalloc(amount);
    word *b;

    if (va <= 0)
        return -1;   /* -errno, or the bare-VM native no-op (0): malloc falls through to NULL */

    if (g_bigalloc_end && (unsigned long)va == (unsigned long)g_bigalloc_end + HDR) {
        /* maize-348: this grant is virtually contiguous with the previous one, so the previous
         * grant's trailing 8-byte pad IS this grant's header slot. Place the header there and
         * give the block the grant's full `amount` as its size, wasting nothing. The header
         * bytes [va-HDR, va) lie in the previous grant's already-mapped range (its dead tail),
         * and the payload [va, va+amount-HDR) lies in this grant's range, so nothing is written
         * outside a mapped page. Because the previous block ends exactly at this header,
         * free_insert coalesces the two. Opportunistic only: the check falls back below whenever
         * the kernel does not hand out a contiguous grant. See the SYSCALL-ABI.md note. */
        b = (word *)g_bigalloc_end;
        b[0] = amount & SIZEMASK;   /* amount is page-aligned, hence already 16-aligned */
    } else {
        /* First grant, or discontiguous (post-fork reset, or a future kernel that no longer
         * hands out contiguous grants): pay the one-time 16-byte pad exactly as before. Header
         * at va+HDR lands at 8 (mod 16) since va is page-aligned, giving 16-aligned payloads.
         * The block spans [va+HDR, va+HDR+(amount-ALIGN)) subset [va, va+amount). */
        b = (word *)((unsigned long)va + HDR);
        b[0] = (amount - ALIGN) & SIZEMASK;   /* 16-aligned free block (amount is 16-aligned) */
    }
    /* Both branches leave g_bigalloc_end at va+amount-HDR (== 8 mod 16), so the invariant
     * self-sustains: a cursor that starts 8-mod-16 stays 8-mod-16 across every grant. */
    g_bigalloc_end = (char *)b + (b[0] & SIZEMASK);
    free_insert(b);
    return 0;
}

/* Carve `total` bytes out of free block cur (whose predecessor in the free list is
 * prev), splitting off the remainder as a new free block when it is large enough. */
static void
alloc_from(word *prev, word *cur, unsigned long total)
{
    unsigned long csz = cur[0] & SIZEMASK;
    unsigned long remain = csz - total;
    word *nextfree = (word *)cur[1];

    if (remain >= MIN_BLOCK) {
        word *tail = (word *)((char *)cur + total);
        tail[0] = remain;              /* free remainder */
        tail[1] = (word)nextfree;
        cur[0] = total | INUSE;
        if (prev)
            prev[1] = (word)tail;
        else
            g_free_head = tail;
    } else {
        /* Whole block; unlink it. */
        cur[0] = csz | INUSE;
        if (prev)
            prev[1] = (word)nextfree;
        else
            g_free_head = nextfree;
    }
}

static word *
find_fit(unsigned long total, word **prev_out)
{
    word *prev = 0;
    word *cur = g_free_head;

    while (cur) {
        if ((cur[0] & SIZEMASK) >= total) {
            *prev_out = prev;
            return cur;
        }
        prev = cur;
        cur = (word *)cur[1];
    }
    *prev_out = 0;
    return 0;
}

void *
malloc(size_t size)
{
    unsigned long total;
    word *prev;
    word *cur;

    /* malloc(0) contract: return a unique, freeable pointer (size rounded up to a
       minimum block), never NULL-on-zero. */
    if (size == 0)
        size = 1;

    if (ensure_init() != 0)
        return 0;

    total = round_block(size);

    cur = find_fit(total, &prev);
    if (!cur) {
        /* maize-251: on an sbrk-refused grow, try the bigalloc window (quesOS large-alloc
         * fallback) before giving up. Transparent no-op under bare-VM (sys_bigalloc <= 0). */
        if (grow(total) != 0 && bigalloc_grow(total) != 0)
            return 0;
        cur = find_fit(total, &prev);
        if (!cur)
            return 0;
    }
    alloc_from(prev, cur, total);
    return (char *)cur + HDR;
}

void
free(void *ptr)
{
    word *b;

    if (!ptr)
        return;   /* free(NULL) is a no-op */

    b = (word *)((char *)ptr - HDR);
    b[0] &= SIZEMASK;   /* clear the in-use bit */
    free_insert(b);
}

void *
calloc(size_t nmemb, size_t size)
{
    unsigned long n;
    void *p;

    n = (unsigned long)nmemb * (unsigned long)size;
    /* Overflow guard: if the multiply wrapped, refuse. */
    if (nmemb != 0 && n / (unsigned long)nmemb != (unsigned long)size) {
        errno = ENOMEM;
        return 0;
    }

    p = malloc(n);
    if (!p)
        return 0;

    /* A reused free-list block is NOT zeroed (only fresh sbrk pages are), so
       calloc must always clear. */
    memset(p, 0, n);
    return p;
}

void *
realloc(void *ptr, size_t size)
{
    word *b;
    unsigned long oldtotal, oldpayload, newtotal;
    void *np;

    if (!ptr)
        return malloc(size);        /* realloc(NULL, n) == malloc(n) */
    if (size == 0) {
        free(ptr);                  /* realloc(p, 0) == free(p), returns NULL */
        return 0;
    }

    b = (word *)((char *)ptr - HDR);
    oldtotal = b[0] & SIZEMASK;
    oldpayload = oldtotal - HDR;
    newtotal = round_block(size);

    if (newtotal <= oldtotal)
        return ptr;                 /* fits in place */

    np = malloc(size);
    if (!np)
        return 0;                   /* original block left intact */
    memcpy(np, ptr, oldpayload < size ? oldpayload : size);
    free(ptr);
    return np;
}

/* --- numeric conversions (maize-142) ------------------------------------------ */
/* Pure computation over the ctype predicates; no syscalls, no VM surface.
 *
 * long is 64-bit on Maize (unsigned long backs size_t, stddef.h:13). The slice has
 * no limits.h and this card does not introduce one (decision 8257): LONG_MAX/LONG_MIN
 * live as file-private macros here. A later card that needs a full limits.h (DOOM may)
 * can promote them. */
#define LONG_MAX 0x7fffffffffffffffL              /* 9223372036854775807 */
#define LONG_MIN (-LONG_MAX - 1L)                 /* -9223372036854775808 */

int
abs(int j)
{
    /* Modular negate. abs(INT_MIN) is UB per the standard and is not special-cased
       (it returns INT_MIN unchanged via wraparound). Never touches errno. */
    return j < 0 ? -j : j;
}

long
labs(long j)
{
    /* labs(LONG_MIN) is UB per the standard, mirroring abs above. */
    return j < 0 ? -j : j;
}

/* Map a character to its digit value in `base`, or -1 if it is not a valid digit
 * in that base. Digits 0-9, then a-z / A-Z for values 10..35; only values < base
 * are accepted. */
static int
digit_value(int c, int base)
{
    int d;

    if (c >= '0' && c <= '9')
        d = c - '0';
    else if (c >= 'a' && c <= 'z')
        d = c - 'a' + 10;
    else if (c >= 'A' && c <= 'Z')
        d = c - 'A' + 10;
    else
        return -1;
    return d < base ? d : -1;
}

long
strtol(const char *nptr, char **endptr, int base)
{
    const char *p = nptr;
    int neg = 0;
    int any = 0, ovf = 0;
    unsigned long acc, cutoff, cutlim;

    /* Invalid base (not 0 and not in 2..36): no conversion, errno=EINVAL, endptr=nptr
       (decision 8260 / POSIX; EINVAL is already in errno.h). */
    if (base != 0 && (base < 2 || base > 36)) {
        if (endptr)
            *endptr = (char *)nptr;
        errno = EINVAL;
        return 0;
    }

    /* 1. Skip leading whitespace. */
    while (isspace((unsigned char)*p))
        p++;

    /* 2. Optional single sign. */
    if (*p == '+' || *p == '-') {
        neg = (*p == '-');
        p++;
    }

    /* 3. Base / prefix resolution. A 0x/0X prefix is consumed only for base 0 or 16
       AND only when a hex digit actually follows it; otherwise the leading '0' is a
       plain zero digit and conversion stops at the 'x' (the bare-"0x" corner). */
    if ((base == 0 || base == 16)
        && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')
        && digit_value((unsigned char)p[2], 16) >= 0) {
        p += 2;
        base = 16;
    } else if (base == 0) {
        base = (p[0] == '0') ? 8 : 10;
    }

    /* 4. Accumulate. glibc-style cutoff/cutlim clamp: on the first digit that would
       exceed the signed range, clamp and keep consuming remaining valid digits so
       endptr still lands past the whole numeric token. */
    cutoff = neg ? (unsigned long)LONG_MAX + 1UL : (unsigned long)LONG_MAX;
    cutlim = cutoff % (unsigned long)base;
    cutoff /= (unsigned long)base;
    acc = 0;
    for (;; p++) {
        int d = digit_value((unsigned char)*p, base);
        if (d < 0)
            break;
        any = 1;
        if (ovf)
            continue;
        if (acc > cutoff || (acc == cutoff && (unsigned long)d > cutlim)) {
            ovf = 1;
            continue;
        }
        acc = acc * (unsigned long)base + (unsigned long)d;
    }

    /* 5/6. endptr + result. errno is NOT cleared on the success path (standard C:
       strtol only ever sets errno). */
    if (ovf) {
        errno = ERANGE;
        if (endptr)
            *endptr = (char *)p;
        return neg ? LONG_MIN : LONG_MAX;
    }
    if (!any) {
        if (endptr)
            *endptr = (char *)nptr;   /* no conversion: endptr = nptr */
        return 0;
    }
    if (endptr)
        *endptr = (char *)p;
    return neg ? -(long)acc : (long)acc;
}

/* strtoll (maize-94): the long long twin of strtol, needed by borrowed sbase
   (libutil/strtonum.c's strtonum, which printf's estrtonum wraps for field
   width / precision parsing). Maize is LP64 with long == long long == 64 bit
   (limits.h), so LLONG_MAX/LLONG_MIN equal LONG_MAX/LONG_MIN and this body is
   strtol with a wider accumulator; it shares digit_value and the same clamp. */
#define LLONG_MAX 0x7fffffffffffffffLL
#define LLONG_MIN (-LLONG_MAX - 1LL)

long long
strtoll(const char *nptr, char **endptr, int base)
{
    const char *p = nptr;
    int neg = 0;
    int any = 0, ovf = 0;
    unsigned long long acc, cutoff, cutlim;

    if (base != 0 && (base < 2 || base > 36)) {
        if (endptr)
            *endptr = (char *)nptr;
        errno = EINVAL;
        return 0;
    }

    while (isspace((unsigned char)*p))
        p++;

    if (*p == '+' || *p == '-') {
        neg = (*p == '-');
        p++;
    }

    if ((base == 0 || base == 16)
        && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')
        && digit_value((unsigned char)p[2], 16) >= 0) {
        p += 2;
        base = 16;
    } else if (base == 0) {
        base = (p[0] == '0') ? 8 : 10;
    }

    cutoff = neg ? (unsigned long long)LLONG_MAX + 1ULL
                 : (unsigned long long)LLONG_MAX;
    cutlim = cutoff % (unsigned long long)base;
    cutoff /= (unsigned long long)base;
    acc = 0;
    for (;; p++) {
        int d = digit_value((unsigned char)*p, base);
        if (d < 0)
            break;
        any = 1;
        if (ovf)
            continue;
        if (acc > cutoff || (acc == cutoff && (unsigned long long)d > cutlim)) {
            ovf = 1;
            continue;
        }
        acc = acc * (unsigned long long)base + (unsigned long long)d;
    }

    if (ovf) {
        errno = ERANGE;
        if (endptr)
            *endptr = (char *)p;
        return neg ? LLONG_MIN : LLONG_MAX;
    }
    if (!any) {
        if (endptr)
            *endptr = (char *)nptr;
        return 0;
    }
    if (endptr)
        *endptr = (char *)p;
    return neg ? -(long long)acc : (long long)acc;
}

/* strtoul (maize-94): the unsigned twin of strtol, needed by borrowed sbase
   (printf.c parses %d/%c integer arguments with strtoul). Per C, a leading '-'
   is accepted and the result negated in the unsigned return type; on overflow it
   clamps to ULONG_MAX with errno=ERANGE. Shares digit_value with strtol. */
#define ULONG_MAX 0xffffffffffffffffUL

unsigned long
strtoul(const char *nptr, char **endptr, int base)
{
    const char *p = nptr;
    int neg = 0;
    int any = 0, ovf = 0;
    unsigned long acc, cutoff, cutlim;

    if (base != 0 && (base < 2 || base > 36)) {
        if (endptr)
            *endptr = (char *)nptr;
        errno = EINVAL;
        return 0;
    }

    while (isspace((unsigned char)*p))
        p++;

    if (*p == '+' || *p == '-') {
        neg = (*p == '-');
        p++;
    }

    if ((base == 0 || base == 16)
        && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')
        && digit_value((unsigned char)p[2], 16) >= 0) {
        p += 2;
        base = 16;
    } else if (base == 0) {
        base = (p[0] == '0') ? 8 : 10;
    }

    cutoff = ULONG_MAX / (unsigned long)base;
    cutlim = ULONG_MAX % (unsigned long)base;
    acc = 0;
    for (;; p++) {
        int d = digit_value((unsigned char)*p, base);
        if (d < 0)
            break;
        any = 1;
        if (ovf)
            continue;
        if (acc > cutoff || (acc == cutoff && (unsigned long)d > cutlim)) {
            ovf = 1;
            continue;
        }
        acc = acc * (unsigned long)base + (unsigned long)d;
    }

    if (ovf) {
        errno = ERANGE;
        if (endptr)
            *endptr = (char *)p;
        return ULONG_MAX;
    }
    if (!any) {
        if (endptr)
            *endptr = (char *)nptr;
        return 0;
    }
    if (endptr)
        *endptr = (char *)p;
    return neg ? -acc : acc;
}

int
atoi(const char *nptr)
{
    /* Single conversion core (decision 8259). The (int) cast truncates the long
       result; atoi does not report overflow (standard: UB). */
    return (int)strtol(nptr, (char **)0, 10);
}

/* --- environment / sort / float parse (maize-144) ----------------------------
 * Small pure-libc growth over existing primitives (malloc, the function-pointer
 * ABI, the maize-137 signed int<->double float codegen), added for the DOOM boot.
 * No new VM/ISA surface. Each helper stays inside the pinned qbe-maize backend's
 * authoring budget (small frame, a single loop per helper). */

/* --- environment (maize-94 decision 8942) ------------------------------------
 * crt0.mazm captures the process envp into `environ`, a NULL-terminated array of
 * "NAME=value" strings. getenv walks it; setenv/putenv/unsetenv mutate it over a
 * malloc-backed, growable array (no fixed cap -- DIRT: no silent failure ceiling). The
 * initial environ points into the SysV start block (the stack), so the first mutation
 * migrates it to a heap array this module owns (env_owned). This closes maize-144's
 * hardcoded-NULL getenv deviation; DOOM is unaffected (a real empty-environment lookup
 * still returns NULL). One loop per helper keeps each inside the qbe-maize backend's
 * pointer-indexing budget. */

char **environ = 0;                 /* set by crt0 (decision 8942); NULL before _start */

static char  **env_owned = 0;       /* this module's malloc'd array once migrated */
static size_t  env_cap   = 0;       /* slots in env_owned, including the NULL terminator */

static size_t
env_count(void)
{
    size_t n = 0;
    if (environ != NULL) { while (environ[n] != NULL) { n++; } }
    return n;
}

/* True when `entry` ("NAME=value") has key `name` (matched up to '=' / the NUL). */
static int
env_match(const char *entry, const char *name)
{
    size_t i = 0;
    while (name[i] != '\0' && entry[i] == name[i]) { i++; }
    return name[i] == '\0' && entry[i] == '=';
}

/* Index of `name` in environ, or -1. */
static long
env_find(const char *name)
{
    long i;
    if (environ == NULL) { return -1; }
    for (i = 0; environ[i] != NULL; i++) {
        if (env_match(environ[i], name)) { return i; }
    }
    return -1;
}

char *
getenv(const char *name)
{
    long idx;
    char *p;
    if (name == NULL || name[0] == '\0') { return NULL; }
    idx = env_find(name);
    if (idx < 0) { return NULL; }
    p = environ[idx];
    while (*p != '\0' && *p != '=') { p++; }
    return (*p == '=') ? p + 1 : NULL;
}

/* Ensure environ is env_owned with room for `need` entries + the NULL terminator,
 * migrating the initial stack-backed environ on first use. Returns 0, or -1 on OOM. */
static int
env_reserve(size_t need)
{
    size_t want = need + 1;
    size_t n, newcap, i;
    char **buf;
    if (environ == env_owned && env_cap >= want) { return 0; }
    n = env_count();
    newcap = (env_cap != 0) ? env_cap : 8;
    while (newcap < want) { newcap *= 2; }
    buf = (char **)malloc(newcap * sizeof(char *));
    if (buf == NULL) { return -1; }
    for (i = 0; i < n; i++) { buf[i] = environ[i]; }
    buf[n] = NULL;
    if (env_owned != NULL && environ == env_owned) { free(env_owned); }
    env_owned = buf;
    env_cap = newcap;
    environ = buf;
    return 0;
}

/* True when `s` contains an '=' (an invalid environment variable NAME). */
static int
name_has_eq(const char *s)
{
    size_t i = 0;
    while (s[i] != '\0') { if (s[i] == '=') { return 1; } i++; }
    return 0;
}

/* Build a freshly-malloc'd "NAME=value" string, or NULL on OOM. */
static char *
env_make_entry(const char *name, const char *value)
{
    size_t nlen = strlen(name);
    size_t vlen = strlen(value);
    char *entry = (char *)malloc(nlen + vlen + 2);
    if (entry == NULL) { return NULL; }
    memcpy(entry, name, nlen);
    entry[nlen] = '=';
    memcpy(entry + nlen + 1, value, vlen);
    entry[nlen + vlen + 1] = '\0';
    return entry;
}

int
setenv(const char *name, const char *value, int overwrite)
{
    long idx;
    size_t n;
    char *entry;
    if (name == NULL || name[0] == '\0' || name_has_eq(name)) { errno = EINVAL; return -1; }
    idx = env_find(name);
    if (idx >= 0 && !overwrite) { return 0; }
    entry = env_make_entry(name, value);
    if (entry == NULL) { errno = ENOMEM; return -1; }
    if (idx >= 0) {                              /* overwrite in place */
        environ[idx] = entry;                    /* old string leaked (POC allocator) */
        return 0;
    }
    n = env_count();
    if (env_reserve(n + 1) != 0) { free(entry); errno = ENOMEM; return -1; }
    environ[n] = entry;
    environ[n + 1] = NULL;
    return 0;
}

int
unsetenv(const char *name)
{
    long idx;
    size_t n, k;
    if (name == NULL || name[0] == '\0' || name_has_eq(name)) { errno = EINVAL; return -1; }
    idx = env_find(name);
    if (idx < 0) { return 0; }
    if (env_reserve(env_count()) != 0) { errno = ENOMEM; return -1; }
    idx = env_find(name);                        /* re-find after a possible migration */
    if (idx < 0) { return 0; }
    n = env_count();
    for (k = (size_t)idx; k < n; k++) { environ[k] = environ[k + 1]; }
    return 0;
}

int
putenv(char *string)
{
    /* string is "NAME=value"; POSIX lets it become part of the environment directly. We
     * copy the key out to reuse setenv's insert/replace, then point the slot at `string`
     * itself so a later getenv returns the caller's buffer (matching glibc's aliasing). */
    char key[256];
    size_t i;
    long idx;
    size_t n;
    if (string == NULL) { errno = EINVAL; return -1; }
    for (i = 0; string[i] != '\0' && string[i] != '=' && i < sizeof(key) - 1; i++) { key[i] = string[i]; }
    key[i] = '\0';
    if (string[i] != '=') { errno = EINVAL; return -1; }   /* no '=' : malformed */
    idx = env_find(key);
    if (idx >= 0) { environ[idx] = string; return 0; }
    n = env_count();
    if (env_reserve(n + 1) != 0) { errno = ENOMEM; return -1; }
    environ[n] = string;
    environ[n + 1] = NULL;
    return 0;
}

/* Swap `size` bytes between a and b using a single 1-byte temp (no dynamic
 * size-byte buffer, no VLA), one loop. The sole element-move primitive for qsort. */
static void
qsort_swap(char *a, char *b, size_t size)
{
    size_t k;
    for (k = 0; k < size; k++) {
        char t = a[k];
        a[k] = b[k];
        b[k] = t;
    }
}

/* qsort: correctness-first in-place SELECTION sort (decision 8352). O(n^2), needs
 * only the fixed 1-byte swap temp above, no recursion. compar is invoked through
 * the function-pointer ABI (the atexit indirect-call precedent). A sub-quadratic
 * median-of-3 quicksort is a deliberate perf-shelf follow-up, not this card. */
void
qsort(void *base, size_t nmemb, size_t size,
      int (*compar)(const void *, const void *))
{
    char *arr = (char *)base;
    size_t i, j, min;

    if (nmemb < 2 || size == 0)
        return;
    for (i = 0; i + 1 < nmemb; i++) {
        min = i;
        for (j = i + 1; j < nmemb; j++) {
            if (compar(arr + j * size, arr + min * size) < 0)
                min = j;
        }
        if (min != i)
            qsort_swap(arr + i * size, arr + min * size, size);
    }
}

/* atof: strtod-lite (decision 8353). Skips leading whitespace, an optional sign,
 * integer digits, and an optional '.'+fraction. No exponent, no full IEEE round-
 * tripping (DOOM needs only simple decimals like "3.14" / "-2" / "0.5"). It leans
 * on the maize-137 SIGNED int<->double path and double + - * / only: each digit is
 * kept as a signed int before the (double) cast, deliberately avoiding the
 * unsigned->float conversion the backend does not cover (BACKEND-COVERAGE.md). */
double
atof(const char *nptr)
{
    const char *p = nptr;
    int neg = 0;
    double result = 0.0;
    double scale;

    while (isspace((unsigned char)*p))
        p++;
    if (*p == '+' || *p == '-') {
        neg = (*p == '-');
        p++;
    }
    while (*p >= '0' && *p <= '9') {
        int d = *p - '0';                       /* signed digit, no unsigned->float */
        result = result * 10.0 + (double)d;
        p++;
    }
    if (*p == '.') {
        p++;
        scale = 0.1;
        while (*p >= '0' && *p <= '9') {
            int d = *p - '0';
            result = result + (double)d * scale;
            scale = scale * 0.1;
            p++;
        }
    }
    /* Negate via double subtraction (0.0 - result), not unary minus: cproc lowers a
     * float unary minus to the QBE `neg` instruction, which the pinned qbe -t maize
     * predates (its parser has no `neg` keyword). Double `-` is covered by maize-137. */
    return neg ? (0.0 - result) : result;
}

/* --- system (maize-148) ------------------------------------------------------
 * No shell on Maize (decision 8442). system(NULL) returns 0 to signal "no command
 * processor available" (standard C); any actual command returns -1 (cannot execute).
 * DOOM's i_system.c only needs it to link and observe a failure. Honest deviation. */
int
system(const char *command)
{
    if (command == NULL)
        return 0;   /* no command processor available */
    return -1;      /* cannot execute the command */
}

/* rand / srand (maize-94): the ISO C pseudo-random pair for borrowed oksh's $RANDOM.
 * The classic POSIX example LCG (glibc's TYPE_0 documented reference), yielding a
 * 15-bit value (RAND_MAX == 0x7fff), which is exactly the range oksh's $RANDOM expects.
 * Not cryptographic; $RANDOM is not meant to be. */
static unsigned long g_rand_next = 1;

int
rand(void)
{
	g_rand_next = g_rand_next * 1103515245UL + 12345UL;
	return (int)((g_rand_next >> 16) & 0x7fffUL);
}

void
srand(unsigned seed)
{
	g_rand_next = seed;
}

/* mkstemp (maize-94): create-and-open a uniquely-named temp file from a "...XXXXXX"
 * template (in place). hostfs has no O_EXCL, so uniqueness is best-effort: the six X's
 * are filled from a clock-seeded rolling counter and open() with O_CREAT|O_RDWR is
 * retried on failure. Returns the open fd, or -1 with errno (EINVAL for a bad template,
 * EEXIST if every attempt collided). Honest single-user wave-1 deviation. */
int
mkstemp(char *template)
{
	static const char cs[] = "abcdefghijklmnopqrstuvwxyz0123456789";
	static unsigned long ctr = 0;
	size_t len;
	char *x;
	int i;
	int tries;

	if (template == 0) {
		errno = EINVAL;
		return -1;
	}
	len = strlen(template);
	if (len < 6) {
		errno = EINVAL;
		return -1;
	}
	x = template + (len - 6);
	for (i = 0; i < 6; i++) {
		if (x[i] != 'X') {
			errno = EINVAL;
			return -1;
		}
	}

	for (tries = 0; tries < 256; tries++) {
		unsigned long v = sys_clock_ms() + (ctr++) + (unsigned long)tries;
		int fd;
		for (i = 0; i < 6; i++) {
			x[i] = cs[v % 36];
			v /= 36;
			if (v == 0) {
				v = ctr + (unsigned long)(i + 1);
			}
		}
		fd = open(template, O_RDWR | O_CREAT | O_TRUNC, 0600);
		if (fd >= 0) {
			return fd;
		}
	}
	errno = EEXIST;
	return -1;
}
