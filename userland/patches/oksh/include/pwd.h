/*
 * pwd.h (maize-94): Maize-local shim. oksh uses getpwnam() for ~user tilde
 * expansion and $HOME fallback (main.c, edit.c, eval.c), reading pw_name/pw_dir.
 * quesOS hostfs has no password database, so the backing getpwnam/getpwuid
 * (userland/patches/oksh/include/maize_stubs.c) return NULL: ~user expansion and
 * pw-based HOME lookup fall through to the environment, which is the honest wave-1
 * behavior (an env-driven shell with no /etc/passwd). Named, not faked.
 */
#ifndef _MAIZE_PWD_H_
#define _MAIZE_PWD_H_

#include <sys/types.h>

struct passwd {
	char   *pw_name;
	char   *pw_passwd;
	uid_t   pw_uid;
	gid_t   pw_gid;
	char   *pw_gecos;
	char   *pw_dir;
	char   *pw_shell;
};

struct passwd *getpwnam(const char *name);
struct passwd *getpwuid(uid_t uid);

/* setpwent/getpwent/endpwent: oksh's edit.c iterates the user database for ~user
 * tab-completion. quesOS has none, so getpwent returns NULL (an empty enumeration) and
 * set/endpwent are no-ops: completion simply finds no users. Stubs in maize_stubs.c. */
void setpwent(void);
void endpwent(void);
struct passwd *getpwent(void);

#endif /* _MAIZE_PWD_H_ */
