/* stdin_wake_mixed.c -- maize-313 AC-27 fixture, run UNDER quesOS.
 *
 * The need_ticks spin arm, which no fixture in tree exercises. The scheduler must decide
 * between parking and spinning over the WHOLE process scan rather than on the first match,
 * because a mixed set is the case where a single test on the first hit gives the wrong
 * answer: one process parked on the console can be woken by a device IRQ raised off the CPU
 * thread, and one parked on a poll with a finite deadline cannot, because a parked CPU
 * retires no instructions and the instruction-tick timer stops with it.
 *
 * So the fixture holds both at once. The child polls a never-ready pipe with a finite timeout
 * in a loop, which keeps a finite deadline live for the whole run; the parent reads fd 0. The
 * kernel must spin, and a spinning guest retires instructions, so the instruction-tick pump
 * probes and raises exactly as it does on master and the parent's bytes still arrive. If the
 * scan decided on its first match and parked, the child's deadlines would stop advancing and
 * the run would hang.
 *
 * Output on success: stdin-wake-mixed: PASS <bytes> <polls>
 */
#include "poll.h"

int  printf(const char *, ...);
long sys_fork(void);
long sys_pipe(int *fds);
long sys_read(long fd, void *buf, long count);
long sys_write(long fd, const void *buf, long count);
long sys_wait4(long pid, int *status, long options, long rusage);

int main(void) {
    int p[2], sy[2];
    long pid;
    long got = 0;

    if (sys_pipe(p) != 0) { printf("stdin-wake-mixed: FAIL pipe\n"); return 0; }
    if (sys_pipe(sy) != 0) { printf("stdin-wake-mixed: FAIL pipe2\n"); return 0; }

    pid = sys_fork();
    if (pid == 0) {
        /* The finite-deadline poller. p[0] never becomes readable, because this process holds
         * the only write end and never writes it, so every poll runs its deadline out. It
         * stops when the parent closes the sync pipe, which makes sy[0] readable. */
        struct pollfd fds[2];
        long polls = 0;
        fds[0].fd = p[0];   fds[0].events = POLLIN; fds[0].revents = 0;
        fds[1].fd = sy[0];  fds[1].events = POLLIN; fds[1].revents = 0;
        for (;;) {
            long r = poll(fds, 2, 50);
            if (r > 0 && (fds[1].revents & (POLLIN | POLLHUP)) != 0) { break; }
            ++polls;
            if (polls > 100000) { break; }   /* a runaway is a failure, not a hang */
        }
        return (int)(polls > 0 ? 1 : 0);
    }

    /* The console reader. Three bytes arrive slowly enough that the kernel reaches its idle
     * path between each one, with the child's finite deadline live throughout. */
    while (got < 3) {
        char c = 0;
        long r = sys_read(0, &c, 1);
        if (r != 1) {
            printf("stdin-wake-mixed: FAIL read returned %ld after %ld\n", r, got);
            return 0;
        }
        if (c != (char)('x' + got)) {
            printf("stdin-wake-mixed: FAIL wrong byte at %ld\n", got);
            return 0;
        }
        ++got;
    }

    sys_write(sy[1], "z", 1);   /* release the poller */
    {
        int st = 0;
        long w = sys_wait4(pid, &st, 0, 0);
        int polled = (st >> 8) & 0xFF;
        if (w != pid || polled != 1) {
            printf("stdin-wake-mixed: FAIL poller w=%ld polled=%d\n", w, polled);
            return 0;
        }
    }

    printf("stdin-wake-mixed: PASS %ld 1\n", got);
    return 0;
}
