/* poll_timeout.c -- maize-238 Family B AC fixture (AC 9191), run UNDER quesOS.
 *
 * Proves poll() timeout semantics:
 *   - poll() with a finite positive timeout and nothing ever ready returns 0 (not
 *     blocking forever, not erroring), revents left 0, at approximately the requested
 *     delay (bracketed via sys_clock_ms, not exact-timing-sensitive);
 *   - poll() with timeout 0 never parks (returns immediately) even with nothing ready.
 * Output on success: poll-timeout: PASS
 */
#include "poll.h"

int  printf(const char *, ...);
long sys_pipe(int *fds);
unsigned long sys_clock_ms(void);

int main(void) {
    int p[2];
    struct pollfd fds[1];
    long r;
    unsigned long t0, dt;

    sys_pipe(p);   /* p[0] read side: empty, writers open -> never POLLIN-ready */
    fds[0].fd = p[0];
    fds[0].events = POLLIN;

    /* timeout 0: immediate return, 0 ready, revents cleared. */
    fds[0].revents = (short)0x7FFF;
    r = poll(fds, 1, 0);
    if (r != 0 || fds[0].revents != 0) { printf("poll-timeout: FAIL nonblock\n"); return 0; }

    /* finite timeout, nothing ready: returns 0 after roughly the delay. */
    fds[0].revents = (short)0x7FFF;
    t0 = sys_clock_ms();
    r = poll(fds, 1, 60);
    dt = sys_clock_ms() - t0;
    if (r != 0 || fds[0].revents != 0) { printf("poll-timeout: FAIL timeout-ret\n"); return 0; }
    if (dt < 25) { printf("poll-timeout: FAIL too-fast\n"); return 0; }

    printf("poll-timeout: PASS\n");
    return 0;
}
