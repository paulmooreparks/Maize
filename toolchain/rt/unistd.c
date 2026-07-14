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
 * ftruncate is a documented PARTIAL: with no truncate syscall yet it does not resize
 * the file. See the unistd.h header note and the maize-172 follow-up card.
 */
#include "unistd.h"
#include "termios.h"
#include "errno.h"

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
    /* PARTIAL (maize-172): Maize has no truncate syscall yet (SYSCALL-ABI.md reserves
       it), so the file is NOT resized. Returning 0 lets a full-rewrite save (kilo)
       succeed when the new content is at least as long as the old file; a shrink leaves
       a stale tail. The real truncate syscall is tracked as a follow-up card. */
    (void)fd;
    (void)length;
    return 0;
}
