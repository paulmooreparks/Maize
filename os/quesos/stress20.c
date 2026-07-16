/* stress20.c -- maize-93 AC6 fixture (20-process stress), run UNDER quesOS.
 *
 * Forks 20 children, each doing a little compute (so the round-robin timer preempts
 * them) and exiting with a distinct status, then reaps all 20 and verifies every
 * status. Completing at all exercises the process table + scheduler under load; the
 * status check proves each of the 20 ran and was reaped correctly.
 */

int printf(const char *, ...);
long sys_fork(void);
long sys_wait4(long pid, int *status, long options, long rusage);

long g_sink;   /* external linkage: keeps the compute loop from being optimized away */

int main(void) {
    long pids[20];
    int i;

    for (i = 0; i < 20; ++i) {
        long p = sys_fork();
        if (p == 0) {
            long k;
            for (k = 0; k < 3000; ++k) { g_sink = g_sink + k + i; }
            return i + 1;
        }
        pids[i] = p;
    }

    int ok = 1;
    for (i = 0; i < 20; ++i) {
        int st = 0;
        long w = sys_wait4(pids[i], &st, 0, 0);
        if (w != pids[i] || ((st >> 8) & 0xFF) != (i + 1)) { ok = 0; }
    }
    printf(ok ? "stress20: PASS\n" : "stress20: FAIL\n");
    return 0;
}
