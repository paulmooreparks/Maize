/* toolchain/rt/unistd.c -- freestanding <unistd.h> slice for the Maize C runtime
 * (maize-148).
 *
 * Scoped to usleep only (decision 8445); the read/write/close/lseek descriptor
 * wrappers live in errno.c over syscall.h and are NOT re-declared here.
 *
 * usleep is a no-op stub returning 0 (decision 8441): it does not sleep. Frame pacing
 * is therefore not honored (max framerate), acceptable for DOOM Phase A/B bring-up. A
 * busy-wait on sys_clock_ms ($F0) was rejected: its millisecond resolution cannot honor
 * a sub-millisecond usec and it burns CPU in DOOM's frame-pacing loops. A real pacing
 * usleep is a follow-up if Phase B timing needs it.
 *
 * isatty / ftruncate joined here for the kilo editor port (maize-172). isatty asks the
 * termios syscall whether the fd is a console tty (the classic tcgetattr-based probe);
 * ftruncate (maize-179) is now a real syscall (SYS $4D) over the confined hostfs
 * backend, so a shrink truncates the file exactly (kilo's save-after-shrink no longer
 * leaves a stale tail).
 */
#include "unistd.h"
#include "termios.h"
#include "errno.h"
#include "syscall.h"

int
usleep(unsigned useconds)
{
    (void)useconds;   /* no sleep: return immediately */
    return 0;
}

int
isatty(int fd)
{
    struct termios t;
    /* tcgetattr succeeds (returns 0) only when a window console is bound to fd 0/1/2;
       with host stdio or a file it returns -1 / EBADF. That success is exactly the
       "fd is a terminal" predicate isatty reports. */
    if (tcgetattr(fd, &t) == 0) {
        return 1;
    }
    errno = ENOTTY;
    return 0;
}

int
ftruncate(int fd, long length)
{
    /* maize-179: real truncate over SYS $4D (confined hostfs backend). Sets the file to
       exactly `length` (a shrink drops the tail, an extend zero-fills). The raw stub
       returns 0 or a [-4095, -1] -errno; __syscall_ret turns the error band into
       errno + -1, so a shrink-save (kilo) is now byte-exact with no stale tail. */
    return (int)__syscall_ret((unsigned long)sys_ftruncate(fd, length));
}
