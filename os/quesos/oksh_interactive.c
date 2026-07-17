/* oksh_interactive.c -- maize-94 launcher fixture (operator reopen), run UNDER quesOS.
 *
 * The missing deterministic proof of the INTERACTIVE oksh path (the acceptance escape: all
 * other oksh fixtures ran `oksh -c` or piped a script to a non-interactive shell, so the
 * interactive line-editor init -- which queries the terminal size via $F6 sys_ttysize and
 * opens /dev/tty -- was never exercised). This forks `oksh -i`, which forces interactive
 * init even though fd 0 is a pipe (no controlling tty), so it takes the /dev/tty-fallback
 * warning path AND runs the ttysize query. With $F6 forwarded by quesOS the query returns a
 * clean -ENOTTY on the pipe fd (rather than the old unhandled-syscall strand) and oksh
 * degrades to its default size, emits its prompt, and executes the piped commands.
 *
 * The child's stdin is a pipe carrying "pwd\nexit\n"; its stdout+stderr are captured through
 * another pipe (oksh writes the prompt to stderr). PS1 is set in the environment to a
 * distinctive marker so the emitted prompt is greppable. PASS requires BOTH the prompt
 * marker (oksh reached its interactive prompt) and the pwd output "/" (it executed a
 * command), then a clean exit. Requires /bin mounted (:ro) with oksh.mzx.
 */

#include "unistd.h"
#include "sys/wait.h"
#include "syscall.h"   /* _exit */
#include "string.h"    /* strlen */

int printf(const char *, ...);

static int contains(const char *hay, long haylen, const char *needle) {
    long nl = (long)strlen(needle);
    long i;
    if (nl == 0) { return 1; }
    for (i = 0; i + nl <= haylen; ++i) {
        long j = 0;
        while (j < nl && hay[i + j] == needle[j]) { ++j; }
        if (j == nl) { return 1; }
    }
    return 0;
}

int main(void) {
    int sin[2];    /* script  -> oksh stdin  */
    int sout[2];   /* oksh stdout+stderr -> parent */
    char buf[1024];
    long total = 0, n;
    int st = 0;
    pid_t pid;
    char *argv[3];
    char *envp[3];
    const char *script = "pwd\nexit\n";

    if (pipe(sin) != 0 || pipe(sout) != 0) { printf("oksh-interactive: FAIL pipe\n"); return 1; }

    /* Preload the whole script into the pipe (fits well under the ring buffer) so oksh can
     * read it without the parent racing to feed it. */
    n = write(sin[1], script, (unsigned long)strlen(script));
    close(sin[1]);
    if (n != (long)strlen(script)) { printf("oksh-interactive: FAIL preload\n"); return 1; }

    pid = fork();
    if (pid < 0) { printf("oksh-interactive: FAIL fork\n"); return 1; }
    if (pid == 0) {
        argv[0] = "oksh"; argv[1] = "-i"; argv[2] = 0;
        envp[0] = "PATH=/bin"; envp[1] = "PS1=MZPROMPT> "; envp[2] = 0;
        dup2(sin[0], 0);
        dup2(sout[1], 1);
        dup2(sout[1], 2);
        close(sin[0]); close(sout[0]); close(sout[1]);
        execve("/bin/oksh.mzx", argv, envp);
        _exit(127);
    }

    close(sin[0]); close(sout[1]);
    for (;;) {
        n = read(sout[0], buf + total, (long)(sizeof buf - 1 - (unsigned long)total));
        if (n <= 0) { break; }
        total += n;
        if ((unsigned long)total >= sizeof buf - 1) { break; }
    }
    close(sout[0]);
    buf[total] = 0;
    waitpid(pid, &st, 0);

    {
        const char *fail = 0;
        if (!contains(buf, total, "MZPROMPT>")) fail = "no-prompt";
        else if (!contains(buf, total, "/"))    fail = "pwd-not-run";
        else if (!WIFEXITED(st))                fail = "shell-did-not-exit";
        if (fail != 0) {
            printf("oksh-interactive: FAIL %s st=%d captured=[%s]\n", fail, st, buf);
            return 1;
        }
    }

    printf("oksh-interactive: PASS\n");
    return 0;
}
