/* toolchain/rt/sys/time.h -- freestanding <sys/time.h> slice for the Maize C
 * runtime (maize-172).
 *
 * kilo includes <sys/time.h> for portability but, on this build, reaches elapsed
 * time only through time() (<time.h>); it uses nothing declared here. struct timeval
 * is provided so the include resolves and any incidental reference compiles. A real
 * gettimeofday would need a host wall-clock syscall and is out of scope; it is not
 * declared here so a caller that needs it fails at compile time rather than linking
 * a silent stub.
 */
#ifndef MAIZE_SYS_TIME_H
#define MAIZE_SYS_TIME_H

struct timeval {
	long tv_sec;    /* seconds (long, matching time_t width on this LP64 target) */
	long tv_usec;   /* microseconds */
};

#endif /* MAIZE_SYS_TIME_H */
