/*
 * maize_stubs.c (maize-94): Maize-local overlay source backing the wave-1 oksh shim
 * headers (grp.h/pwd.h/sys/file.h/sys/resource.h) with honest, named no-op behavior
 * for services quesOS does not provide under wave 1. This file is copied into the
 * pristine oksh submodule scratch by build-userland.sh and listed in oksh.list; it
 * is never committed into the submodule, keeping the vendored tree re-pinnable.
 *
 * Every stub here corresponds to a decision-noted deviation, not a silent fake:
 *   - getpwnam/getpwuid: quesOS hostfs has no /etc/passwd, so there is no user db to
 *     answer from; returning NULL makes oksh fall back to the environment for HOME
 *     and makes ~user expansion decline gracefully.
 *   - flock: single-user, foreground-only wave-1 shell (decision 8947) never contends
 *     the history file, so advisory locking is a success no-op.
 *   - getrlimit/setrlimit/getrusage: quesOS enforces no per-process resource limits
 *     and keeps no rusage accounting; report unlimited/zeroed.
 */

#include <sys/types.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <pwd.h>
#include <grp.h>
#include <string.h>

struct passwd *
getpwnam(const char *name)
{
	(void)name;
	return (struct passwd *)0;
}

struct passwd *
getpwuid(uid_t uid)
{
	(void)uid;
	return (struct passwd *)0;
}

/* User-database enumeration: quesOS has no /etc/passwd, so the enumeration is empty. */
void setpwent(void) { }
void endpwent(void) { }
struct passwd *getpwent(void) { return (struct passwd *)0; }

int
flock(int fd, int operation)
{
	(void)fd;
	(void)operation;
	return 0;
}

int
getrlimit(int resource, struct rlimit *rlp)
{
	(void)resource;
	if (rlp != (struct rlimit *)0) {
		rlp->rlim_cur = RLIM_INFINITY;
		rlp->rlim_max = RLIM_INFINITY;
	}
	return 0;
}

int
setrlimit(int resource, const struct rlimit *rlp)
{
	(void)resource;
	(void)rlp;
	return 0;
}

int
getrusage(int who, struct rusage *ru)
{
	(void)who;
	if (ru != (struct rusage *)0)
		memset(ru, 0, sizeof(*ru));
	return 0;
}

int
setgroups(int ngroups, const gid_t *grouplist)
{
	(void)ngroups;
	(void)grouplist;
	return 0;
}
