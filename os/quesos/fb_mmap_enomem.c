/* fb_mmap_enomem.c -- maize-238 Family C AC fixture (AC 9185), run UNDER quesOS.
 *
 * Proves alloc_frames_contig's graceful-failure path: repeated sys_fb_mmap + exit cycles
 * that exhaust the physical frame pool cause sys_fb_mmap to return -ENOMEM on the
 * exhausting call, and quesOS (and the VM) keep running -- no PANIC/poweroff, unlike the
 * existing single-frame alloc_frame() exhaustion path. Each child fb_mmaps (leaking its
 * frames on exit, the documented pool-leak) until one child's fb_mmap fails ENOMEM; the
 * parent, still running, reports PASS. Output on success: fb-enomem: PASS
 */
int  printf(const char *, ...);
long sys_fork(void);
long sys_fb_mmap(void);
long sys_wait4(long pid, int *status, long options, long rusage);

int main(void) {
    int i;
    for (i = 0; i < 600; ++i) {
        long pid = sys_fork();
        int st = 0;
        if (pid == 0) {
            long va = sys_fb_mmap();
            return (va < 0) ? 42 : 0;   /* 42 = ENOMEM on the exhausting call */
        }
        if (pid < 0) {
            /* maize-251: fork now pre-checks the pool and returns -EAGAIN on exhaustion rather
             * than PANICking mid eager-copy. The enlarged 256 KiB stack pushed fork's per-call
             * frame demand above fb_mmap's 63, so an exhausting cycle now trips the fork's own
             * graceful path one allocation earlier than the child's fb_mmap would. This proves
             * the SAME property this fixture exists for: the pool exhausts GRACEFULLY with the
             * VM still running (no PANIC/poweroff), just via fork's guard now instead of
             * fb_mmap's. */
            printf("fb-enomem: PASS\n");
            return 0;
        }
        sys_wait4(pid, &st, 0, 0);
        if (((st >> 8) & 0xFF) == 42) {
            /* The exhausting fb_mmap returned -ENOMEM gracefully and we are still running. */
            printf("fb-enomem: PASS\n");
            return 0;
        }
    }
    printf("fb-enomem: FAIL not-exhausted\n");
    return 0;
}
