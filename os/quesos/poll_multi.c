/* poll_multi.c -- maize-238 Family B AC fixture (AC 9190), run WITH --input=console.
 *
 * A single process polls a pipe read fd, a connected socket fd, and fd 0 (console). A
 * forked peer drives each fd ready one at a time, sequenced over the socket so exactly
 * one fd is ready per poll() call:
 *   - the peer writes the pipe -> poll() reports POLLIN on exactly the pipe fd (0 else);
 *   - the peer writes the socket -> POLLIN on exactly the socket fd (0 else);
 *   - injected console input -> POLLIN on exactly fd 0 (0 else).
 * The peer stays alive (parked reading an ack) through the console leg so the pipe/socket
 * read sides do not report EOF-readable there. Output on success: poll-multi: PASS
 */
#include "poll.h"
#include "sys/socket.h"

int  printf(const char *, ...);
long sys_fork(void);
long sys_pipe(int *fds);
long read(int fd, void *buf, unsigned long count);
long write(int fd, const void *buf, unsigned long count);
long sys_wait4(long pid, int *status, long options, long rusage);

int main(void) {
    int p[2], sv[2];
    long pid, r;
    char c;
    struct pollfd f2[2];
    struct pollfd f3[3];

    sys_pipe(p);
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) { printf("poll-multi: FAIL pair\n"); return 0; }

    pid = sys_fork();
    if (pid == 0) {
        write(p[1], "x", 1);      /* leg 1: pipe becomes readable */
        read(sv[1], &c, 1);       /* wait ack1 */
        write(sv[1], "y", 1);     /* leg 2: parent's socket end becomes readable */
        read(sv[1], &c, 1);       /* ack2: keep p[1]/sv[1] open through the console leg */
        return 0;
    }

    f2[0].fd = p[0];  f2[0].events = POLLIN;
    f2[1].fd = sv[0]; f2[1].events = POLLIN;

    /* Leg 1: pipe. */
    f2[0].revents = 0; f2[1].revents = 0;
    r = poll(f2, 2, -1);
    if (r != 1 || f2[0].revents != POLLIN || f2[1].revents != 0) { printf("poll-multi: FAIL pipe-leg\n"); return 0; }
    read(p[0], &c, 1);
    write(sv[0], "a", 1);         /* ack1 -> peer writes the socket next */

    /* Leg 2: socket. */
    f2[0].revents = 0; f2[1].revents = 0;
    r = poll(f2, 2, -1);
    if (r != 1 || f2[1].revents != POLLIN || f2[0].revents != 0) { printf("poll-multi: FAIL sock-leg\n"); return 0; }
    read(sv[0], &c, 1);

    /* Leg 3: console (fd 0). Peer still parked reading ack2, so p[0]/sv[0] are not EOF. */
    f3[0].fd = p[0];  f3[0].events = POLLIN;
    f3[1].fd = sv[0]; f3[1].events = POLLIN;
    f3[2].fd = 0;     f3[2].events = POLLIN;
    f3[0].revents = 0; f3[1].revents = 0; f3[2].revents = 0;
    r = poll(f3, 3, -1);   /* blocks until the injected console byte arrives */
    if (f3[2].revents != POLLIN || f3[0].revents != 0 || f3[1].revents != 0) {
        printf("poll-multi: FAIL console-leg\n"); return 0;
    }

    printf("poll-multi: PASS\n");
    write(sv[0], "z", 1);         /* ack2: release the peer */
    { int st; sys_wait4(pid, &st, 0, 0); }
    return 0;
}
