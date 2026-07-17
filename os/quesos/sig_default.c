/* sig_default.c -- maize-174 AC fixture, run UNDER quesOS.
 *
 * Proves default-action terminate + SIG_IGN survival + WIFSIGNALED, deterministically
 * (pipe-synced fork, direct kill, no console timing):
 *   case 1: a handler-less child killed with SIGINT terminates with WIFSIGNALED and
 *           WTERMSIG == SIGINT, with NO explicit exit call (default-terminate).
 *   case 2: same for SIGQUIT (WTERMSIG == SIGQUIT).
 *   case 3: a child that sigaction(SIGQUIT, SIG_IGN)'d survives that signal and reaches
 *           a NORMAL exit (WIFEXITED, code 55): proof the signal was ignored, not fatal.
 *
 * Output on success: "sig-default: PASS".
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

#define SIGINT  2
#define SIGQUIT 3
#define SIG_IGN 1

long g_sink;

/* Default-terminate case: a handler-less child announces readiness, busy-loops; the
 * parent kills it with `sig` and returns the wait status. */
static int term_case(int sig, unsigned int *out_status) {
    int fds[2];
    long child;
    char c = 0;
    if (sys_pipe(fds) != 0) { return -1; }
    child = sys_fork();
    if (child < 0) { return -1; }
    if (child == 0) {
        long k;
        sys_close(fds[0]);
        sys_write(fds[1], "R", 1);
        for (k = 0; k < 2000000000; ++k) { g_sink = g_sink + k; }
        return 0;   /* not reached: the signal terminates the loop */
    }
    sys_close(fds[1]);
    sys_read(fds[0], &c, 1);
    sys_kill(child, sig);
    sys_wait4(child, out_status, 0, 0);
    sys_close(fds[0]);
    return 0;
}

int main(void) {
    unsigned int st = 0;

    if (term_case(SIGINT, &st) != 0) { printf("sig-default: FAIL c1-run\n"); return 0; }
    if ((st & 0x7Fu) != (unsigned)SIGINT) { printf("sig-default: FAIL c1-termsig\n"); return 0; }

    st = 0;
    if (term_case(SIGQUIT, &st) != 0) { printf("sig-default: FAIL c2-run\n"); return 0; }
    if ((st & 0x7Fu) != (unsigned)SIGQUIT) { printf("sig-default: FAIL c2-termsig\n"); return 0; }

    /* case 3: SIG_IGN survival. Two pipes: pipe A announces readiness, pipe B releases
     * the child to exit. The child ignores SIGQUIT and blocks reading B; the parent sends
     * SIGQUIT (which, if not ignored, would terminate the child before it reads B), then
     * releases it via B. A normal exit 55 proves the signal was ignored. */
    {
        int a[2], b[2];
        long child;
        char c = 0;
        unsigned int s3 = 0;
        if (sys_pipe(a) != 0 || sys_pipe(b) != 0) { printf("sig-default: FAIL c3-pipe\n"); return 0; }
        child = sys_fork();
        if (child < 0) { printf("sig-default: FAIL c3-fork\n"); return 0; }
        if (child == 0) {
            unsigned long act[3];
            char g = 0;
            sys_close(a[0]); sys_close(b[1]);
            act[0] = (unsigned long)SIG_IGN; act[1] = 0; act[2] = 0;
            sys_rt_sigaction(SIGQUIT, act, 0);
            sys_write(a[1], "R", 1);          /* armed */
            sys_read(b[0], &g, 1);            /* blocks until released; SIGQUIT is ignored */
            return 55;                        /* normal exit: proof of survival */
        }
        sys_close(a[1]); sys_close(b[0]);
        sys_read(a[0], &c, 1);               /* wait until the child armed SIG_IGN */
        sys_kill(child, SIGQUIT);            /* ignored: the child must not die here */
        sys_write(b[1], "G", 1);             /* release the child to exit normally */
        sys_wait4(child, &s3, 0, 0);
        if ((s3 & 0x7Fu) != 0) { printf("sig-default: FAIL c3-signaled\n"); return 0; }
        if (((s3 >> 8) & 0xFFu) != 55u) { printf("sig-default: FAIL c3-code\n"); return 0; }
    }

    printf("sig-default: PASS\n");
    return 0;
}
