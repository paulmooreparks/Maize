/* maize-141: monotonic millisecond clock self-check over the raw sys_clock_ms
 * (SYS $F0) stub. Prints exactly "clock: PASS" iff the clock is sane, else a
 * single FAIL line. Checks three invariants, none of which prints the raw ms
 * value (which is nondeterministic):
 *   1. non-decreasing at fine grain (two back-to-back reads: b >= a),
 *   2. advance-under-load (spin reading the clock until it moves, bounded by a
 *      safety cap so a stuck clock cannot hang the test),
 *   3. plausibility (the observed delta is nonzero and < 60000 ms, catching a
 *      ns- or seconds-scale unit bug), plus non-decreasing across the whole run. */
#include "syscall.h"
#include "stdio.h"

int
main(void)
{
    unsigned long a = sys_clock_ms();
    unsigned long b = sys_clock_ms();
    if (b < a) { puts("clock: FAIL fine-grain"); return 1; }   /* non-decreasing at fine grain */

    /* Advance-under-load: spin reading the clock until it moves, bounded by a
       safety cap so a stuck clock cannot hang the test. The Maize interpreter is
       slow enough that the clock crosses a 1 ms boundary in far fewer than CAP
       reads. */
    unsigned long start = sys_clock_ms();
    unsigned long now = start, spins = 0;
    const unsigned long CAP = 2000000000UL;
    while (now == start && spins < CAP) { now = sys_clock_ms(); spins++; }

    unsigned long delta = now - start;
    if (delta == 0)      { puts("clock: FAIL stuck");   return 1; }   /* never advanced within CAP */
    if (delta > 60000UL) { puts("clock: FAIL scale");   return 1; }   /* unit/scale bug (ns, or s*1000) */

    if (sys_clock_ms() < a) { puts("clock: FAIL monotonic"); return 1; } /* non-decreasing across the run */

    puts("clock: PASS");
    return 0;
}
