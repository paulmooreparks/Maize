/* pipe_roundtrip.c -- maize-93 AC3 fixture, run UNDER quesOS.
 *
 * Proves pipe + dup2 with blocking semantics in BOTH directions. The parent sends
 * "ping" to the child over pipe p2c; the child upper-cases it and sends "PING" back
 * over pipe c2p, but writes it via fd 1 after dup2'ing stdout onto the pipe's write end
 * (so this also proves dup2 redirection of stdout to a pipe). The parent's read on c2p
 * blocks until the child writes (empty-pipe read parks), and returns EOF (0) once the
 * child exits and its write end closes. Output on success:
 *   pipe-roundtrip: PASS
 */

int printf(const char *, ...);
long sys_fork(void);
long sys_pipe(int *fds);
long sys_read(long fd, void *buf, long count);
long sys_write(long fd, const void *buf, long count);
long sys_close(long fd);
long sys_dup2(long oldfd, long newfd);
long sys_wait4(long pid, int *status, long options, long rusage);

int main(void) {
    int p2c[2];   /* parent -> child */
    int c2p[2];   /* child  -> parent */
    sys_pipe(p2c);
    sys_pipe(c2p);

    long pid = sys_fork();
    if (pid == 0) {
        sys_close(p2c[1]);
        sys_close(c2p[0]);
        char buf[16];
        long n = sys_read(p2c[0], buf, sizeof buf);   /* has data from the parent */
        long i;
        for (i = 0; i < n; ++i) {
            if (buf[i] >= 'a' && buf[i] <= 'z') { buf[i] = (char)(buf[i] - 32); }
        }
        sys_dup2(c2p[1], 1);          /* redirect stdout onto the reply pipe */
        sys_write(1, buf, n);         /* write to fd 1 (now the pipe) */
        return 0;                     /* exit closes the write end -> EOF for the parent */
    }

    sys_close(p2c[0]);
    sys_close(c2p[1]);
    sys_write(p2c[1], "ping", 4);
    sys_close(p2c[1]);                /* EOF for the child's read */

    char rep[16];
    long total = 0;
    for (;;) {
        long n = sys_read(c2p[0], rep + total, (long)sizeof rep - total);  /* blocks */
        if (n <= 0) { break; }
        total += n;
    }

    int ok = (total == 4)
             && rep[0] == 'P' && rep[1] == 'I' && rep[2] == 'N' && rep[3] == 'G';
    printf(ok ? "pipe-roundtrip: PASS\n" : "pipe-roundtrip: FAIL\n");

    int st = 0;
    sys_wait4(pid, &st, 0, 0);
    return 0;
}
