/* wave2_stdin_pipe.c -- maize-292 launcher fixture (AC 9685/9686), run UNDER
 * quesOS, modeled directly on maize-94's oksh_shell.c (execve /bin/oksh.mzx
 * non-interactively with `-c <script>`, capture stdout through a real pipe).
 *
 * AC 9685: at least one Group-B tool must read stdin end to end through oksh's
 * REAL pipeline machinery, not merely be invoked standalone on an unconnected fd 0.
 * `printf 'a\na\nb\n' | uniq` proves it: oksh forks printf and uniq, wires printf's
 * stdout to uniq's stdin via a real pipe, and uniq's dedup only works if it actually
 * read the piped bytes (a standalone invocation with no upstream writer would just
 * block on empty stdin forever, which this is not).
 *
 * AC 9686: `printf 'abc' | md5sum` exercises libutil/crypt.c's stdin path (fd 0,
 * "<stdin>" label) specifically, checked against the well-known md5("abc") digest.
 *
 * Requires /bin mounted (:ro) with oksh.mzx + printf.mzx (wave-1) + uniq.mzx +
 * md5sum.mzx (this card's wave-2 set).
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
    static const char *script =
        "printf 'a\\na\\nb\\n' | uniq; "
        "printf abc | md5sum";
    char *argv[4];
    char *envp[2];
    int p[2];
    char buf[512];
    long total = 0, n;
    int st = 0;
    pid_t pid;

    if (pipe(p) != 0) { printf("wave2-stdin-pipe: FAIL pipe\n"); return 1; }

    pid = fork();
    if (pid < 0) { printf("wave2-stdin-pipe: FAIL fork\n"); return 1; }
    if (pid == 0) {
        argv[0] = "oksh"; argv[1] = "-c"; argv[2] = (char *)script; argv[3] = 0;
        envp[0] = "PATH=/bin"; envp[1] = 0;
        dup2(p[1], 1);
        close(p[0]); close(p[1]);
        execve("/bin/oksh.mzx", argv, envp);
        _exit(127);
    }

    close(p[1]);
    for (;;) {
        n = read(p[0], buf + total, (long)(sizeof buf - 1 - (unsigned long)total));
        if (n <= 0) { break; }
        total += n;
        if ((unsigned long)total >= sizeof buf - 1) { break; }
    }
    close(p[0]);
    buf[total] = 0;
    waitpid(pid, &st, 0);

    {
        const char *fail = 0;
        /* uniq's real-pipe dedup: "a\na\nb\n" in -> "a\nb\n" out (AC 9685). */
        if (!contains(buf, total, "a\nb\n"))
            fail = "uniq-pipe-dedup";
        /* md5("abc") = 900150983cd24fb0d6963f7d28e17f72 (AC 9686). */
        else if (!contains(buf, total, "900150983cd24fb0d6963f7d28e17f72"))
            fail = "md5sum-stdin-digest";
        else if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
            fail = "shell-exit-status";
        if (fail != 0) {
            printf("wave2-stdin-pipe: FAIL %s st=%d captured=[%s]\n", fail, st, buf);
            return 1;
        }
    }

    printf("wave2-stdin-pipe: PASS\n");
    return 0;
}
