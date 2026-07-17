/* sig_kill.c -- maize-174 AC fixture, run UNDER quesOS.
 *
 * Proves SIGKILL is uncatchable and unblockable (OQ 9014) even against a process that is
 * currently RUNNING a signal handler: a runaway handler that loops forever (in_handler
 * set) must still be killed by SIGKILL. This is the cycle-2 regression guard for the
 * deliver_pending_signal ordering (SIGKILL check precedes the in_handler defer guard).
 *
 *   child: install a SIGINT handler that announces "running" on pipe B and then loops
 *          forever; announce readiness on pipe A; busy-loop awaiting SIGINT.
 *   parent: wait for A; SIGINT the child (its handler runs and wedges, in_handler set);
 *          wait for B (proof the handler is executing); SIGKILL the child; wait4 must
 *          report WIFSIGNALED with WTERMSIG==SIGKILL. Without the ordering fix, SIGKILL
 *          would be deferred forever and this test would hang (caught by the harness
 *          timeout).
 *
 * Output on success: "sig-kill: PASS".
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
#define SIGKILL 9

long g_sink;
static long g_bfd;   /* pipe B write end: the handler signals that it is running */

static void on_int(int s) {
    long k;
    (void)s;
    sys_write(g_bfd, "H", 1);              /* announce: handler is executing */
    for (k = 0; ; ++k) { g_sink = g_sink + k; }   /* runaway handler: never returns */
}

int main(void) {
    int a[2], b[2];
    long child;
    char c = 0;
    unsigned int st = 0;

    if (sys_pipe(a) != 0 || sys_pipe(b) != 0) { printf("sig-kill: FAIL pipe\n"); return 0; }
    child = sys_fork();
    if (child < 0) { printf("sig-kill: FAIL fork\n"); return 0; }

    if (child == 0) {
        unsigned long act[3];
        long k;
        sys_close(a[0]); sys_close(b[0]);
        g_bfd = b[1];
        act[0] = (unsigned long)&on_int; act[1] = 0; act[2] = 0;
        sys_rt_sigaction(SIGINT, act, 0);
        sys_write(a[1], "R", 1);
        for (k = 0; k < 2000000000; ++k) { g_sink = g_sink + k; }   /* await SIGINT */
        return 0;
    }

    sys_close(a[1]); sys_close(b[1]);
    sys_read(a[0], &c, 1);        /* child armed */
    sys_kill(child, SIGINT);      /* handler runs and wedges (in_handler set) */
    sys_read(b[0], &c, 1);        /* proof: the handler is executing */
    sys_kill(child, SIGKILL);     /* must terminate even mid-handler (OQ 9014) */
    sys_wait4(child, &st, 0, 0);
    if ((st & 0x7Fu) != (unsigned)SIGKILL) { printf("sig-kill: FAIL not-sigkill\n"); return 0; }
    printf("sig-kill: PASS\n");
    return 0;
}
