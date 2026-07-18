/* select_console_pipe.c -- maize-238 Family B AC fixture (AC 9192), run WITH --input=console.
 *
 * select() with fd 0 and a pipe read fd both in readfds, none initially ready, blocks;
 * injecting console input makes FD_ISSET(0, &readfds) true after select() returns with the
 * pipe bit clear; a subsequent call proves the pipe-write case symmetrically (the peer
 * writes the pipe -> FD_ISSET(pipe) true, fd 0 clear once its byte is drained).
 * Output on success: select-console-pipe: PASS
 */
#include "sys/select.h"
#include "sys/socket.h"

int  printf(const char *, ...);
long sys_fork(void);
long sys_pipe(int *fds);
long read(int fd, void *buf, unsigned long count);
long write(int fd, const void *buf, unsigned long count);
long sys_wait4(long pid, int *status, long options, long rusage);

int main(void) {
    int p[2], sy[2];
    long pid, r;
    char c;
    fd_set rf;
    int nfds;

    sys_pipe(p);
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sy) != 0) { printf("select-console-pipe: FAIL pair\n"); return 0; }
    nfds = (p[0] > sy[0] ? p[0] : sy[0]) + 1;
    if (nfds < 1) { nfds = 1; }

    pid = sys_fork();
    if (pid == 0) {
        read(sy[1], &c, 1);       /* ack1: parent finished the console leg */
        write(p[1], "x", 1);      /* make the pipe readable for the pipe leg */
        read(sy[1], &c, 1);       /* ack2: stay alive */
        return 0;
    }

    /* Console leg: fd 0 becomes ready via injected input; the pipe bit stays clear. */
    FD_ZERO(&rf); FD_SET(0, &rf); FD_SET(p[0], &rf);
    r = select(nfds, &rf, 0, 0, 0);   /* blocks until the console byte arrives */
    if (r < 1 || !FD_ISSET(0, &rf) || FD_ISSET(p[0], &rf)) {
        printf("select-console-pipe: FAIL console-leg\n"); return 0;
    }
    read(0, &c, 1);                   /* drain the console byte */
    write(sy[0], "a", 1);             /* ack1 -> peer writes the pipe */

    /* Pipe leg: the pipe read fd becomes ready. fd 0 is NOT in this set: once its byte is
     * drained the piped console reaches EOF, and console EOF is now poll/select-readable
     * (review cycle 1, the POSIX model -- a read returns 0), so a program still watching fd 0
     * here would legitimately see it ready on EOF. This leg proves the pipe-write wake in
     * isolation; the poll_fd0_eof fixture proves console EOF is itself readable. */
    FD_ZERO(&rf); FD_SET(p[0], &rf);
    r = select(nfds, &rf, 0, 0, 0);
    if (r < 1 || !FD_ISSET(p[0], &rf)) {
        printf("select-console-pipe: FAIL pipe-leg\n"); return 0;
    }

    printf("select-console-pipe: PASS\n");
    write(sy[0], "z", 1);
    { int st; sys_wait4(pid, &st, 0, 0); }
    return 0;
}
