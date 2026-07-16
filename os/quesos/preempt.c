/* preempt.c -- maize-93 AC6 fixture (compute-bound child does not starve), run UNDER
 * quesOS.
 *
 * The parent forks a COMPUTE-BOUND child (a long loop with no syscalls) and then a
 * QUICK child that exits immediately, and reaps whichever finishes first. Under
 * timer-bounded round robin the quick child runs and exits while the compute-bound
 * child is repeatedly preempted, so the first child reaped is the QUICK one. Under
 * cooperative-only scheduling the compute-bound child (forked first, scheduled first
 * when the parent blocks) would monopolize the CPU to completion and be reaped first.
 * So "first reaped == the quick child" is exactly the no-starvation property.
 */

int printf(const char *, ...);
long sys_fork(void);
long sys_wait4(long pid, int *status, long options, long rusage);

long g_sink;   /* external linkage: keeps the compute loop from being optimized away */

int main(void) {
    long slow = sys_fork();
    if (slow == 0) {
        long k;
        for (k = 0; k < 300000; ++k) { g_sink = g_sink + k; }   /* compute-bound */
        return 1;
    }
    long quick = sys_fork();
    if (quick == 0) {
        return 2;   /* exits immediately */
    }

    int st = 0;
    long first = sys_wait4(-1, &st, 0, 0);          /* the first child to finish */
    int ok = (first == quick) && (((st >> 8) & 0xFF) == 2);

    int st2 = 0;
    sys_wait4(-1, &st2, 0, 0);                        /* reap the slow one too */

    printf(ok ? "preempt: PASS\n" : "preempt: FAIL\n");
    return 0;
}
