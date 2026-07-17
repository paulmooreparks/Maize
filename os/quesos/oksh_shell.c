/* oksh_shell.c -- maize-94 launcher fixture (decision 9078 shape), run UNDER quesOS.
 *
 * The deterministic, shell-driven proof of the wave-1 shell ACs 8931-8934: it forks and
 * execve's the vendored /bin/oksh.mzx NON-interactively (`oksh -c <script>`, so no console
 * timing and no interactive job control) with PATH=/bin, capturing oksh's stdout through a
 * pipe. The one script drives, FROM oksh:
 *   - AC 8931 pipeline: `echo.mzx AApipe | cat.mzx` (two vendored sbase utils through
 *     oksh's own fork+pipe+dup2+execve+wait4),
 *   - AC 8932 redirection: `> o.txt` (truncate-create) then `>> o.txt` (append) into the
 *     writable /rw mount, read back with the vendored cat.mzx,
 *   - AC 8933 exit status: `false.mzx` then `$?` == 1, `true.mzx` then `$?` == 0, and the
 *     shell's OWN final `exit 7` observed by the parent via waitpid/WEXITSTATUS,
 *   - AC 8934 builtins: `cd /rw` (real chdir, so the later relative `o.txt` resolves under
 *     /rw), `pwd`, `export EE=childenv` made visible to a child's getenv via a NESTED
 *     `oksh -c 'echo child=$EE'` (single-quoted so the child, not the parent, expands it).
 * The parent checks every expected marker is present in the captured output AND that
 * oksh's own exit status is 7, then prints "oksh-shell: PASS" or a FAIL marker.
 *
 * Requires /bin mounted (:ro) with oksh.mzx + echo.mzx + cat.mzx + true.mzx + false.mzx,
 * and a writable /rw mount.
 */

#include "unistd.h"
#include "sys/wait.h"
#include "syscall.h"   /* _exit */
#include "string.h"    /* strlen, strstr */

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
        "cd /rw; "
        "pwd; "
        "export EE=childenv; "
        "echo shellvar=$EE; "
        /* cat / oksh are typed WITHOUT an extension to also prove decision 9084's
         * exact -> .mzx fallback in oksh's own command lookup (echo.mzx is explicit so
         * the pipeline still spans two vendored sbase utilities for AC 8931). */
        "echo.mzx AApipe | cat; "
        "echo r1 > o.txt; "
        "echo r2 >> o.txt; "
        "cat o.txt; "
        "false.mzx; echo sf=$?; "
        "true.mzx; echo st=$?; "
        "oksh -c 'echo child=$EE'; "
        "exit 7";
    char *argv[4];
    char *envp[2];
    int p[2];
    char buf[1024];
    long total = 0, n;
    int st = 0;
    pid_t pid;

    if (pipe(p) != 0) { printf("oksh-shell: FAIL pipe\n"); return 1; }

    pid = fork();
    if (pid < 0) { printf("oksh-shell: FAIL fork\n"); return 1; }
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
        if (!contains(buf, total, "/rw"))                    fail = "cd/pwd";
        else if (!contains(buf, total, "shellvar=childenv")) fail = "export/expand";
        else if (!contains(buf, total, "AApipe"))            fail = "pipeline";
        else if (!contains(buf, total, "r1")
                 || !contains(buf, total, "r2"))             fail = "redirect";
        else if (!contains(buf, total, "sf=1"))              fail = "exit-status-false";
        else if (!contains(buf, total, "st=0"))              fail = "exit-status-true";
        else if (!contains(buf, total, "child=childenv"))    fail = "export-to-child";
        else if (!WIFEXITED(st) || WEXITSTATUS(st) != 7)     fail = "shell-exit-status";
        if (fail != 0) {
            /* Dump the captured transcript + status so a platform-specific failure (e.g.
             * the Windows leg) is diagnosable from the CI log in one shot. */
            printf("oksh-shell: FAIL %s st=%d captured=[%s]\n", fail, st, buf);
            return 1;
        }
    }

    printf("oksh-shell: PASS\n");
    return 0;
}
