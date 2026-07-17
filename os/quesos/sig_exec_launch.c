/* sig_exec_launch.c -- maize-174 AC fixture (launcher), run UNDER quesOS.
 *
 * Proves POSIX exec signal semantics: a caught handler is reset to SIG_DFL across execve,
 * so a signal delivered to the post-exec image default-terminates rather than jumping to
 * the stale handler VA that belonged to the orphaned old image.
 *
 *   child: install a SIGINT handler (its VA lives in THIS image), dup2 a pipe write end
 *          onto fd 5 (survives execve), then execve sig_exec_target (which installs NO
 *          handler and announces readiness on fd 5, then busy-loops).
 *   parent: read the target's readiness (proof exec completed), SIGINT the child; if exec
 *          reset the handler to SIG_DFL the child default-terminates and wait4 reports
 *          WTERMSIG==SIGINT. A stale-VA jump would not yield a clean SIGINT termination.
 *
 * Output on success: "sig-exec: PASS".
 */

int printf(const char *, ...);
long sys_fork(void);
long sys_wait4(long pid, void *status, long options, void *rusage);
long sys_pipe(void *fds);
long sys_read(long fd, void *buf, long count);
long sys_close(long fd);
long sys_dup2(long oldfd, long newfd);
long sys_kill(long pid, long sig);
long sys_rt_sigaction(long sig, const void *act, void *oldact);
long sys_execve(const char *path, char **argv, char **envp);

#define SIGINT 2

long g_sink;

static void on_int(int s) { (void)s; g_sink++; }   /* caught handler; VA is in this image */

int main(void) {
    int b[2];
    long child;
    char c = 0;
    unsigned int st = 0;

    if (sys_pipe(b) != 0) { printf("sig-exec: FAIL pipe\n"); return 0; }
    child = sys_fork();
    if (child < 0) { printf("sig-exec: FAIL fork\n"); return 0; }

    if (child == 0) {
        unsigned long act[3];
        char *argv[] = { "/progs/sig_exec_target.mzx", 0 };
        char *envp[] = { 0 };
        sys_close(b[0]);
        sys_dup2(b[1], 5);        /* pipe write end at fd 5; survives execve */
        sys_close(b[1]);
        act[0] = (unsigned long)&on_int; act[1] = 0; act[2] = 0;
        sys_rt_sigaction(SIGINT, act, 0);
        sys_execve(argv[0], argv, envp);   /* handler[SIGINT] VA now belongs to the OLD image */
        printf("sig-exec: FAIL (execve returned)\n");
        return 1;
    }

    sys_close(b[1]);
    sys_read(b[0], &c, 1);        /* target is up (post-exec) */
    sys_kill(child, SIGINT);      /* exec must have reset the caught handler to SIG_DFL */
    sys_wait4(child, &st, 0, 0);
    if ((st & 0x7Fu) != (unsigned)SIGINT) { printf("sig-exec: FAIL not-sigint\n"); return 0; }
    printf("sig-exec: PASS\n");
    return 0;
}
