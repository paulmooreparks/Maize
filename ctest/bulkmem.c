/* maize-216: large-n memcpy/memmove/memset self-check, driving the host bulk
 * syscalls (SYS $F4 sys_bulk_copy / $F5 sys_bulk_set). str.c only ever copies
 * fewer than 8 bytes-to-256 and so exercises the inline word loop; every call
 * here is at or straddling BULK_SYSCALL_THRESHOLD (256), so the syscall path is
 * actually run and checked byte-for-byte.
 *
 * memmove is checked in BOTH overlap directions at large n: sys_bulk_copy stages
 * the whole source through a host buffer before writing the destination, so an
 * overlapping move must still produce the original source bytes. The reference is
 * captured with an open-coded byte loop (never memcpy) so it does not depend on the
 * function under test. Static buffers + runtime indices keep the pinned qbe -t
 * maize backend off the `$sym + K` fold path (see the str.c authoring note).
 *
 * Prints a single "bulkmem PASS" line (or "bulkmem FAIL" on the first failure).
 */
#include "string.h"
#include "stdio.h"

#define N   300u          /* > 256: forces the syscall path */
#define CAP 640u

static unsigned char src[CAP];
static unsigned char dst[CAP];
static unsigned char work[CAP];
static unsigned char snap[CAP];

static int ok = 1;

static void check(int cond) { if (!cond) ok = 0; }

/* pattern byte for index i, deterministic and non-trivial */
static unsigned char pat(unsigned i) { return (unsigned char)(i * 7u + 3u); }

int
main(void)
{
    unsigned i, k;

    /* 1. memcpy non-overlapping, aligned bases, n=300. Bytes past n untouched. */
    for (i = 0; i < CAP; i++) { src[i] = pat(i); dst[i] = 0xEE; }
    memcpy(dst, src, N);
    for (i = 0; i < N; i++)   check(dst[i] == src[i]);
    for (i = N; i < CAP; i++) check(dst[i] == 0xEE);

    /* 2. memcpy unaligned bases (dst+3, src+1), n=300, proving the host copy is
       correct off non-8-aligned starts, not just fast. */
    for (i = 0; i < CAP; i++) { src[i] = pat(i + 11u); dst[i] = 0xEE; }
    memcpy(dst + 3, src + 1, N);
    for (i = 0; i < N; i++) check(dst[3 + i] == src[1 + i]);
    check(dst[2] == 0xEE);
    check(dst[3 + N] == 0xEE);

    /* 3. threshold boundary: n in {255 (inline), 256 (syscall), 257 (syscall)}
       all byte-correct, and the byte just past n stays untouched. */
    {
        unsigned ns[3];
        ns[0] = 255u; ns[1] = 256u; ns[2] = 257u;
        for (k = 0; k < 3u; k++) {
            unsigned n = ns[k];
            for (i = 0; i < CAP; i++) { src[i] = pat(i + n); dst[i] = 0x11; }
            memcpy(dst, src, n);
            for (i = 0; i < n; i++) check(dst[i] == src[i]);
            check(dst[n] == 0x11);
        }
    }

    /* 4. memset large, aligned and unaligned, n=300; bytes outside the span
       untouched. Also n=256 exactly at the threshold. */
    for (i = 0; i < CAP; i++) work[i] = 0xEE;
    memset(work, 0xAB, N);
    for (i = 0; i < N; i++)   check(work[i] == 0xAB);
    for (i = N; i < CAP; i++) check(work[i] == 0xEE);

    for (i = 0; i < CAP; i++) work[i] = 0xEE;
    memset(work + 5, 0xCD, N);
    for (i = 0; i < 5u; i++)      check(work[i] == 0xEE);
    for (i = 5u; i < 5u + N; i++) check(work[i] == 0xCD);
    for (i = 5u + N; i < CAP; i++) check(work[i] == 0xEE);

    for (i = 0; i < CAP; i++) work[i] = 0xEE;
    memset(work, 0x00, 256u);            /* BSS-zero-shaped fill at the boundary */
    for (i = 0; i < 256u; i++) check(work[i] == 0x00);
    check(work[256] == 0xEE);

    /* 5. memmove forward overlap (dst < src), n=300, overlapping and unaligned.
       Reference = original bytes, captured by an open-coded loop. */
    for (i = 0; i < CAP; i++) work[i] = pat(i + 5u);
    for (i = 0; i < CAP; i++) snap[i] = work[i];
    memmove(work + 1, work + 5, N);      /* d < s */
    for (i = 0; i < N; i++) check(work[1 + i] == snap[5 + i]);
    check(work[0] == snap[0]);            /* below dst: untouched */

    /* 6. memmove backward overlap (dst > src), n=300. */
    for (i = 0; i < CAP; i++) work[i] = pat(i + 9u);
    for (i = 0; i < CAP; i++) snap[i] = work[i];
    memmove(work + 5, work + 1, N);      /* d > s */
    for (i = 0; i < N; i++) check(work[5 + i] == snap[1 + i]);
    check(work[0] == snap[0]);            /* below src: untouched */

    /* 7. n == 0 is a no-op that returns dst and touches nothing. */
    for (i = 0; i < CAP; i++) dst[i] = 0x5A;
    check(memcpy(dst, src, 0) == (void *)dst);
    check(memset(dst, 0xFF, 0) == (void *)dst);
    for (i = 0; i < CAP; i++) check(dst[i] == 0x5A);

    puts(ok ? "bulkmem PASS" : "bulkmem FAIL");
    return 0;
}
