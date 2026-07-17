/* fb_exit_cleanup.c -- maize-236 AC fixture, run UNDER quesOS.
 *
 * Proves do_exit() releases a held registration: a child registers a framebuffer (slot 0)
 * and exits WITHOUT calling SYS_fb_release; after reaping it, the parent registers and must
 * get slot 0 again. If quesOS's do_exit path did not run the release, slot 0 would stay
 * claimed on the device and the parent's free-slot scan would land elsewhere; getting slot
 * 0 proves the exit cleanup ran (not merely that the process died).
 *
 * Output on success: "fb-exit: PASS".
 */

int printf(const char *, ...);
long sys_fork(void);
long sys_wait4(long pid, int *status, long options, long rusage);
long sys_fb_register(void *base);

static unsigned int fbbuf[64];

int main(void) {
    long pid = sys_fork();
    if (pid == 0) {
        /* Child: register and exit without releasing; do_exit must free the slot. */
        sys_fb_register(fbbuf);
        return 0;
    }

    int status = 0;
    sys_wait4(pid, &status, 0, 0);

    /* Parent registers after the child was reaped: must reclaim slot 0. */
    long s = sys_fb_register(fbbuf);
    if (s == 0) { printf("fb-exit: PASS\n"); }
    else { printf("fb-exit: FAIL got-slot\n"); }
    return 0;
}
