/* toolchain/rt/unistd.c -- freestanding <unistd.h> slice for the Maize C runtime
 * (maize-148).
 *
 * Scoped to usleep only (decision 8445); the read/write/close/lseek descriptor
 * wrappers live in errno.c over syscall.h and are NOT re-declared here.
 *
 * usleep is a no-op stub returning 0 (decision 8441): it does not sleep. Frame pacing
 * is therefore not honored (max framerate), acceptable for DOOM Phase A/B bring-up. A
 * busy-wait on sys_clock_ms ($F0) was rejected: its millisecond resolution cannot honor
 * a sub-millisecond usec and it burns CPU in DOOM's frame-pacing loops. A real pacing
 * usleep is a follow-up if Phase B timing needs it.
 *
 * isatty / ftruncate joined here for the kilo editor port (maize-172). isatty asks the
 * termios syscall whether the fd is a console tty (the classic tcgetattr-based probe);
 * ftruncate (maize-179) is now a real syscall (SYS $4D) over the confined hostfs
 * backend, so a shrink truncates the file exactly (kilo's save-after-shrink no longer
 * leaves a stale tail).
 */
#include "unistd.h"
#include "termios.h"
#include "errno.h"
#include "syscall.h"
#include "sys/wait.h"   /* maize-94: wait/waitpid bodies live here (over sys_wait4) */
#include "stdlib.h"     /* maize-94: environ / getenv for execv / execvp */
#include "string.h"     /* maize-94: strlen / memcpy for execvp's PATH search */

int
usleep(unsigned useconds)
{
    (void)useconds;   /* no sleep: return immediately */
    return 0;
}

int
isatty(int fd)
{
    struct termios t;
    /* tcgetattr succeeds (returns 0) only when a window console is bound to fd 0/1/2;
       with host stdio or a file it returns -1 / EBADF. That success is exactly the
       "fd is a terminal" predicate isatty reports. */
    if (tcgetattr(fd, &t) == 0) {
        return 1;
    }
    errno = ENOTTY;
    return 0;
}

int
ftruncate(int fd, long length)
{
    /* maize-179: real truncate over SYS $4D (confined hostfs backend). Sets the file to
       exactly `length` (a shrink drops the tail, an extend zero-fills). The raw stub
       returns 0 or a [-4095, -1] -errno; __syscall_ret turns the error band into
       errno + -1, so a shrink-save (kilo) is now byte-exact with no stale tail. */
    return (int)__syscall_ret((unsigned long)sys_ftruncate(fd, length));
}

/* maize-174 job-control wrappers. Each guest-only syscall is dispatched by quesOS; a
 * bare-VM caller reaches the native table (harmless no-op / -errno). tcgetpgrp returns
 * the foreground pgid directly (never -errno); the rest pass through __syscall_ret so a
 * [-4095,-1] result becomes errno + -1, matching read/write/close. */
int
kill(pid_t pid, int sig)
{
    return (int)__syscall_ret((unsigned long)sys_kill((long)pid, (long)sig));
}

int
setpgid(pid_t pid, pid_t pgid)
{
    return (int)__syscall_ret((unsigned long)sys_setpgid((long)pid, (long)pgid));
}

pid_t
getpgid(pid_t pid)
{
    return (pid_t)__syscall_ret((unsigned long)sys_getpgid((long)pid));
}

pid_t
tcgetpgrp(int fd)
{
    (void)fd;   /* exactly one controlling tty; the fd is ignored (decision) */
    return (pid_t)sys_tcgetpgrp();
}

int
tcsetpgrp(int fd, pid_t pgid)
{
    (void)fd;
    return (int)__syscall_ret((unsigned long)sys_tcsetpgrp((long)pgid));
}

/* ==================================================================================
 * maize-94 (decision 8943): POSIX process-control wrappers over the maize-93 guest-only
 * raw stubs. Each is a thin call through __syscall_ret, exactly like read/write/open, so
 * a [-4095,-1] raw result becomes errno + -1. These let oksh/sbase fork+exec+wait+pipe
 * without touching the raw SYS layer.
 * ================================================================================== */

pid_t
fork(void)
{
    return (pid_t)__syscall_ret((unsigned long)sys_fork());
}

pid_t
getpid(void)
{
    return (pid_t)sys_getpid();   /* getpid never fails, so no __syscall_ret band check */
}

int
execve(const char *path, char *const argv[], char *const envp[])
{
    return (int)__syscall_ret((unsigned long)sys_execve(path, argv, envp));
}

int
execv(const char *path, char *const argv[])
{
    return execve(path, argv, (char *const *)environ);
}

int
pipe(int fds[2])
{
    return (int)__syscall_ret((unsigned long)sys_pipe(fds));
}

int
dup(int oldfd)
{
    return (int)__syscall_ret((unsigned long)sys_dup((long)oldfd));
}

int
dup2(int oldfd, int newfd)
{
    return (int)__syscall_ret((unsigned long)sys_dup2((long)oldfd, (long)newfd));
}

/* maize-94 (decision 8940): cwd wrappers over the new guest-only chdir/getcwd stubs.
 * getcwd returns buf on success, NULL + errno (ERANGE) when the buffer is too small. */
int
chdir(const char *path)
{
    return (int)__syscall_ret((unsigned long)sys_chdir(path));
}

char *
getcwd(char *buf, unsigned long size)
{
    if (__syscall_ret((unsigned long)sys_getcwd(buf, size)) < 0) {
        return (char *)0;   /* errno set by __syscall_ret (ERANGE) */
    }
    return buf;
}

/* maize-94 (decision 8943): sys/wait.h wait/waitpid over the raw sys_wait4 stub. */
pid_t
wait(int *status)
{
    return (pid_t)__syscall_ret((unsigned long)sys_wait4(-1L, status, 0L, (void *)0));
}

pid_t
waitpid(pid_t pid, int *status, int options)
{
    return (pid_t)__syscall_ret((unsigned long)sys_wait4((long)pid, status, (long)options, (void *)0));
}

/* Does `s` contain a '/'? (Its own function: one loop, keeping execvp's own body simple
 * for the qbe-maize backend.) A file with a slash is exec'd directly, no PATH search. */
static int
path_has_slash(const char *s)
{
    long i = 0;
    while (s[i] != '\0') { if (s[i] == '/') { return 1; } i++; }
    return 0;
}

/* execvp: like execv, but a file with no '/' is searched along PATH (colon-separated;
 * default "/bin" when PATH is unset/empty, per the /bin convention, decision 8939). An
 * empty PATH entry means the current directory. execve only returns on failure; a
 * per-candidate ENOENT keeps searching, any other error stops. */
int
execvp(const char *file, char *const argv[])
{
    char buf[256];
    const char *path;
    const char *p;
    unsigned long flen;

    if (file == (const char *)0 || file[0] == '\0') { errno = ENOENT; return -1; }
    if (path_has_slash(file)) { return execve(file, argv, (char *const *)environ); }

    path = getenv("PATH");
    if (path == (const char *)0 || path[0] == '\0') { path = "/bin"; }
    flen = strlen(file);

    p = path;
    for (;;) {
        const char *start = p;
        unsigned long dlen;
        while (*p != '\0' && *p != ':') { p++; }
        dlen = (unsigned long)(p - start);
        if (dlen == 0) { start = "."; dlen = 1; }             /* empty entry = cwd */
        if (dlen + 1 + flen + 1 <= sizeof(buf)) {
            memcpy(buf, start, dlen);
            buf[dlen] = '/';
            memcpy(buf + dlen + 1, file, flen);
            buf[dlen + 1 + flen] = '\0';
            execve(buf, argv, (char *const *)environ);        /* returns only on error */
            if (errno != ENOENT) { return -1; }               /* a real error: stop */
        }
        if (*p == '\0') { break; }
        p++;                                                   /* skip the ':' */
    }
    errno = ENOENT;
    return -1;
}
