/* pipeline.c -- maize-93 AC5 fixture (the launcher), run UNDER quesOS.
 *
 * Builds a three-stage pipeline `producer | filter | consumer` out of SEPARATE .mzx
 * programs, doing exactly what a shell does: two pipes, three fork+dup2+execve children
 * wired stdin/stdout onto the pipe ends, every unused pipe fd closed so EOF propagates,
 * and a wait for all three. The consumer prints "pipeline: PASS". This is the oksh
 * shape without the shell (fork/exec/pipe/dup2/wait all exercised together).
 */

long sys_fork(void);
long sys_pipe(int *fds);
long sys_close(long fd);
long sys_dup2(long oldfd, long newfd);
long sys_execve(const char *path, char **argv, char **envp);
long sys_wait4(long pid, int *status, long options, long rusage);
void _exit(int code);

static char *nullenv[] = { 0 };

/* In the child: wire stdin/stdout onto the given pipe ends, close all four pipe fds,
 * then exec `path`. Does not return. */
static void child_exec(const char *path, int in_fd, int out_fd,
                       int a, int b, int c, int d) {
    if (in_fd >= 0) { sys_dup2(in_fd, 0); }
    if (out_fd >= 0) { sys_dup2(out_fd, 1); }
    sys_close(a); sys_close(b); sys_close(c); sys_close(d);
    char *argv[] = { (char *)path, 0 };
    sys_execve(path, argv, nullenv);
    _exit(127);   /* only reached if execve failed */
}

int main(void) {
    int p1[2];   /* producer -> filter */
    int p2[2];   /* filter -> consumer */
    sys_pipe(p1);
    sys_pipe(p2);

    long a = sys_fork();
    if (a == 0) { child_exec("/progs/producer.mzx", -1, p1[1], p1[0], p1[1], p2[0], p2[1]); }
    long b = sys_fork();
    if (b == 0) { child_exec("/progs/filter.mzx", p1[0], p2[1], p1[0], p1[1], p2[0], p2[1]); }
    long c = sys_fork();
    if (c == 0) { child_exec("/progs/consumer.mzx", p2[0], -1, p1[0], p1[1], p2[0], p2[1]); }

    /* Parent holds no pipe ends, so EOF flows when each stage exits. */
    sys_close(p1[0]); sys_close(p1[1]); sys_close(p2[0]); sys_close(p2[1]);
    int st = 0;
    sys_wait4(a, &st, 0, 0);
    sys_wait4(b, &st, 0, 0);
    sys_wait4(c, &st, 0, 0);
    return 0;
}
