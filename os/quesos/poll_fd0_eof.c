/* poll_fd0_eof.c -- maize-238 Branch A fixture (review cycle 1), run UNDER quesOS on the
 * plain DEFAULT invocation with a host stdin that stays open (no data) and then closes.
 *
 * Proves that a PARKED poll() on fd 0 wakes on console end-of-input, not only on a keystroke:
 * console EOF is poll/select-readable (the POSIX model -- a read then returns 0). With stdin
 * held open and empty, poll(fd0, POLLIN, -1) finds nothing ready and parks; when the host
 * closes stdin the readiness IRQ fires, poll_recheck_all re-evaluates, and the parked call
 * wakes with POLLIN set. The follow-on read returns 0 (EOF).
 * Output on success: poll-fd0-eof: PASS
 */
#include "poll.h"

int  printf(const char *, ...);
long read(int fd, void *buf, unsigned long count);

int main(void) {
    struct pollfd f;
    char c;
    long r, n;

    f.fd = 0; f.events = POLLIN; f.revents = 0;
    r = poll(&f, 1, -1);   /* parks (stdin open, empty); wakes when host stdin reaches EOF */
    if (r < 1 || (f.revents & POLLIN) == 0) {
        printf("poll-fd0-eof: FAIL poll r=%ld rev=%d\n", r, f.revents); return 0;
    }
    n = read(0, &c, 1);    /* console at EOF -> read returns 0 */
    if (n != 0) { printf("poll-fd0-eof: FAIL read n=%ld\n", n); return 0; }

    printf("poll-fd0-eof: PASS\n");
    return 0;
}
