/* bulk_noncontig.c -- maize-247 AC fixture (AC 9352), run UNDER quesOS.
 *
 * Proves $F4's contiguity-check -ENOSYS branch: a deliberately non-contiguous single
 * buffer, passed to the raw sys_bulk_copy, returns -ENOSYS (38) -- the exact value the RT
 * memcpy/memmove fall-back gates on (toolchain/rt/string.c: `if (sys_bulk_copy(...) == n)`
 * else the word loop). This closes a real coverage gap: no other fixture reaches the
 * contiguity-check's negative branch, and it makes the rv a DETERMINISTIC path discriminator
 * (no clock, no timing bracket, no CI-noise exposure -- cycle 3 dropped the timing shape).
 *
 * Non-contiguity is constructed via fork-interleaved heap growth, an unconditional
 * consequence of quesOS's global bump allocator (quesos.c alloc_frame / do_fork):
 *
 *   1. Grow the heap ONE page -> page A maps to frame F_A = g_pool_next at that moment.
 *   2. fork(): do_fork's build_address_space allocs a fresh root+L2+L1+L0 (4 frames) AND its
 *      eager-copy walk allocs one fresh frame for EVERY currently-mapped user page of the
 *      parent (the 16-page user stack + the loaded image pages + page A) -- well over a dozen
 *      bump-allocator steps. The burst is synchronous inside the parent's fork syscall, so
 *      g_pool_next is unconditionally advanced far past F_A + PAGE_SIZE before the parent's
 *      next brk. The child just exits (allocates nothing; the bump allocator never frees, so
 *      the advance is monotonic regardless of scheduling order).
 *   3. Grow the heap ONE more page -> page B maps to frame F_B = the now-advanced g_pool_next,
 *      which is NOT F_A + PAGE_SIZE. So [base, base+0x2000) is VIRTUALLY contiguous (two
 *      adjacent heap pages) but PHYSICALLY scattered across two runs.
 *
 * Burn arithmetic: F_B - F_A >= (4 + parent_user_pages) * PAGE_SIZE >= (4 + 17) * 0x1000, so
 * F_B is at least ~21 frames past F_A, never adjacent. sys_bulk_copy over the scattered buffer
 * (src == dst; both ranges validate mapped+user, but neither is a single contiguous run) hits
 * the `!src_contig` branch of do_bulk_copy and returns -ENOSYS.
 * Output on success: bulk-noncontig: PASS
 */
int  printf(const char *, ...);
long sys_brk(unsigned long addr);
long sys_fork(void);
long sys_wait4(long pid, int *status, long options, long rusage);
long sys_bulk_copy(void *dst, const void *src, unsigned long n);

int main(void) {
    unsigned long cur  = (unsigned long)sys_brk(0);
    unsigned long base = (cur + 0xFFFUL) & ~0xFFFUL;
    unsigned char *scattered = (unsigned char *)base;
    long pid, rv;
    int status = 0;

    /* Page A. */
    if (sys_brk(base + 0x1000UL) != (long)(base + 0x1000UL)) {
        printf("bulk-noncontig: FAIL brkA\n");
        return 0;
    }
    scattered[0] = 0x11;                       /* touch page A */

    /* Burn frames between A and B via fork's eager copy. */
    pid = sys_fork();
    if (pid == 0) { return 0; }                /* child: exit immediately, no allocation */
    sys_wait4(pid, &status, 0, 0);             /* reap the child before growing page B */

    /* Page B (now physically far from A). */
    if (sys_brk(base + 0x2000UL) != (long)(base + 0x2000UL)) {
        printf("bulk-noncontig: FAIL brkB\n");
        return 0;
    }
    scattered[0x1000] = 0x22;                  /* touch page B */

    /* Raw $F4 across the scattered two-page buffer: MUST be -ENOSYS (38). */
    rv = sys_bulk_copy(scattered, scattered, 0x2000UL);
    if (rv == -38) { printf("bulk-noncontig: PASS\n"); }
    else { printf("bulk-noncontig: FAIL rv=%d\n", (int)rv); }
    return 0;
}
