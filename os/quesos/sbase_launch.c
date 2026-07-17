/* sbase_launch.c -- maize-94 launcher fixture (decision 9078), run UNDER quesOS.
 *
 * The standalone (no-shell) AC-8935 substrate for arg-taking sbase utilities: quesOS
 * worklist entries take no args, but execve marshals argv, so a fixture forks and
 * execve's /bin/<util>.mzx with a representative argv and checks the result. This one
 * runs a real two-stage pipeline of vendored sbase binaries, `echo payload | cat`,
 * through fork + pipe + dup2 + execve + wait4: echo's stdout feeds cat's stdin, cat's
 * stdout feeds a pipe the parent reads. Proves echo (argv marshaling + output) and cat
 * (stdin passthrough) run correctly, and exercises the exact plumbing AC 8931's
 * shell-driven pipeline will use. Requires /bin mounted with echo.mzx + cat.mzx. Prints
 * "sbase-launch: PASS" or a FAIL marker naming the failing step.
 */

#include "unistd.h"
#include "sys/wait.h"
#include "syscall.h"   /* _exit */
#include "string.h"    /* memcmp */

int printf(const char *, ...);

int main(void) {
    int ab[2];   /* echo -> cat  */
    int bp[2];   /* cat  -> parent */
    char buf[64];
    long n;
    int st;
    pid_t pe, pc;

    if (pipe(ab) != 0 || pipe(bp) != 0) { printf("sbase-launch: FAIL pipe\n"); return 1; }

    pe = fork();
    if (pe < 0) { printf("sbase-launch: FAIL fork-echo\n"); return 1; }
    if (pe == 0) {
        char *argv[3];
        argv[0] = "echo"; argv[1] = "payload"; argv[2] = 0;
        dup2(ab[1], 1);
        close(ab[0]); close(ab[1]); close(bp[0]); close(bp[1]);
        execve("/bin/echo.mzx", argv, 0);
        _exit(127);
    }

    pc = fork();
    if (pc < 0) { printf("sbase-launch: FAIL fork-cat\n"); return 1; }
    if (pc == 0) {
        char *argv[2];
        argv[0] = "cat"; argv[1] = 0;
        dup2(ab[0], 0);
        dup2(bp[1], 1);
        close(ab[0]); close(ab[1]); close(bp[0]); close(bp[1]);
        execve("/bin/cat.mzx", argv, 0);
        _exit(127);
    }

    close(ab[0]); close(ab[1]); close(bp[1]);
    n = read(bp[0], buf, sizeof buf);
    waitpid(pe, &st, 0);
    waitpid(pc, &st, 0);

    if (n != 8 || memcmp(buf, "payload\n", 8) != 0) {
        printf("sbase-launch: FAIL output (n=%ld)\n", n);
        return 1;
    }
    printf("sbase-launch: PASS\n");
    return 0;
}
