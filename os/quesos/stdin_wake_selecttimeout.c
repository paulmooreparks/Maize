/* stdin_wake_selecttimeout.c -- maize-313 AC-11 fixture, run UNDER quesOS.
 *
 * The regression the park-versus-spin split exists to prevent, and one that is invisible to
 * every other fixture in tree. A LONE process parks on select(fd 0, timeout 500 ms) with
 * nothing ever arriving, which makes it the only blocked process and gives the scheduler a
 * set whose sole member carries a finite deadline. That deadline is served by the
 * instruction-tick timer, and a parked CPU retires no instructions, so the timer stops with
 * it: a kernel that parked here unconditionally would never return from the select.
 *
 * The green run alone proves nothing, because a design that never parks at all would also
 * pass it. The negative control is a build whose idle path parks unconditionally, on which
 * this fixture hangs to its timeout.
 *
 * Output on success: stdin-wake-selecttimeout: PASS <elapsed_ms>
 */
#include "sys/select.h"

int  printf(const char *, ...);
long read(int fd, void *buf, unsigned long count);
unsigned long sys_clock_ms(void);

int main(void) {
    fd_set rf;
    struct timeval tv;
    long r;
    unsigned long t0, dt;

    FD_ZERO(&rf);
    FD_SET(0, &rf);
    tv.tv_sec = 0;
    tv.tv_usec = 500000;

    t0 = sys_clock_ms();
    r = select(1, &rf, 0, 0, &tv);
    dt = sys_clock_ms() - t0;

    if (r != 0) {
        printf("stdin-wake-selecttimeout: FAIL select returned %ld\n", r);
        return 0;
    }
    if (FD_ISSET(0, &rf)) {
        printf("stdin-wake-selecttimeout: FAIL fd 0 reported ready\n");
        return 0;
    }
    /* Bracketed rather than exact: the deadline sweep runs on the instruction tick, so the
     * return is on schedule rather than on the millisecond. Too FAST is the failure that
     * matters here, because it would mean the select never really waited. */
    if (dt < 250) {
        printf("stdin-wake-selecttimeout: FAIL too fast (%lu ms)\n", dt);
        return 0;
    }
    printf("stdin-wake-selecttimeout: PASS %lu\n", dt);
    return 0;
}
