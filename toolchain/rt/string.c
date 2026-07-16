/* toolchain/rt/string.c -- freestanding <string.h> core (maize-76, decision 7343).
 *
 * memcpy/memset/memmove's forward path move a uint64_t word at a time with a
 * byte tail for the remainder (maize-212): the Maize VM's LD/ST are unaligned-safe
 * (memory_module::write_bytes / read()'s in-block fast path std::memcpys for
 * widths 1/2/4/8 regardless of address), so no alignment prologue is needed, and
 * a word LD/ST costs the interpreter about the same as a byte LD/ST, so moving 8
 * bytes per instruction instead of 1 is a real win on every guest bulk copy.
 * memmove's backward-overlap branch and memcmp stay byte-at-a-time (colder paths,
 * maize-212 decision). Comparisons return the unsigned-char difference, per the
 * C standard.
 *
 * maize-216: for large copies/sets (n >= BULK_SYSCALL_THRESHOLD) memcpy/memmove/
 * memset hand off to the host via SYS $F4 (sys_bulk_copy, memmove-safe) / SYS $F5
 * (sys_bulk_set), running at host memcpy speed instead of grinding the word loop
 * one interpreter step at a time. Below the threshold the inline word loop wins
 * (it beats a SYS dispatch + two block walks), so small struct/string copies keep
 * the fast path. The threshold is the measured-safe crossover; retune the one
 * constant if profiling moves it.
 */
#include "string.h"
#include "stdlib.h"   /* malloc (strdup); stdio.c pairs the two headers likewise */
#include "stdint.h"   /* uint64_t for the word-at-a-time copy/set body */
#include "syscall.h"  /* maize-216: sys_bulk_copy / sys_bulk_set for large n */

/* Crossover below which the inline word loop is faster than a host round-trip
   (SYS dispatch + two guest-memory block walks). The word loop costs ~14 host
   cycles per guest-byte; the syscall is host memcpy speed plus a fixed overhead
   of a few hundred cycles, so anything at or above a few dozen bytes already wins
   -- 256 is a conservative margin that never regresses small/medium copies. */
#define BULK_SYSCALL_THRESHOLD 256u

void *
memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (n >= BULK_SYSCALL_THRESHOLD) {   /* maize-216: hoist large copies to the host */
        sys_bulk_copy(dst, src, n);
        return dst;
    }
    while (n >= 8) {
        *(uint64_t *)d = *(uint64_t *)s;
        d += 8;
        s += 8;
        n -= 8;
    }
    while (n--)
        *d++ = *s++;
    return dst;
}

void *
memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (d == s || n == 0)
        return dst;
    if (n >= BULK_SYSCALL_THRESHOLD) {   /* maize-216: sys_bulk_copy is overlap-safe */
        sys_bulk_copy(dst, src, n);
        return dst;
    }
    if (d < s) {
        while (n >= 8) {
            *(uint64_t *)d = *(uint64_t *)s;
            d += 8;
            s += 8;
            n -= 8;
        }
        while (n--)
            *d++ = *s++;
    } else {
        /* dst overlaps ahead of src: copy from the top down. Byte loop retained
           (cold path, maize-212 decision: a backward word copy would need the
           tail handled at the low end instead of the high end, more failure-prone
           for a rare path). */
        d += n;
        s += n;
        while (n--)
            *--d = *--s;
    }
    return dst;
}

void *
memset(void *dst, int c, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    unsigned char v = (unsigned char)c;
    uint64_t vv;
    if (n >= BULK_SYSCALL_THRESHOLD) {   /* maize-216: hoist large fills to the host */
        sys_bulk_set(dst, c, n);
        return dst;
    }
    vv = (uint64_t)v * 0x0101010101010101UL;
    while (n >= 8) {
        *(uint64_t *)d = vv;
        d += 8;
        n -= 8;
    }
    while (n--)
        *d++ = v;
    return dst;
}

int
memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *pa = (const unsigned char *)a;
    const unsigned char *pb = (const unsigned char *)b;
    while (n--) {
        if (*pa != *pb)
            return (int)*pa - (int)*pb;
        pa++;
        pb++;
    }
    return 0;
}

void *
memchr(const void *s, int c, size_t n)
{
    const unsigned char *p = (const unsigned char *)s;
    unsigned char v = (unsigned char)c;
    while (n--) {
        if (*p == v)
            return (void *)p;
        p++;
    }
    return NULL;
}

size_t
strlen(const char *s)
{
    const char *p = s;
    while (*p)
        p++;
    return (size_t)(p - s);
}

int
strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int
strncmp(const char *a, const char *b, size_t n)
{
    while (n && *a && *a == *b) {
        a++;
        b++;
        n--;
    }
    if (n == 0)
        return 0;
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

char *
strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++) != '\0')
        ;
    return dst;
}

char *
strncpy(char *dst, const char *src, size_t n)
{
    char *d = dst;
    while (n && *src) {
        *d++ = *src++;
        n--;
    }
    /* Pad the remainder with NUL (strncpy does not guarantee termination but
       does NUL-fill any slack). */
    while (n--)
        *d++ = '\0';
    return dst;
}

char *
strcat(char *dst, const char *src)
{
    char *d = dst;
    while (*d)
        d++;
    while ((*d++ = *src++) != '\0')
        ;
    return dst;
}

char *
strncat(char *dst, const char *src, size_t n)
{
    char *d = dst;
    while (*d)
        d++;
    while (n && *src) {
        *d++ = *src++;
        n--;
    }
    *d = '\0';
    return dst;
}

char *
strchr(const char *s, int c)
{
    char ch = (char)c;
    for (;; s++) {
        if (*s == ch)
            return (char *)s;
        if (*s == '\0')
            return NULL;
    }
}

char *
strrchr(const char *s, int c)
{
    char ch = (char)c;
    const char *last = NULL;
    for (;; s++) {
        if (*s == ch)
            last = s;
        if (*s == '\0')
            return (char *)last;
    }
}

/* --- search / tokenize (maize-100) -------------------------------------------
 * Byte-at-a-time, no allocation, C-locale. Results are plain runtime pointers
 * into the caller's buffers, so the pinned qbe -t maize backend never has to
 * fold a `$sym + N` offset (the strchr authoring note above). */

char *
strstr(const char *haystack, const char *needle)
{
    /* An empty needle matches at the start of the haystack (C contract). */
    if (*needle == '\0')
        return (char *)haystack;

    for (; *haystack; haystack++) {
        const char *h = haystack;
        const char *n = needle;
        while (*h && *n && *h == *n) {
            h++;
            n++;
        }
        if (*n == '\0')
            return (char *)haystack;   /* whole needle matched */
        /* Mismatch: restart the needle one byte further along (backtrack). */
    }
    return NULL;
}

char *
strpbrk(const char *s, const char *accept)
{
    for (; *s; s++) {
        const char *a = accept;
        while (*a) {
            if (*s == *a)
                return (char *)s;
            a++;
        }
    }
    return NULL;   /* also the empty-accept case */
}

size_t
strspn(const char *s, const char *accept)
{
    const char *p = s;
    for (; *p; p++) {
        const char *a = accept;
        while (*a && *a != *p)
            a++;
        if (*a == '\0')
            break;   /* *p is not in accept: the initial run ends here */
    }
    return (size_t)(p - s);
}

size_t
strcspn(const char *s, const char *reject)
{
    const char *p = s;
    for (; *p; p++) {
        const char *r = reject;
        while (*r) {
            if (*r == *p)
                return (size_t)(p - s);   /* first rejected byte */
            r++;
        }
    }
    return (size_t)(p - s);   /* no reject byte found: full length */
}

char *
strtok_r(char *str, const char *delim, char **saveptr)
{
    char *start;
    char *p;

    if (str == NULL)
        str = *saveptr;   /* continuation: resume where we left off */

    /* Skip any leading delimiter bytes (collapses consecutive delimiters). */
    while (*str) {
        const char *d = delim;
        int is_delim = 0;
        while (*d) {
            if (*str == *d) {
                is_delim = 1;
                break;
            }
            d++;
        }
        if (!is_delim)
            break;
        str++;
    }

    if (*str == '\0') {
        *saveptr = str;   /* exhausted */
        return NULL;
    }

    start = str;

    /* Scan to the next delimiter, NUL-terminate the token, and record the
       resume point just past it. */
    for (p = start; *p; p++) {
        const char *d = delim;
        while (*d) {
            if (*p == *d) {
                *p = '\0';
                *saveptr = p + 1;
                return start;
            }
            d++;
        }
    }

    /* No trailing delimiter: the token runs to the end of the string. */
    *saveptr = p;   /* points at the terminating NUL */
    return start;
}

/* strtok is the non-reentrant wrapper: the same tokenizer over one file-scope
   saveptr, so the scanning logic lives only in strtok_r. */
static char *g_strtok_save;

char *
strtok(char *str, const char *delim)
{
    return strtok_r(str, delim, &g_strtok_save);
}

/* --- strdup (maize-144) ------------------------------------------------------
 * POSIX strdup: malloc a fresh buffer of strlen(s)+1 and memcpy the string plus
 * its terminating NUL, so the result is an independent, free-able copy. Returns
 * NULL if malloc fails. The one memcpy is a helper call (no open-coded second
 * loop in this body), keeping the pinned qbe-maize backend on its budget. */
char *
strdup(const char *s)
{
    size_t n = strlen(s) + 1u;
    char *p = malloc(n);
    if (p == NULL)
        return NULL;
    memcpy(p, s, n);
    return p;
}

/* --- strerror (maize-172) ----------------------------------------------------
 * A static message per errno code the runtime names (errno.h). The set is small
 * and matched by switch rather than a sparse table; unknown codes get a fixed
 * "Unknown error". The returned pointer is a static string literal (never freed). */
#include "errno.h"

char *
strerror(int errnum)
{
    switch (errnum) {
    case 0:      return "Success";
    case EPERM:  return "Operation not permitted";
    case ENOENT: return "No such file or directory";
    case EBADF:  return "Bad file descriptor";
    case ENOMEM: return "Cannot allocate memory";
    case EISDIR: return "Is a directory";
    case EINVAL: return "Invalid argument";
    case ENOTTY: return "Inappropriate ioctl for device";
    case ERANGE: return "Numerical result out of range";
    default:     return "Unknown error";
    }
}
