/* fb_fork_cleanup.c -- maize-236 AC fixture, run UNDER quesOS.
 *
 * Proves fork() does NOT propagate the framebuffer registration (Decision D4): a parent
 * holding a registration forks; the child inherits no slot, so its SYS_fb_release returns
 * -EBADF (nothing to release), while the parent's registration stays claimed and untouched
 * (the parent's own later SYS_fb_release succeeds, proving it still held the slot).
 *
 * Every reachable path prints an explicit marker (no silent exit): the child's result is
 * folded into the parent's single verdict through its exit status, so a child anomaly can
 * never leave the parent to print a false PASS.
 *
 * Output on success:
 *   child: no-slot EBADF
 *   fb-fork: PASS
 */

int printf(const char *, ...);
long sys_fork(void);
long sys_wait4(long pid, int *status, long options, long rusage);
long sys_fb_register(void *base);
long sys_fb_release(void);

static unsigned int fbbuf[64];

int main(void) {
    long slot = sys_fb_register(fbbuf);
    if (slot < 0) { printf("fb-fork: FAIL parent-register\n"); return 0; }

    long pid = sys_fork();
    if (pid == 0) {
        /* Child: fb_slot did not propagate (D4), so a release finds nothing (-EBADF). The
         * exit status carries the verdict to the parent; the print is diagnostic. */
        long r = sys_fb_release();
        if (r == -9) { printf("child: no-slot EBADF\n"); return 0; }
        printf("child: FAIL not-ebadf\n");
        return 7;   /* nonzero flags the anomaly to the parent's wait4 */
    }

    int status = 0;
    if (sys_wait4(pid, &status, 0, 0) != pid) { printf("fb-fork: FAIL wait\n"); return 0; }
    int child_rc = (status >> 8) & 0xFF;

    /* Parent still holds its registration: releasing it now succeeds. PASS requires BOTH
     * the child's -EBADF (child_rc == 0) and the parent's own release. */
    long pr = sys_fb_release();
    if (child_rc == 0 && pr == 0) { printf("fb-fork: PASS\n"); }
    else { printf("fb-fork: FAIL\n"); }
    return 0;
}
