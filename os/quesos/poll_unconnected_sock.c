/* poll_unconnected_sock.c -- maize-238 Family B guard fixture, run UNDER quesOS.
 *
 * Regression for the fd_ready() OOB (Convention counterexamples, Entry 9): a freshly
 * socket()'d AF_UNIX SOCK_STREAM fd is UNCONNECTED (pipe_idx == peer_idx == -1). Adding it
 * to a poll()/select() set once made the kernel index g_pipe[-1], a guest-reachable
 * out-of-bounds read of the static ring pool. This is the only fixture that polls an
 * unconnected socket (every other socket test connects first, which is why the OOB stayed
 * invisible, ASan included). With the connectedness guard, an unconnected socket reports
 * ready for NOTHING: poll(timeout=0) returns 0 with revents 0, and select(timeout={0,0})
 * returns 0 with the fd bits clear. No crash, no spurious readiness.
 * Output on success: poll-unconn-sock: PASS
 */
#include "poll.h"
#include "sys/select.h"
#include "sys/socket.h"

int printf(const char *, ...);

int main(void) {
    int s;
    long r;
    struct pollfd f;

    s = (int)socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0) { printf("poll-unconn-sock: FAIL socket\n"); return 0; }

    /* poll() a non-blocking (timeout 0) set containing ONLY the unconnected socket: it must
     * return 0 (nothing ready) with revents cleared, having safely skipped the -1 rings. */
    f.fd = s; f.events = POLLIN | POLLOUT; f.revents = 0;
    r = poll(&f, 1, 0);
    if (r != 0 || f.revents != 0) { printf("poll-unconn-sock: FAIL poll r=%ld rev=%d\n", r, f.revents); return 0; }

    /* select() the same fd in both readfds and writefds with a {0,0} (non-blocking) timeout:
     * must return 0 with both bits clear. */
    {
        fd_set rf, wf;
        struct timeval tv;
        tv.tv_sec = 0; tv.tv_usec = 0;
        FD_ZERO(&rf); FD_ZERO(&wf); FD_SET(s, &rf); FD_SET(s, &wf);
        r = select(s + 1, &rf, &wf, 0, &tv);
        if (r != 0 || FD_ISSET(s, &rf) || FD_ISSET(s, &wf)) {
            printf("poll-unconn-sock: FAIL select r=%ld\n", r); return 0;
        }
    }

    printf("poll-unconn-sock: PASS\n");
    return 0;
}
