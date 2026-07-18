/* bulk_forward.c -- maize-247 AC fixture (AC 9348, 9349, 9356 part iii), run UNDER quesOS.
 *
 * Proves quesOS forwards the native bulk-memory accelerators ($F4 sys_bulk_copy, $F5
 * sys_bulk_set) on a physically-contiguous buffer, instead of hard-returning -ENOSYS as it
 * did before this card. The raw return value is the discriminator: no os/quesos fixture
 * before this card allocated a >= 256-byte buffer, so nothing distinguished "forwarded"
 * (rv == n) from "silently fell back to the RT word loop" (both produce correct bytes).
 *
 *  - AC 9348: raw sys_bulk_copy across a >= 3-page (>= 12 KiB) contiguous src/dst filled
 *    with a non-trivial pattern (byte i = (i*167+13) & 0xFF, not constant-fill); rv == n
 *    proves the native $F4 forward executed, and every byte is checked.
 *  - AC 9349: raw sys_bulk_set across a multi-page contiguous dst; rv == n and the whole
 *    range is verified filled.
 *  - AC 9356 (iii): the libc memcpy/memset entrypoints with n >= the 256-byte threshold
 *    produce byte-correct output under quesOS. This is a NECESSARY smoke test only; path
 *    selection is proven by the raw-rv discriminator here and in bulk_noncontig.c, NOT by
 *    correct output alone (correct bytes are also produced by the RT fallback).
 *
 * The three N-byte buffers are grown in ONE sys_brk call, so quesOS's do_brk maps all nine
 * pages in a single uninterrupted alloc_frame loop and they are physically contiguous.
 * Output on success: bulk-forward: PASS
 */
int  printf(const char *, ...);
long sys_brk(unsigned long addr);
long sys_bulk_copy(void *dst, const void *src, unsigned long n);
long sys_bulk_set(void *dst, int c, unsigned long n);
void *memcpy(void *dst, const void *src, unsigned long n);
void *memset(void *dst, int c, unsigned long n);

#define N 12288u   /* 3 pages exactly: >= 12 KiB, well above the 256-byte RT threshold */

int main(void) {
    unsigned long cur  = (unsigned long)sys_brk(0);
    unsigned long base = (cur + 0xFFFUL) & ~0xFFFUL;
    unsigned char *src  = (unsigned char *)base;
    unsigned char *dst  = (unsigned char *)(base + N);
    unsigned char *fill = (unsigned char *)(base + 2UL * N);
    unsigned long i;
    long rv;
    int ok;

    if (sys_brk(base + 3UL * N) != (long)(base + 3UL * N)) {
        printf("bulk-forward: FAIL brk\n");
        return 0;
    }

    /* --- AC 9348: raw sys_bulk_copy, contiguous >= 3-page src/dst, non-trivial pattern --- */
    for (i = 0; i < N; ++i) { src[i] = (unsigned char)((i * 167u + 13u) & 0xFFu); }
    for (i = 0; i < N; ++i) { dst[i] = 0; }
    rv = sys_bulk_copy(dst, src, N);
    ok = (rv == (long)N);
    for (i = 0; i < N && ok; ++i) {
        if (dst[i] != (unsigned char)((i * 167u + 13u) & 0xFFu)) { ok = 0; }
    }
    if (!ok) { printf("bulk-forward: FAIL copy rv=%d\n", (int)rv); return 0; }

    /* --- AC 9349: raw sys_bulk_set, contiguous multi-page dst --- */
    for (i = 0; i < N; ++i) { fill[i] = 0; }
    rv = sys_bulk_set(fill, 0xAB, N);
    ok = (rv == (long)N);
    for (i = 0; i < N && ok; ++i) { if (fill[i] != 0xAB) { ok = 0; } }
    if (!ok) { printf("bulk-forward: FAIL set rv=%d\n", (int)rv); return 0; }

    /* --- AC 9356 (iii): libc memcpy/memset entrypoints, n >= threshold, byte-correct --- */
    for (i = 0; i < N; ++i) { src[i] = (unsigned char)((i * 211u + 7u) & 0xFFu); dst[i] = 0; }
    memcpy(dst, src, N);
    ok = 1;
    for (i = 0; i < N && ok; ++i) {
        if (dst[i] != (unsigned char)((i * 211u + 7u) & 0xFFu)) { ok = 0; }
    }
    memset(fill, 0x5C, N);
    for (i = 0; i < N && ok; ++i) { if (fill[i] != 0x5C) { ok = 0; } }
    if (!ok) { printf("bulk-forward: FAIL libc\n"); return 0; }

    printf("bulk-forward: PASS\n");
    return 0;
}
