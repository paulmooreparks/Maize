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
typedef long clock_t;

/* Seconds since VM start (monotonic). If t is non-NULL the value is also stored
 * through it, matching the C signature. */
time_t time(time_t *t);

/* maize-94: struct timespec + clock_gettime, for borrowed oksh's mail-check and
 * `time`-builtin elapsed-time bookkeeping (mail.c / jobs.c use CLOCK_MONOTONIC with
 * timespecsub). clock_gettime is layered over the same sys_clock_ms monotonic
 * millisecond counter time() reads: tv_sec = ms/1000, tv_nsec = (ms%1000)*1e6. The
 * epoch is VM start (honest deviation, correct for every relative/elapsed use, which
 * is all oksh needs). Body in time.c. */
struct timespec {
	time_t tv_sec;
	long   tv_nsec;
};

typedef int clockid_t;
#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1

#define CLOCKS_PER_SEC 1000L

int clock_gettime(clockid_t clk_id, struct timespec *tp);
clock_t clock(void);

/* struct tm + localtime (maize-94): borrowed oksh's lex.c formats a prompt timestamp.
 * Maize's clock epoch is VM start (not the Unix epoch), so localtime derives an honest
 * time-of-day (tm_hour/tm_min/tm_sec) from the monotonic second counter and leaves the
 * calendar fields (year/mon/mday) at a fixed sentinel: correct for the relative
 * "seconds since boot as h:m:s" a prompt shows, a named deviation for calendar dates
 * (a real wall clock needs a host time-of-day syscall, out of scope). Body in time.c;
 * the returned pointer is a static struct tm (not reentrant, matching ISO C localtime). */
struct tm {
	int tm_sec;
	int tm_min;
	int tm_hour;
	int tm_mday;
	int tm_mon;
	int tm_year;
	int tm_wday;
	int tm_yday;
	int tm_isdst;
};

struct tm *localtime(const time_t *timep);

/* strftime (maize-94): a minimal formatter for borrowed oksh's prompt timestamp. Handles
 * the common numeric conversions (%H %M %S %T %Y %m %d %j %% %n %t) plus literal text;
 * any unrecognized %X is emitted verbatim. Calendar-dependent fields ride localtime's
 * VM-start epoch (an honest deviation for %Y/%m/%d, correct for %H:%M:%S). Body in time.c.
 * Returns the number of bytes written (excluding the NUL), or 0 if it would overflow. */
unsigned long strftime(char *s, unsigned long max, const char *format,
                       const struct tm *tm);

#endif /* MAIZE_TIME_H */
