/*
 * grp.h (maize-94): Maize-local shim. oksh's portable.h includes <grp.h> inside its
 * __linux__ block (the host cpp defines __linux__), but wave-1 oksh references no
 * group-database symbol (no getgrnam/getgrgid/setgroups). This shim exists only so
 * the include resolves under -nostdinc; it declares the minimal struct group in case
 * a future source touches it, with no functions (quesOS hostfs has no group db).
 */
#ifndef _MAIZE_GRP_H_
#define _MAIZE_GRP_H_

#include <sys/types.h>

struct group {
	char   *gr_name;
	char   *gr_passwd;
	gid_t   gr_gid;
	char  **gr_mem;
};

/* setgroups: oksh's misc.c drops supplementary groups when running a set-id script.
 * quesOS is single-user with no group model, so the backing stub (maize_stubs.c)
 * succeeds as a no-op (there is nothing to set). */
int setgroups(int ngroups, const gid_t *grouplist);

#endif /* _MAIZE_GRP_H_ */
