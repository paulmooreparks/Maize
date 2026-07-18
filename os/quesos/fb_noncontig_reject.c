/* fb_noncontig_reject.c -- maize-238 Family C AC fixture (AC 9184), run UNDER quesOS.
 *
 * Proves the hardened do_fb_register rejects a deliberately non-contiguous buffer with
 * -EINVAL. Contiguity is normally an unstated side effect of the bump allocator's
 * atomicity, so we force a discontinuity: grow the heap one page (buffer page 0 -> frame
 * F), interleave an UNRELATED allocation (sys_fb_mmap grabs ~63 contiguous frames), then
 * grow the heap one more page (buffer page 1 -> a frame far past F). The two-page heap
 * buffer is now virtually contiguous but physically scattered, so register must return
 * -EINVAL rather than silently scanning out wrong memory beyond page 0.
 * Output on success: fb-noncontig: PASS
 */
int  printf(const char *, ...);
long sys_brk(unsigned long addr);
long sys_fb_mmap(void);
long sys_fb_register(void *base);

int main(void) {
    unsigned long cur, base;
    long va, s;

    cur = (unsigned long)sys_brk(0);            /* current heap break (page-aligned) */
    base = (cur + 0xFFFUL) & ~0xFFFUL;

    sys_brk(base + 0x1000UL);                   /* map buffer page 0 -> frame F (brk maps it) */

    va = sys_fb_mmap();                          /* unrelated allocation: ~63 frames between */
    if (va < 0) { printf("fb-noncontig: FAIL mmap\n"); return 0; }

    sys_brk(base + 0x2000UL);                    /* map buffer page 1 -> a far, non-adjacent frame */

    s = sys_fb_register((void *)base);           /* pages 0 and 1 mapped but non-contiguous */
    if (s == -22) { printf("fb-noncontig: PASS\n"); }   /* -EINVAL */
    else { printf("fb-noncontig: FAIL not-einval\n"); }
    return 0;
}
