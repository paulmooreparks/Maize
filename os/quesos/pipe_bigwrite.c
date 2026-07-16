/* pipe_bigwrite.c -- maize-93 AC3 fixture (full-pipe write parks), run UNDER quesOS.
 *
 * Completes the AC3 blocking semantics: the parent writes 10000 bytes through a pipe
 * whose kernel ring holds only 4096, so once the ring fills the parent's write PARKS
 * until the child drains enough to make space; the child, reading in small chunks,
 * repeatedly wakes the parked writer. The child verifies every byte matches the ramp
 * pattern and that exactly 10000 bytes arrived. Output on success:
 *   pipe-bigwrite: PASS
 */

int printf(const char *, ...);
long sys_fork(void);
long sys_pipe(int *fds);
long sys_read(long fd, void *buf, long count);
long sys_write(long fd, const void *buf, long count);
long sys_close(long fd);
long sys_wait4(long pid, int *status, long options, long rusage);

#define TOTAL 10000

static char wbuf[TOTAL];

int main(void) {
    int fds[2];
    sys_pipe(fds);

    long pid = sys_fork();
    if (pid == 0) {
        sys_close(fds[1]);            /* child reads only */
        char rbuf[512];
        long got = 0;
        int ok = 1;
        for (;;) {
            long n = sys_read(fds[0], rbuf, (long)sizeof rbuf);
            long i;
            if (n <= 0) { break; }
            for (i = 0; i < n; ++i) {
                if ((unsigned char)rbuf[i] != (unsigned char)((got + i) & 0xFF)) { ok = 0; }
            }
            got += n;
        }
        printf((ok && got == TOTAL) ? "pipe-bigwrite: PASS\n" : "pipe-bigwrite: FAIL\n");
        return 0;
    }

    sys_close(fds[0]);                /* parent writes only */
    long i;
    for (i = 0; i < TOTAL; ++i) { wbuf[i] = (char)(i & 0xFF); }
    long written = 0;
    while (written < TOTAL) {
        long n = sys_write(fds[1], wbuf + written, TOTAL - written);   /* parks when full */
        if (n <= 0) { break; }
        written += n;
    }
    sys_close(fds[1]);                /* EOF for the child */

    int st = 0;
    sys_wait4(pid, &st, 0, 0);
    return 0;
}
