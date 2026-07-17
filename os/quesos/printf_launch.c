/* printf_launch.c -- maize-94 launcher fixture (decision 9078), run UNDER quesOS.
 *
 * Standalone (no-shell) AC-8935 substrate for the arg-taking sbase `printf`. quesOS
 * worklist entries take no args, so this fixture forks and execve's /bin/printf.mzx
 * with a representative argv, captures its stdout through a pipe, and checks it. The
 * invocation exercises a literal run, %s, %d (integer parse via strtoul), and \n
 * unescape: `printf 'x=%s:%d\n' hi 42` must yield "x=hi:42\n". Requires /bin mounted
 * with printf.mzx. Prints "printf-launch: PASS" or a FAIL marker naming the step.
 */

#include "unistd.h"
#include "sys/wait.h"
#include "syscall.h"   /* _exit */
#include "string.h"    /* memcmp */

int printf(const char *, ...);

int main(void) {
    int pp[2];   /* printf -> parent */
    char buf[64];
    long got = 0, n;
    int st;
    pid_t pc;

    if (pipe(pp) != 0) { printf("printf-launch: FAIL pipe\n"); return 1; }

    pc = fork();
    if (pc < 0) { printf("printf-launch: FAIL fork\n"); return 1; }
    if (pc == 0) {
        char *argv[5];
        argv[0] = "printf";
        argv[1] = "x=%s:%d\n";
        argv[2] = "hi";
        argv[3] = "42";
        argv[4] = 0;
        dup2(pp[1], 1);
        close(pp[0]); close(pp[1]);
        execve("/bin/printf.mzx", argv, 0);
        _exit(127);
    }

    close(pp[1]);
    /* printf's stdout is a pipe (not a tty); it may arrive in several writes, so
     * accumulate until EOF (child closes the write end) rather than trusting one
     * read to deliver the whole line. */
    while (got < (long)sizeof buf
           && (n = read(pp[0], buf + got, sizeof buf - got)) > 0)
        got += n;
    waitpid(pc, &st, 0);

    if (got != 8 || memcmp(buf, "x=hi:42\n", 8) != 0) {
        printf("printf-launch: FAIL output (got=%ld)\n", got);
        return 1;
    }
    printf("printf-launch: PASS\n");
    return 0;
}
