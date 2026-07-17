/* fb_fork_cleanup.c -- maize-236 AC fixture, run UNDER quesOS.
 *
 * Proves fork() does NOT propagate the framebuffer registration (Decision D4): a parent
 * holding a registration forks; the child inherits no slot, so its SYS_fb_release returns
 * -EBADF (nothing to release), while the parent's registration stays claimed and untouched
 * (the parent's own later SYS_fb_release succeeds, proving it still held the slot).
 *
 * Output on success (child line first, then the parent verdict):
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
        /* Child: fb_slot did not propagate, so a release finds nothing. */
        long r = sys_fb_release();
        if (r == -9) { printf("child: no-slot EBADF\n"); }   /* -EBADF */
        else { printf("child: FAIL not-ebadf\n"); }
        return 0;
    }

    int status = 0;
    sys_wait4(pid, &status, 0, 0);

    /* Parent still holds its registration: releasing it now succeeds. */
    long pr = sys_fb_release();
    if (pr == 0) { printf("fb-fork: PASS\n"); }
    else { printf("fb-fork: FAIL parent-release\n"); }
    return 0;
}
