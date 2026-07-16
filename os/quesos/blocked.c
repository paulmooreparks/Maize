/* blocked.c -- maize-93 AC6 fixture (blocked processes consume no slices), run UNDER
 * quesOS.
 *
 * A "blocker" child parks on an empty pipe whose write end the parent holds but never
 * writes; the scheduler only runs RUNNABLE processes, so a BLOCKED process is never
 * scheduled and consumes no slices. The parent reaps a separate worker child (which
 * completes normally while the blocker is parked), THEN writes one byte to release the
 * blocker and reaps it. The blocker returns its success status only if it stayed parked
 * until released and then read exactly the released byte.
 */

int printf(const char *, ...);
long sys_fork(void);
long sys_pipe(int *fds);
long sys_read(long fd, void *buf, long count);
long sys_write(long fd, const void *buf, long count);
long sys_close(long fd);
long sys_wait4(long pid, int *status, long options, long rusage);

long g_sink;

int main(void) {
    int p[2];
    sys_pipe(p);

    long blocker = sys_fork();
    if (blocker == 0) {
        sys_close(p[1]);                 /* blocker holds only the read end */
        char b = 0;
        long n = sys_read(p[0], &b, 1);  /* parks until the parent writes */
        return (n == 1 && b == 'G') ? 42 : 43;
    }

    long worker = sys_fork();
    if (worker == 0) {
        sys_close(p[0]);
        sys_close(p[1]);
        long k;
        for (k = 0; k < 40000; ++k) { g_sink = g_sink + k; }
        return 7;
    }

    /* Reap the worker first: it completes while the blocker is parked (never scheduled). */
    int wst = 0;
    long w = sys_wait4(worker, &wst, 0, 0);
    int ok = (w == worker) && (((wst >> 8) & 0xFF) == 7);

    /* Now release the blocker and reap it. */
    sys_write(p[1], "G", 1);
    int bst = 0;
    long bl = sys_wait4(blocker, &bst, 0, 0);
    ok = ok && (bl == blocker) && (((bst >> 8) & 0xFF) == 42);

    sys_close(p[0]);
    sys_close(p[1]);
    printf(ok ? "blocked-noslice: PASS\n" : "blocked-noslice: FAIL\n");
    return 0;
}
