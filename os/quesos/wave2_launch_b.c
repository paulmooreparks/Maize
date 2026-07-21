/* wave2_launch_b.c -- maize-292 launcher fixture (part B), run UNDER quesOS. See
 * wave2_launch_a.c's header for why this fixture is split (now three ways, parts
 * A/B/C): an earlier single-fixture draft covering all 32 wave-2 tools crashed the
 * whole VM partway through with an "unhandled interrupt: vector 8" (a page fault
 * quesOS has no user-mode recovery for yet). Part A (12 tools) and this part B (10
 * tools) each stay well under whatever fork/pipe-churn threshold triggered it, on
 * their own fresh quesOS boot; part C (the remaining 10) is a separate file.
 *
 * AC 9683/9684 substrate, part B: cmp, cols, comm, cut, fold, head, join, md5sum,
 * paste, rev.
 *
 * expand/unexpand are NOT wave-2 tools this card ships: both call parselist(),
 * whose `estrtonum(p, 1, MIN(LLONG_MAX, SIZE_MAX))` triggers a genuine, pre-existing
 * cproc/qbe backend defect (a 64-bit signed/unsigned ternary mis-promotion;
 * MIN(LLONG_MAX, SIZE_MAX) evaluates to -1, not LLONG_MAX, reproduced minimally and
 * unrelated to anything this card touches), so their default (no -t) invocation
 * exits 1 with "strtonum 8: invalid" instead of passing text through. This is a
 * toolchain-correctness bug, not a stdin/libc gap, and squarely out of this card's
 * scope (decision 9695); flagged in the card's own comments as a candidate for its
 * own dedicated card.
 *
 * Requires /bin mounted and a writable /rw mount seeded with a few small fixture
 * files (seed(), below). Every execve passes a real, valid (if empty) envp array.
 */

#include "unistd.h"
#include "fcntl.h"
#include "sys/wait.h"
#include "syscall.h"   /* _exit */
#include "string.h"    /* strcmp */

int printf(const char *, ...);

static int g_fail = 0;

static int seed(const char *path, const char *content) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    long n;
    if (fd < 0)
        return -1;
    n = write(fd, content, strlen(content));
    close(fd);
    return (n == (long)strlen(content)) ? 0 : -1;
}

static pid_t run_case(const char *path, char *const argv[], char *outbuf, long cap, int *status) {
    int outp[2];
    pid_t pc;
    long got;
    char *envp[1];
    envp[0] = 0;

    if (pipe(outp) != 0)
        return -1;

    pc = fork();
    if (pc < 0)
        return -1;
    if (pc == 0) {
        dup2(outp[1], 1);
        close(outp[0]); close(outp[1]);
        execve(path, argv, envp);
        _exit(127);
    }
    close(outp[1]);

    got = 0;
    if (outbuf && cap > 0) {
        long n;
        while (got < cap - 1 && (n = read(outp[0], outbuf + got, cap - 1 - got)) > 0)
            got += n;
        outbuf[got] = 0;
    } else {
        char scratch[64];
        while (read(outp[0], scratch, sizeof scratch) > 0)
            ;
    }
    close(outp[0]);
    waitpid(pc, status, 0);
    return pc;
}

static void check(const char *name, const char *path, char *const argv[], int expect_exit) {
    int status = 0;
    if (run_case(path, argv, 0, 0, &status) < 0) {
        printf("wave2-launch-b: FAIL %s (fork/pipe setup)\n", name);
        g_fail = 1;
        return;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != expect_exit) {
        printf("wave2-launch-b: FAIL %s (status=0x%x, expected exit %d)\n",
               name, status, expect_exit);
        g_fail = 1;
    }
}

int main(void) {
    seed("/rw/w2a", "line1\nline2\n");
    seed("/rw/w2b", "line1\nline3\n");

    { char *av[4]; av[0] = "cmp"; av[1] = "/rw/w2a"; av[2] = "/rw/w2a"; av[3] = 0;
      check("cmp", "/bin/cmp.mzx", av, 0); }

    { char *av[3]; av[0] = "cols"; av[1] = "/rw/w2a"; av[2] = 0;
      check("cols", "/bin/cols.mzx", av, 0); }

    { char *av[4]; av[0] = "comm"; av[1] = "/rw/w2a"; av[2] = "/rw/w2b"; av[3] = 0;
      check("comm", "/bin/comm.mzx", av, 0); }

    { char *av[5]; av[0] = "cut"; av[1] = "-c"; av[2] = "1"; av[3] = "/rw/w2a"; av[4] = 0;
      check("cut", "/bin/cut.mzx", av, 0); }

    { char *av[3]; av[0] = "fold"; av[1] = "/rw/w2a"; av[2] = 0;
      check("fold", "/bin/fold.mzx", av, 0); }

    { char *av[3]; av[0] = "head"; av[1] = "/rw/w2a"; av[2] = 0;
      check("head", "/bin/head.mzx", av, 0); }

    { char *av[4]; av[0] = "join"; av[1] = "/rw/w2a"; av[2] = "/rw/w2a"; av[3] = 0;
      check("join", "/bin/join.mzx", av, 0); }

    { char *av[3]; av[0] = "md5sum"; av[1] = "/rw/w2a"; av[2] = 0;
      check("md5sum", "/bin/md5sum.mzx", av, 0); }

    { char *av[4]; av[0] = "paste"; av[1] = "/rw/w2a"; av[2] = "/rw/w2b"; av[3] = 0;
      check("paste", "/bin/paste.mzx", av, 0); }

    { char *av[3]; av[0] = "rev"; av[1] = "/rw/w2a"; av[2] = 0;
      check("rev", "/bin/rev.mzx", av, 0); }

    if (g_fail) {
        printf("wave2-launch-b: FAIL (see above)\n");
        return 1;
    }
    printf("wave2-launch-b: PASS\n");
    return 0;
}
