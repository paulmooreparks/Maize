/* execvp_ext.c -- maize-94 launcher fixture (decision 9084), run UNDER quesOS.
 *
 * Proves libc execvp's PATHEXT-style executable-name resolution (exact -> name.mzx ->
 * name.mzb per PATH entry) end to end, plus its negative path:
 *
 *   POSITIVE: a child sets PATH=/bin and execvp's the BARE name "bin_echoer" (no
 *   extension). The exact "/bin/bin_echoer" does not exist, so the fallback retries
 *   "/bin/bin_echoer.mzx", which does; bin_echoer prints "execvp-target: ran" and exits 7.
 *   The parent checks both the captured marker and WEXITSTATUS == 7.
 *
 *   NEGATIVE: a child execvp's a name for which neither the bare form nor any extended
 *   form exists. execvp must exhaust the forms and RETURN -1 (errno ENOENT) rather than
 *   the child being destroyed, so the child reaches its own _exit(77); the parent checks
 *   WEXITSTATUS == 77. This also exercises quesOS's execve returning -ENOENT on a missing
 *   image (instead of do_exit(127)), the kernel half that makes the userland retry work.
 *
 * The kernel execve does NO name rewriting; the .mzx/.mzb policy lives entirely in libc
 * (toolchain/rt execvp). Requires /bin mounted (:ro) with bin_echoer.mzx.
 */

#include "unistd.h"
#include "sys/wait.h"
#include "syscall.h"   /* _exit */
#include "string.h"    /* memcmp */

int printf(const char *, ...);

int main(void) {
    int p[2];
    char buf[64];
    long n;
    int st;
    pid_t pid;
    char *envp[2];
    char *argv[2];

    envp[0] = "PATH=/bin";
    envp[1] = 0;

    /* ---- POSITIVE: bare "bin_echoer" resolves to /bin/bin_echoer.mzx ---- */
    if (pipe(p) != 0) { printf("execvp-ext: FAIL pipe\n"); return 1; }
    pid = fork();
    if (pid < 0) { printf("execvp-ext: FAIL fork\n"); return 1; }
    if (pid == 0) {
        argv[0] = "bin_echoer"; argv[1] = 0;
        dup2(p[1], 1);
        close(p[0]); close(p[1]);
        execvp("bin_echoer", argv);   /* no extension: relies on the .mzx fallback */
        _exit(126);                   /* only reached if resolution failed */
    }
    close(p[1]);
    n = read(p[0], buf, sizeof buf - 1);
    close(p[0]);
    if (n < 0) { n = 0; }
    buf[n] = 0;
    waitpid(pid, &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 7) {
        printf("execvp-ext: FAIL positive-status (st=%d)\n", st);
        return 1;
    }
    if (n < 18 || memcmp(buf, "execvp-target: ran", 18) != 0) {
        printf("execvp-ext: FAIL positive-output\n");
        return 1;
    }

    /* ---- NEGATIVE: no form exists -> execvp returns, child reaches _exit(77) ---- */
    pid = fork();
    if (pid < 0) { printf("execvp-ext: FAIL fork2\n"); return 1; }
    if (pid == 0) {
        argv[0] = "no-such-command-xyz"; argv[1] = 0;
        execvp("no-such-command-xyz", argv);   /* every form ENOENT -> returns -1 */
        _exit(77);                             /* the point: execvp RETURNED, not killed */
    }
    waitpid(pid, &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 77) {
        printf("execvp-ext: FAIL negative-status (st=%d)\n", st);
        return 1;
    }

    printf("execvp-ext: PASS\n");
    return 0;
}
