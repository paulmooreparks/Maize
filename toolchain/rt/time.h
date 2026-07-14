/* toolchain/rt/time.h -- freestanding <time.h> slice for the Maize C runtime
 * (maize-172).
 *
 * kilo (the terminal forcing-function editor) uses time(NULL) only to timestamp
 * its status message and expire it after a few seconds, so the sole requirement is
 * a monotonic second counter. time() is layered over sys_clock_ms (SYS $F0), the
 * same monotonic millisecond clock DOOM's frame pacing reads, divided to seconds.
 * The epoch is therefore VM start, not the Unix epoch: an honest deviation that is
 * correct for every RELATIVE elapsed-time use (kilo's is one). Calendar time (a
 * real wall-clock epoch) would need a host time-of-day syscall and is out of scope.
 */
#ifndef MAIZE_TIME_H
#define MAIZE_TIME_H

typedef long time_t;

/* Seconds since VM start (monotonic). If t is non-NULL the value is also stored
 * through it, matching the C signature. */
time_t time(time_t *t);

#endif /* MAIZE_TIME_H */
