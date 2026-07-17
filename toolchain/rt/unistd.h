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

#endif /* MAIZE_UNISTD_H */
