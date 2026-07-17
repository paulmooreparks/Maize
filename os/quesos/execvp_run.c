/* execvp_run.c -- maize-94 Phase (b) fixture, run UNDER quesOS.
 *
 * Proves execvp's PATH search (decision 8939) and the process-control wrappers together:
 * the child sets PATH=/bin, then execvp's the bare command name "bin_echoer.mzx" (no
 * slash), so execvp walks PATH and execve's /bin/bin_echoer.mzx. The parent waitpid's and
 * checks the child's WEXITSTATUS matches the target's exit (7). The target also prints its
 * own marker. Runs with the binaries mounted at BOTH /progs (worklist) and /bin (PATH).
 * Prints "execvp: PASS" or a FAIL marker.
 */

#include "unistd.h"
#include "stdlib.h"
#include "sys/wait.h"
#include "syscall.h"   /* _exit */

int printf(const char *, ...);

int main(void) {
    int st = 0;
    pid_t pid = fork();
    if (pid < 0) { printf("execvp: FAIL fork\n"); return 1; }
    if (pid == 0) {
        char *argv[2];
        argv[0] = "bin_echoer.mzx";
        argv[1] = 0;
        setenv("PATH", "/bin", 1);
        execvp("bin_echoer.mzx", argv);   /* returns only on failure */
        printf("execvp: FAIL execvp-returned\n");
        _exit(127);
    }
    if (waitpid(pid, &st, 0) != pid || !WIFEXITED(st) || WEXITSTATUS(st) != 7) {
        printf("execvp: FAIL wait\n");
        return 1;
    }
    printf("execvp: PASS\n");
    return 0;
}
