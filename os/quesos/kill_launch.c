/* kill_launch.c -- maize-292 launcher fixture (AC 9687), run UNDER quesOS, modeled
 * on maize-94's sbase_launch.c (fork + pipe + dup2 + execve + waitpid) and maize-174's
 * sig_kill.c (kill/wait4 status decode).
 *
 * Drives the VENDORED, PATCHED /bin/kill.mzx (not the raw kill() syscall) against
 * real target processes:
 *   - kill -TERM <pid>, kill -9 <pid>, kill -HUP <pid>: each spawns a busy-loop
 *     target, execve's kill.mzx with the signal name/number against its real pid,
 *     and checks the target was reaped WIFSIGNALED with the matching WTERMSIG.
 *   - kill -0 <pid> against a real, live target: quesOS's do_kill rejects any signal
 *     outside 1..31 (decision 9696), so this must NOT crash or silently misreport;
 *     the patch (0007-kill-sig0-existence.patch) makes kill.mzx print a clear
 *     "process-existence check ... is not supported" message on stderr and exit
 *     nonzero. Both are checked. The live target is then reaped with a real SIGTERM
 *     so nothing is left running.
 *
 * Requires /bin mounted (kill.mzx). Prints "kill-launch: PASS" or a FAIL marker
 * naming the failing step.
 */

#include "unistd.h"
#include "fcntl.h"
#include "sys/wait.h"
#include "signal.h"
#include "syscall.h"   /* _exit */
#include "string.h"    /* memmem-free: use strstr-free substring check below */

int printf(const char *, ...);

static int g_fail = 0;

/* File-scope, non-volatile busy-loop sink (maize-174's sig_kill.c precedent): the
 * pinned cproc-qbe cannot yet lower a volatile store (build-userland.sh's oksh
 * -D volatile= comment), so a target's infinite "await a signal" loop keeps the
 * compiler from proving it dead by writing through a global instead of a local
 * `volatile`, exactly as sig_kill.c's own g_sink does. */
static long g_sink;

/* Minimal unsigned-to-decimal (no snprintf dependency): writes into buf, returns
 * a pointer to the first digit (buf's tail holds the NUL-terminated string). */
static char *utoa(unsigned long v, char *buf, int buflen) {
    char *p = buf + buflen - 1;
    *p = 0;
    if (v == 0) { *--p = '0'; return p; }
    while (v > 0) { *--p = (char)('0' + (v % 10)); v /= 10; }
    return p;
}

/* Does haystack (length n) contain the NUL-terminated needle? */
static int contains(const char *hay, long n, const char *needle) {
    long i, j;
    for (i = 0; i < n; i++) {
        for (j = 0; needle[j] && i + j < n && hay[i + j] == needle[j]; j++)
            ;
        if (!needle[j])
            return 1;
    }
    return 0;
}

/* execve /bin/kill.mzx with argv {"kill", sigarg, pidstr, NULL} against `target`,
 * capturing stderr. Returns the killer's own exit status via *kstatus and fills
 * errbuf (NUL-terminated) with up to errcap-1 bytes of its stderr. */
static pid_t run_kill(const char *sigarg, pid_t target, char *errbuf, long errcap, int *kstatus) {
    char pidbuf[24];
    char *pidstr = utoa((unsigned long)target, pidbuf, sizeof pidbuf);
    char *argv[4];
    /* Real, valid (if empty) envp; see wave2_launch.c's run_case for why a literal
     * NULL envp is unsafe (leaves `environ` itself NULL for any tool that reads it). */
    char *envp[1];
    int errp[2];
    pid_t pk;
    long got;

    argv[0] = "kill"; argv[1] = (char *)sigarg; argv[2] = pidstr; argv[3] = 0;
    envp[0] = 0;

    if (pipe(errp) != 0)
        return -1;
    pk = fork();
    if (pk < 0)
        return -1;
    if (pk == 0) {
        close(errp[0]);
        dup2(errp[1], 2);
        close(errp[1]);
        execve("/bin/kill.mzx", argv, envp);
        _exit(127);
    }
    close(errp[1]);
    got = 0;
    if (errbuf && errcap > 0) {
        long n;
        while (got < errcap - 1 && (n = read(errp[0], errbuf + got, errcap - 1 - got)) > 0)
            got += n;
        errbuf[got] = 0;
    } else {
        char scratch[64];
        while (read(errp[0], scratch, sizeof scratch) > 0)
            ;
    }
    close(errp[0]);
    waitpid(pk, kstatus, 0);
    return pk;
}

/* One real-signal case: spawn a fresh target, kill.mzx -SIGARG <pid>, verify the
 * target was reaped WIFSIGNALED with WTERMSIG == expect_sig. */
static void check_signal(const char *label, const char *sigarg, int expect_sig) {
    int rp[2];
    pid_t target, killer;
    int kstatus = 0, tstatus = 0;
    char c;

    if (pipe(rp) != 0) { printf("kill-launch: FAIL %s (pipe)\n", label); g_fail = 1; return; }
    target = fork();
    if (target < 0) { printf("kill-launch: FAIL %s (fork)\n", label); g_fail = 1; return; }
    if (target == 0) {
        long k;
        close(rp[0]);
        write(rp[1], "R", 1);
        for (k = 0; ; ++k) { g_sink = g_sink + k; }
        _exit(0);
    }
    close(rp[1]);
    read(rp[0], &c, 1);   /* target is running */
    close(rp[0]);

    killer = run_kill(sigarg, target, 0, 0, &kstatus);
    if (killer < 0 || !WIFEXITED(kstatus) || WEXITSTATUS(kstatus) != 0) {
        printf("kill-launch: FAIL %s (kill.mzx itself did not exit 0)\n", label);
        g_fail = 1;
    }

    waitpid(target, &tstatus, 0);
    if (!WIFSIGNALED(tstatus) || WTERMSIG(tstatus) != expect_sig) {
        printf("kill-launch: FAIL %s (target status=0x%x, expected signal %d)\n",
               label, tstatus, expect_sig);
        g_fail = 1;
    }
}

int main(void) {
    int rp[2];
    pid_t target, killer;
    int kstatus = 0, tstatus = 0;
    char errbuf[256];
    char c;

    check_signal("TERM", "-TERM", 15);
    check_signal("KILL", "-9", 9);
    check_signal("HUP", "-HUP", 1);

    /* kill -0 against a real, live target: decision 9696's honest-deviation path. */
    if (pipe(rp) != 0) { printf("kill-launch: FAIL sig0 (pipe)\n"); return 1; }
    target = fork();
    if (target == 0) {
        long k;
        close(rp[0]);
        write(rp[1], "R", 1);
        for (k = 0; ; ++k) { g_sink = g_sink + k; }
        _exit(0);
    }
    close(rp[1]);
    read(rp[0], &c, 1);
    close(rp[0]);

    killer = run_kill("-0", target, errbuf, (long)sizeof errbuf, &kstatus);
    if (killer < 0 || !WIFEXITED(kstatus) || WEXITSTATUS(kstatus) == 0) {
        printf("kill-launch: FAIL sig0 (expected a nonzero, defined exit; got 0x%x)\n", kstatus);
        g_fail = 1;
    }
    if (!contains(errbuf, (long)strlen(errbuf), "not supported")) {
        printf("kill-launch: FAIL sig0 (expected the documented deviation message, got: %s)\n", errbuf);
        g_fail = 1;
    }

    /* Reap the still-live target with a real signal so nothing is left running. */
    kill(target, SIGTERM);
    waitpid(target, &tstatus, 0);

    if (g_fail) {
        printf("kill-launch: FAIL (see above)\n");
        return 1;
    }
    printf("kill-launch: PASS\n");
    return 0;
}
