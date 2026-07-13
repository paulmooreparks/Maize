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
 */
#include "unistd.h"

int
usleep(unsigned useconds)
{
    (void)useconds;   /* no sleep: return immediately */
    return 0;
}
