/* sig_handler.c -- maize-174 AC fixture, run UNDER quesOS.
 *
 * Proves the signal handler-dispatch path end to end (rt_sigaction -> kill -> user
 * trampoline -> rt_sigreturn -> resume), deterministically, with NO console-input
 * timing: a pipe synchronizes so the child installs its SIGINT handler BEFORE the
 * parent delivers the signal.
 *
 *   parent: pipe, fork; close write end; read one byte (blocks until the child is
 *           armed); kill(child, SIGINT); wait4(child); check WIFEXITED && code 42.
 *   child:  close read end; install a SIGINT handler; write "R" (armed); busy-loop
 *           until the handler sets a flag; then _exit(42). The exit code 42 is
 *           reachable ONLY through the handler, so the parent seeing it proves the
 *           handler ran and the interrupted busy-loop resumed correctly.
 *
 * Output on success: "sig-handler: PASS".
 */

int printf(const char *, ...);
long sys_fork(void);
long sys_wait4(long pid, void *status, long options, void *rusage);
long sys_pipe(void *fds);
long sys_read(long fd, void *buf, long count);
long sys_write(long fd, const void *buf, long count);
long sys_close(long fd);
long sys_kill(long pid, long sig);
long sys_rt_sigaction(long sig, const void *act, void *oldact);

#define SIGINT 2

/* External linkage (like preempt.c's g_sink): keeps the busy-loop and the flag store
 * from being optimized away, and is reloaded each loop iteration by the simple backend
 * (cproc-qbe has no volatile-store support and no loop-invariant hoisting). */
long g_caught;
long g_sink;

static void on_sigint(int s) {
    (void)s;
    g_caught = 1;
}

int main(void) {
    int fds[2];
    long child;

    if (sys_pipe(fds) != 0) { printf("sig-handler: FAIL pipe\n"); return 0; }

    child = sys_fork();
    if (child < 0) { printf("sig-handler: FAIL fork\n"); return 0; }

    if (child == 0) {
        /* Child: arm the handler, announce readiness, then spin until it fires. */
        unsigned long act[3];
        long k;
        sys_close(fds[0]);
        act[0] = (unsigned long)&on_sigint;   /* sa_handler */
        act[1] = 0;                            /* sa_mask    */
        act[2] = 0;                            /* sa_flags   */
        if (sys_rt_sigaction(SIGINT, act, 0) != 0) { printf("sig-handler: FAIL sigaction\n"); return 0; }
        sys_write(fds[1], "R", 1);             /* armed */
        for (k = 0; k < 2000000000; ++k) {     /* interrupted by SIGINT, then resumes */
            if (g_caught) { return 42; }        /* reached only via the handler path */
            g_sink = g_sink + k;
        }
        return 7;                              /* signal never arrived (bounded, no hang) */
    }

    /* Parent: wait until the child is armed, then interrupt it and reap. */
    {
        char c = 0;
        unsigned int status = 0xFFFFFFFFu;
        long r;
        sys_close(fds[1]);
        sys_read(fds[0], &c, 1);               /* blocks until the child writes "R" */
        sys_kill(child, SIGINT);
        r = sys_wait4(child, &status, 0, 0);
        if (r != child) { printf("sig-handler: FAIL wait\n"); return 0; }
        if ((status & 0x7Fu) != 0) { printf("sig-handler: FAIL not-exited\n"); return 0; }
        if (((status >> 8) & 0xFFu) != 42u) { printf("sig-handler: FAIL code\n"); return 0; }
        printf("sig-handler: PASS\n");
    }
    return 0;
}
