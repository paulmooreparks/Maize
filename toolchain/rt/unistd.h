/* toolchain/rt/unistd.h -- freestanding <unistd.h> slice for the Maize C runtime
 * (maize-147).
 *
 * DOOM's i_timer.c / i_system.c call usleep; strict cproc needs a visible declaration
 * at each call site. usleep's body lives in the sibling libc card (maize-148). Return
 * int, arg unsigned (avoids needing useconds_t).
 *
 * maize-172 (kilo) widens this to the standard POSIX <unistd.h> surface an editor
 * reaches for: the read/write/close descriptor wrappers (whose bodies live in errno.c
 * over syscall.h) are RE-DECLARED here with signatures byte-identical to syscall.h so a
 * TU that includes <unistd.h> instead of the Maize-private "syscall.h" still sees a
 * prototype (a duplicate typedef-free redeclaration is legal C). isatty and ftruncate
 * are new (bodies in unistd.c).
 */
#ifndef MAIZE_UNISTD_H
#define MAIZE_UNISTD_H

#include "sys/types.h"   /* uid_t / gid_t / pid_t (maize-94) */

int usleep(unsigned useconds);

/* POSIX descriptor wrappers (bodies in errno.c). Signatures MUST match syscall.h. */
long read(int fd, void *buf, unsigned long count);
long write(int fd, const void *buf, unsigned long count);
int  close(int fd);

/* isatty (maize-172): 1 if fd is a bound console tty, 0 otherwise (errno = ENOTTY).
 * Implemented over tcgetattr, which succeeds only for a console-backed fd. */
int isatty(int fd);

/* ftruncate (maize-179): a real syscall (SYS $4D) over the confined hostfs backend.
 * Resizes the open file to exactly `length` (a shrink drops the tail, an extend
 * zero-fills); the file offset is unchanged. Returns 0, or -1 with errno set (EROFS on
 * a :ro mount, EINVAL on a negative length or a non-file fd, EBADF on a bad fd). kilo's
 * save rewrites the whole buffer after ftruncate, so a shrink is now byte-exact. */
int ftruncate(int fd, long length);

/* maize-174 job-control wrappers over the guest-only signal/pgroup syscalls. pid_t is
 * defined here (guarded) so a ported shell that reaches for <unistd.h> gets it. */
#ifndef MAIZE_PID_T_DEFINED
#define MAIZE_PID_T_DEFINED
typedef int pid_t;
#endif

int   kill(pid_t pid, int sig);
int   setpgid(pid_t pid, pid_t pgid);
pid_t getpgid(pid_t pid);
pid_t tcgetpgrp(int fd);
int   tcsetpgrp(int fd, pid_t pgid);

/* maize-94 (decisions 8943 / 8940 / 8939): POSIX process-control + cwd wrappers over the
 * guest-only quesOS syscalls, for the wave-1 shell/userland port. fork/execve/execv/pipe/
 * dup/dup2/getpid are thin __syscall_ret calls; execvp adds PATH search (default /bin);
 * chdir/getcwd drive the per-process working directory. wait/waitpid live in <sys/wait.h>.
 * execve/execv/execvp take char *const argv[]/envp[] (the POSIX signatures). */
pid_t fork(void);
pid_t getpid(void);
int   execve(const char *path, char *const argv[], char *const envp[]);
int   execv(const char *path, char *const argv[]);
int   execvp(const char *file, char *const argv[]);
int   pipe(int fds[2]);
int   dup(int oldfd);
int   dup2(int oldfd, int newfd);
int   chdir(const char *path);
char *getcwd(char *buf, unsigned long size);

/* unlink (maize-94): POSIX file removal, the name borrowed sbase (cp -f retry, the
 * wave-1 rm) uses; body in errno.c beside remove() (its ISO C twin). */
int   unlink(const char *path);

/* maize-94: the remaining POSIX <unistd.h> surface borrowed oksh reaches for. Bodies
 * in unistd.c. quesOS is single-user with no permission/uid model and no symlinks, so
 * several are honest, decision-noted stubs rather than fakes:
 *   access: a real reachability test, composed over stat() (F_OK/R_OK succeed iff the
 *           path stats; X_OK cannot be distinguished on hostfs, so it tracks F_OK).
 *   getuid/getgid/geteuid/getegid: return 0 (quesOS runs everything as a single
 *           unprivileged-yet-unrestricted principal; 0 is the honest "root-ish" answer).
 *   readlink: hostfs has no symlinks, so returns -1/EINVAL (never a link).
 *   alarm: no interval timer under wave 1 (SIGALRM is not delivered); returns 0.
 *   nice: no scheduler priority; returns 0.
 *   ttyname: no /dev name table; returns NULL.
 *   killpg: kill(-pgrp, sig) over the guest kill syscall. */
#define R_OK 4
#define W_OK 2
#define X_OK 1
#define F_OK 0

int   access(const char *path, int mode);
uid_t getuid(void);
uid_t geteuid(void);
gid_t getgid(void);
gid_t getegid(void);
long  readlink(const char *path, char *buf, unsigned long bufsiz);
unsigned int alarm(unsigned int seconds);
int   nice(int inc);
char *ttyname(int fd);
int   killpg(pid_t pgrp, int sig);

/* lseek (maize-94): reposition a descriptor. Body in errno.c over sys_lseek; declared in
 * <fcntl.h>/<stdio.h>/syscall.h already, re-declared here (its POSIX home) so a TU that
 * includes only <unistd.h> (oksh's shf.c) sees it. Identical prototype, cproc-tolerated. */
long  lseek(int fd, long offset, int whence);

/* getppid / getpgrp (maize-94): getppid returns 1 (quesOS has no parent-of-init concept;
 * a shell reads $PPID but does not depend on its value under wave 1). getpgrp aliases
 * getpgid(0) over the maize-174 pgroup syscall. */
pid_t getppid(void);
pid_t getpgrp(void);

/* getsid (maize-94): session id. quesOS has no session model, so returns 0 (a shell
 * reads it for job-control decisions that wave 1 does not exercise, decision 8947). */
pid_t getsid(pid_t pid);

/* gethostname (maize-94): quesOS has no configurable hostname, so this writes a fixed
 * "maize" into the caller's buffer (oksh's \h prompt escape). Body in unistd.c. */
int gethostname(char *name, unsigned long len);

/* _POSIX_VDISABLE (maize-94): the c_cc value meaning "this control char is disabled",
 * which oksh's tty.c compares against. The Linux value is 0. */
#define _POSIX_VDISABLE 0

/* getlogin (maize-94): quesOS has no login/user database, so returns NULL (oksh falls
 * back to $USER / $LOGNAME). */
char *getlogin(void);

/* sleep (maize-94): oksh's jobs.c backs off with sleep(1) on a transient fork failure.
 * Composed over usleep (which is itself a no-op stub under wave 1, so sleep also returns
 * promptly); returns 0 (no unslept remainder). */
unsigned int sleep(unsigned int seconds);

/* set*id (maize-94): quesOS is single-user with no credential model, so these succeed
 * as no-ops. oksh's misc.c reaches them through portable.h's setresgid/setresuid macros
 * when it drops privileges for a set-id script; on Maize there is nothing to drop. */
int setuid(uid_t uid);
int seteuid(uid_t uid);
int setgid(gid_t gid);
int setegid(gid_t gid);

/* _CS_PATH (maize-94): confstr() key for the default utility PATH. oksh's confstr.c
 * compat answers it with _PATH_DEFPATH; main.c uses it to seed PATH. */
#define _CS_PATH 1

#endif /* MAIZE_UNISTD_H */
