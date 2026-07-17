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
        /* Child: register slot 0 and exit WITHOUT releasing; do_exit must free the slot.
         * The register result rides the exit status so a child that never actually claimed
         * cannot let the parent print a false PASS (the parent would trivially get slot 0). */
        long cs = sys_fb_register(fbbuf);
        return (cs == 0) ? 0 : 7;
    }

    int status = 0;
    if (sys_wait4(pid, &status, 0, 0) != pid) { printf("fb-exit: FAIL wait\n"); return 0; }
    int child_rc = (status >> 8) & 0xFF;

    /* Parent registers after the child was reaped: must reclaim slot 0, which proves
     * do_exit ran the release. PASS requires the child to have actually held slot 0. */
    long s = sys_fb_register(fbbuf);
    if (child_rc == 0 && s == 0) { printf("fb-exit: PASS\n"); }
    else { printf("fb-exit: FAIL\n"); }
    return 0;
}
