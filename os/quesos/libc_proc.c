/* libc_proc.c -- maize-94 Phase (b) fixture, run UNDER quesOS.
 *
 * Exercises the new libc process/environment surface end to end through the POSIX
 * wrappers (not the raw stubs): real getenv/setenv/unsetenv over crt0-captured environ
 * (decision 8942), the getcwd wrapper (decision 8940), fork + waitpid + WEXITSTATUS
 * (decision 8943, WIFEXITED/WEXITSTATUS from sys/wait.h), and a pipe + shared-fd read
 * across fork. Self-checks each step; prints "libc-proc: PASS" or a FAIL marker.
 */

#include "unistd.h"
#include "stdlib.h"
#include "sys/wait.h"
#include "string.h"
#include "syscall.h"   /* _exit */

int printf(const char *, ...);

static int seq(const char *a, const char *b) { return strcmp(a, b) == 0; }

int main(void) {
    char cwd[256];
    int st;
    int fds[2];
    char rb[4];
    long n;
    pid_t pid;

    /* --- environment (crt0-captured environ + get/set/unset) --- */
    if (getenv("NOPE") != 0) { printf("libc-proc: FAIL getenv-absent\n"); return 1; }
    if (setenv("FOO", "bar", 1) != 0 || getenv("FOO") == 0 || !seq(getenv("FOO"), "bar")) { printf("libc-proc: FAIL setenv\n"); return 1; }
    if (setenv("FOO", "baz", 0) != 0 || !seq(getenv("FOO"), "bar")) { printf("libc-proc: FAIL setenv-noover\n"); return 1; }
    if (setenv("FOO", "baz", 1) != 0 || !seq(getenv("FOO"), "baz")) { printf("libc-proc: FAIL setenv-over\n"); return 1; }
    if (unsetenv("FOO") != 0 || getenv("FOO") != 0) { printf("libc-proc: FAIL unsetenv\n"); return 1; }

    /* --- getcwd wrapper --- */
    if (getcwd(cwd, 256) == 0 || !seq(cwd, "/")) { printf("libc-proc: FAIL getcwd\n"); return 1; }

    /* --- fork + waitpid + WEXITSTATUS --- */
    st = 0;
    pid = fork();
    if (pid < 0) { printf("libc-proc: FAIL fork\n"); return 1; }
    if (pid == 0) { _exit(42); }
    if (waitpid(pid, &st, 0) != pid || !WIFEXITED(st) || WEXITSTATUS(st) != 42) { printf("libc-proc: FAIL wait\n"); return 1; }

    /* --- pipe + shared-fd read across fork --- */
    if (pipe(fds) != 0) { printf("libc-proc: FAIL pipe\n"); return 1; }
    pid = fork();
    if (pid < 0) { printf("libc-proc: FAIL fork2\n"); return 1; }
    if (pid == 0) {
        write(fds[1], "ok", 2);
        _exit(0);
    }
    close(fds[1]);
    n = read(fds[0], rb, 4);
    st = 0;
    waitpid(pid, &st, 0);
    if (n != 2 || rb[0] != 'o' || rb[1] != 'k') { printf("libc-proc: FAIL pipe-data\n"); return 1; }

    printf("libc-proc: PASS\n");
    return 0;
}
