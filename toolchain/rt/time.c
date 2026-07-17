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

/* clock_gettime (maize-94): borrowed oksh reads CLOCK_MONOTONIC for its mail-check and
 * `time`-builtin elapsed measurements. Both clock ids map to the same monotonic
 * millisecond counter (sys_clock_ms) time() reads; the epoch is VM start (honest
 * deviation, correct for every relative/elapsed use). Fills tv_sec / tv_nsec. */
int
clock_gettime(clockid_t clk_id, struct timespec *tp)
{
	unsigned long ms = sys_clock_ms();
	(void)clk_id;
	if (tp != 0) {
		tp->tv_sec = (time_t)(ms / 1000UL);
		tp->tv_nsec = (long)((ms % 1000UL) * 1000000UL);
	}
	return 0;
}

/* clock (maize-94): ISO C processor-time clock. Maize has no per-process CPU
 * accounting, so this returns elapsed monotonic time scaled to CLOCKS_PER_SEC (== ms),
 * i.e. wall-clock ms since VM start. Honest for the relative deltas a shell computes. */
clock_t
clock(void)
{
	return (clock_t)sys_clock_ms();
}

/* localtime (maize-94): derive an honest time-of-day from the monotonic second counter
 * (epoch = VM start). tm_hour/tm_min/tm_sec are the seconds-since-boot broken down; the
 * calendar fields are a fixed sentinel (1970-01-01) since Maize has no wall clock. Not
 * reentrant (static buffer), matching ISO C localtime. */
struct tm *
localtime(const time_t *timep)
{
	static struct tm tm;
	time_t t = (timep != 0) ? *timep : time(0);
	time_t day = t % 86400;
	if (day < 0) {
		day += 86400;
	}
	tm.tm_sec = (int)(day % 60);
	tm.tm_min = (int)((day / 60) % 60);
	tm.tm_hour = (int)(day / 3600);
	tm.tm_mday = 1;
	tm.tm_mon = 0;
	tm.tm_year = 70;   /* 1970: sentinel; Maize has no calendar clock */
	tm.tm_wday = 0;
	tm.tm_yday = 0;
	tm.tm_isdst = 0;
	return &tm;
}

/* strftime (maize-94): minimal formatter for a shell prompt timestamp. Supports the
 * numeric conversions oksh's prompt uses (%H %M %S %T %Y %m %d %j %n %t %%); an
 * unrecognized %X is copied verbatim. Returns bytes written (excluding NUL), or 0 on
 * overflow (POSIX). */
static unsigned long
put2(char *dst, unsigned long room, int v)
{
	if (room < 2) {
		return 0;
	}
	dst[0] = (char)('0' + (v / 10) % 10);
	dst[1] = (char)('0' + v % 10);
	return 2;
}

unsigned long
strftime(char *s, unsigned long max, const char *format, const struct tm *tm)
{
	unsigned long o = 0;
	const char *f = format;

	if (s == 0 || max == 0) {
		return 0;
	}
	while (*f != '\0') {
		if (o + 1 >= max) {
			return 0;   /* no room for even a char + NUL */
		}
		if (*f != '%') {
			s[o++] = *f++;
			continue;
		}
		f++;   /* consume '%' */
		switch (*f) {
		case 'H': o += put2(s + o, max - o, tm->tm_hour); break;
		case 'M': o += put2(s + o, max - o, tm->tm_min); break;
		case 'S': o += put2(s + o, max - o, tm->tm_sec); break;
		case 'm': o += put2(s + o, max - o, tm->tm_mon + 1); break;
		case 'd': o += put2(s + o, max - o, tm->tm_mday); break;
		case 'y': o += put2(s + o, max - o, tm->tm_year % 100); break;
		case 'T':
			o += put2(s + o, max - o, tm->tm_hour);
			if (o + 1 < max) { s[o++] = ':'; }
			o += put2(s + o, max - o, tm->tm_min);
			if (o + 1 < max) { s[o++] = ':'; }
			o += put2(s + o, max - o, tm->tm_sec);
			break;
		case 'Y': {
			int y = tm->tm_year + 1900;
			if (o + 4 < max) {
				s[o++] = (char)('0' + (y / 1000) % 10);
				s[o++] = (char)('0' + (y / 100) % 10);
				s[o++] = (char)('0' + (y / 10) % 10);
				s[o++] = (char)('0' + y % 10);
			}
			break;
		}
		case 'n': s[o++] = '\n'; break;
		case 't': s[o++] = '\t'; break;
		case '%': s[o++] = '%'; break;
		case '\0': s[o] = '\0'; return o;   /* trailing '%' */
		default:
			if (o + 2 < max) { s[o++] = '%'; s[o++] = *f; }
			break;
		}
		f++;
	}
	s[o] = '\0';
	return o;
}
