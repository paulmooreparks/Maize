/* poll_broken.c -- maize-238 Family B AC fixture (AC 9193), run UNDER quesOS.
 *
 * Proves broken-pipe poll semantics: polling the write side of a pipe whose read end has
 * been closed reports POLLERR (not POLLOUT), and a subsequent write() on that fd then
 * fails -EPIPE, consistent with what the poll result indicated. Output on success:
 *   poll-broken: PASS
 */
#include "poll.h"

int  printf(const char *, ...);
long sys_pipe(int *fds);
long write(int fd, const void *buf, unsigned long count);
int  close(int fd);

int main(void) {
    int p[2];
    struct pollfd fds[1];
    long r, w;

    sys_pipe(p);       /* p[0] read, p[1] write */
    close(p[0]);       /* drop the read end: p[1]'s peer is now gone */

    fds[0].fd = p[1];
    fds[0].events = POLLOUT;
    fds[0].revents = 0;
    r = poll(fds, 1, 0);
    if (r < 1 || (fds[0].revents & POLLERR) == 0 || (fds[0].revents & POLLOUT) != 0) {
        printf("poll-broken: FAIL revents\n");
        return 0;
    }

    w = write(p[1], "x", 1);   /* consistent with POLLERR: the write fails -EPIPE */
    if (w >= 0) { printf("poll-broken: FAIL not-epipe\n"); return 0; }

    printf("poll-broken: PASS\n");
    return 0;
}
