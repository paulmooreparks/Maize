/* sig_chld.c -- maize-174 AC fixture, run UNDER quesOS.
 *
 * Proves SIGCHLD delivery is independent of an active wait4, and that its default action
 * is ignore:
 *   case A: the parent installs a SIGCHLD handler, does NOT block in wait4, and forks a
 *           child that exits immediately; the handler runs (flag set) with no wait4 call,
 *           proving delivery is not gated on wait4. The parent then reaps the zombie.
 *   case B: the parent installs NO SIGCHLD handler and does not block in wait4; the child
 *           exits; the parent is undisturbed (default-ignore) and reaps afterward.
 *
 * Output on success: "sig-chld: PASS".
 */

int printf(const char *, ...);
long sys_fork(void);
long sys_wait4(long pid, void *status, long options, void *rusage);
long sys_kill(long pid, long sig);
long sys_rt_sigaction(long sig, const void *act, void *oldact);

#define SIGCHLD 17

long g_chld;
long g_sink;

static void on_chld(int s) { (void)s; g_chld = 1; }

int main(void) {
    /* case A: handler runs without any wait4. */
    {
        unsigned long act[3];
        long child, k;
        unsigned int st = 0;
        act[0] = (unsigned long)&on_chld; act[1] = 0; act[2] = 0;
        if (sys_rt_sigaction(SIGCHLD, act, 0) != 0) { printf("sig-chld: FAIL sigaction\n"); return 0; }
        child = sys_fork();
        if (child < 0) { printf("sig-chld: FAIL forkA\n"); return 0; }
        if (child == 0) { return 0; }        /* child exits immediately */
        for (k = 0; k < 2000000000; ++k) {   /* NO wait4 here: only the handler ends this */
            if (g_chld) { break; }
            g_sink = g_sink + k;
        }
        if (!g_chld) { printf("sig-chld: FAIL no-handler\n"); return 0; }
        if (sys_wait4(child, &st, 0, 0) != child) { printf("sig-chld: FAIL reapA\n"); return 0; }
    }

    /* case B: default-ignore -- no handler installed, parent undisturbed, reaps later. */
    {
        unsigned long act[3];
        long child, k;
        unsigned int st = 0;
        act[0] = 0 /* SIG_DFL */; act[1] = 0; act[2] = 0;
        sys_rt_sigaction(SIGCHLD, act, 0);   /* restore default (ignore) */
        g_chld = 0;
        child = sys_fork();
        if (child < 0) { printf("sig-chld: FAIL forkB\n"); return 0; }
        if (child == 0) { return 0; }
        for (k = 0; k < 4000000; ++k) { g_sink = g_sink + k; }   /* run undisturbed */
        if (g_chld != 0) { printf("sig-chld: FAIL spurious\n"); return 0; }
        if (sys_wait4(child, &st, 0, 0) != child) { printf("sig-chld: FAIL reapB\n"); return 0; }
        if ((st & 0x7Fu) != 0 || ((st >> 8) & 0xFFu) != 0) { printf("sig-chld: FAIL statusB\n"); return 0; }
    }

    printf("sig-chld: PASS\n");
    return 0;
}
