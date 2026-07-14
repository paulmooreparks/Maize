/* toolchain/rt/time.c -- time() over the monotonic ms clock (maize-172).
 *
 * Compiled through the same cproc -> qbe -t maize -> mazm -c path as the other C
 * runtime modules and linked into every C image (cc-maize.sh RT set). sys_clock_ms
 * (SYS $F0) returns monotonic milliseconds since VM start and is exempt from the
 * errno convention (it never returns -errno), so no __syscall_ret wrapping is needed.
 */
#include "time.h"
#include "syscall.h"

time_t
time(time_t *t)
{
	time_t secs = (time_t)(sys_clock_ms() / 1000UL);
	if (t != 0) {
		*t = secs;
	}
	return secs;
}
