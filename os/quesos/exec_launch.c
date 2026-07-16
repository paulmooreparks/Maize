/* exec_launch.c -- maize-93 AC2 fixture (launcher), run UNDER quesOS.
 *
 * Proves execve replaces the image and runs the new program with the passed argv/envp,
 * and that the fd table survives exec: this process dup2's stdout onto fd 5 BEFORE
 * calling execve, and the exec'd target writes its result to fd 5. If the fd table did
 * not survive, fd 5 would be closed and the target's write would be lost.
 */

int printf(const char *, ...);
long sys_dup2(long oldfd, long newfd);
long sys_execve(const char *path, char **argv, char **envp);

int main(void) {
    sys_dup2(1, 5);   /* fd 5 aliases stdout; must survive the execve below */

    char *argv[] = { "/progs/exec_target.mzx", "alpha", "beta", 0 };
    char *envp[] = { "QOSVAR=set", 0 };
    sys_execve(argv[0], argv, envp);

    printf("exec: FAIL (execve returned)\n");
    return 1;
}
