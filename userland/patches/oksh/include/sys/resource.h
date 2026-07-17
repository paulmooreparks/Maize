/*
 * sys/resource.h (maize-94): Maize-local shim. oksh's ulimit builtin (c_ulimit.c)
 * and its `time` reporting (jobs.c) reach for getrlimit/setrlimit/getrusage. quesOS
 * enforces no per-process resource limits and keeps no rusage accounting under
 * wave-1, so the backing calls (maize_stubs.c) report "unlimited" / zeroed values:
 * getrlimit returns RLIM_INFINITY, setrlimit succeeds as a no-op, getrusage zeroes
 * the struct. Honest deviation, named (resource limits are not a wave-1 feature).
 */
#ifndef _MAIZE_SYS_RESOURCE_H_
#define _MAIZE_SYS_RESOURCE_H_

#include <sys/types.h>
#include <sys/time.h>

typedef unsigned long rlim_t;

#define RLIM_INFINITY ((rlim_t)-1)

struct rlimit {
	rlim_t rlim_cur;
	rlim_t rlim_max;
};

#define RLIMIT_CPU     0
#define RLIMIT_FSIZE   1
#define RLIMIT_DATA    2
#define RLIMIT_STACK   3
#define RLIMIT_CORE    4
#define RLIMIT_RSS     5
#define RLIMIT_MEMLOCK 6
#define RLIMIT_NPROC   7
#define RLIMIT_NOFILE  8

struct rusage {
	struct timeval ru_utime;
	struct timeval ru_stime;
	long ru_maxrss;
	long ru_ixrss;
	long ru_idrss;
	long ru_isrss;
	long ru_minflt;
	long ru_majflt;
	long ru_nswap;
	long ru_inblock;
	long ru_oublock;
	long ru_msgsnd;
	long ru_msgrcv;
	long ru_nsignals;
	long ru_nvcsw;
	long ru_nivcsw;
};

#define RUSAGE_SELF     0
#define RUSAGE_CHILDREN (-1)

int getrlimit(int resource, struct rlimit *rlp);
int setrlimit(int resource, const struct rlimit *rlp);
int getrusage(int who, struct rusage *ru);

#endif /* _MAIZE_SYS_RESOURCE_H_ */
