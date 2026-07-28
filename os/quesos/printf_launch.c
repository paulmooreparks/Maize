/* printf_launch.c -- maize-94 launcher fixture (decision 9078), run UNDER quesOS.
 *
 * Standalone (no-shell) AC-8935 substrate for the arg-taking sbase `printf`. quesOS
 * worklist entries take no args, so this fixture forks and execve's /bin/printf.mzx
 * with a representative argv, captures its stdout through a pipe, and checks it. The
 * invocation exercises a literal run, %s, %d (integer parse via strtoul), and \n
 * unescape: `printf 'x=%s:%d\n' hi 42` must yield "x=hi:42\n". Requires /bin mounted
 * with printf.mzx. Prints "printf-launch: PASS" or a FAIL marker naming the step.
 *
 * A second case covers %o (maize-393). It is the only regression check on that card
 * that reaches the RUNTIME-CONSTRUCTED format path: userland/sbase/printf.c builds
 * "%*.*ll#" in a local array and overwrites the last byte with the user's conversion
 * letter, so no compile-time format check could ever have seen the %o that the guest
 * RT did not implement. `printf '%o\n' 8` must yield "10\n"; before the fix it
 * yielded the literal text "%o\n".
 */

#include "unistd.h"
#include "sys/wait.h"
#include "syscall.h"   /* _exit */
#include "string.h"    /* memcmp */

int printf(const char *, ...);

/* Run /bin/printf.mzx with the given format and one or two operands (pass 0 for a2
 * to omit the second), capturing its stdout. Returns 0 when the captured bytes are
 * exactly the first wantlen bytes of `want`, non-zero otherwise, after printing a
 * FAIL marker naming the format that failed. */
static int
run_printf(const char *fmt, const char *a1, const char *a2,
           const char *want, long wantlen)
{
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
        argv[1] = (char *)fmt;
        argv[2] = (char *)a1;
        argv[3] = (char *)a2;
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

    if (got != wantlen || memcmp(buf, want, wantlen) != 0) {
        printf("printf-launch: FAIL output for '%s' (got=%ld)\n", fmt, got);
        return 1;
    }
    return 0;
}

int main(void) {
    /* maize-94: a literal run, %s, %d and \n unescape in one invocation. */
    if (run_printf("x=%s:%d\n", "hi", "42", "x=hi:42\n", 8) != 0)
        return 1;

    /* maize-393: octal through sbase printf's runtime-constructed format. */
    if (run_printf("%o\n", "8", 0, "10\n", 3) != 0)
        return 1;

    printf("printf-launch: PASS\n");
    return 0;
}
