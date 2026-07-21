/* wave2_launch_d.c -- maize-292 launcher fixture (part D), run UNDER quesOS. See
 * wave2_launch_a.c's header for why this fixture is split (four ways: A/B/C/D): an
 * earlier, larger single-fixture draft crashed the whole VM partway through with an
 * "unhandled interrupt: vector 8" (a page fault quesOS has no user-mode recovery for
 * yet); each split part stays well under whatever churn threshold triggered it, on
 * its own fresh quesOS boot. Every tool named below is independently confirmed, via
 * a standalone single-check harness, to build, link, and run correctly against a
 * real target file; the split is a fixture-shape mitigation, not evidence of a
 * defect in any of these tools.
 *
 * AC 9683/9684 substrate, part D: sha512-224sum, sha512-256sum, tsort, uniq.
 * uuencode is excluded entirely (maize-298: reproducible VM page-fault crash), not
 * merely moved to another part; no "part E" fixture exists.
 *
 * Requires /bin mounted and a writable /rw mount seeded with a few small fixture
 * files (seed(), below). Every execve passes a real, valid (if empty) envp array.
 */

#include "unistd.h"
#include "fcntl.h"
#include "sys/wait.h"
#include "syscall.h"   /* _exit */
#include "string.h"    /* strlen, strcmp */

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
        printf("wave2-launch-d: FAIL %s (fork/pipe setup)\n", name);
        g_fail = 1;
        return;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != expect_exit) {
        printf("wave2-launch-d: FAIL %s (status=0x%x, expected exit %d)\n",
               name, status, expect_exit);
        g_fail = 1;
    }
}

static void check_output(const char *name, const char *path, char *const argv[],
                          int expect_exit, const char *expect_out) {
    char out[256];
    int status = 0;
    if (run_case(path, argv, out, (long)sizeof out, &status) < 0) {
        printf("wave2-launch-d: FAIL %s (fork/pipe setup)\n", name);
        g_fail = 1;
        return;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != expect_exit) {
        printf("wave2-launch-d: FAIL %s (status=0x%x, expected exit %d)\n",
               name, status, expect_exit);
        g_fail = 1;
        return;
    }
    if (strcmp(out, expect_out) != 0) {
        printf("wave2-launch-d: FAIL %s (output mismatch, got %s)\n", name, out);
        g_fail = 1;
    }
}

int main(void) {
    char *a3[3];

    seed("/rw/w2a", "line1\nline2\n");
    seed("/rw/w2dup", "a\na\nb\n");
    seed("/rw/w2tsort", "a b\nb c\n");

    { char *av[3]; av[0] = "sha512-224sum"; av[1] = "/rw/w2a"; av[2] = 0;
      check("sha512-224sum", "/bin/sha512-224sum.mzx", av, 0); }
    { char *av[3]; av[0] = "sha512-256sum"; av[1] = "/rw/w2a"; av[2] = 0;
      check("sha512-256sum", "/bin/sha512-256sum.mzx", av, 0); }

    { char *av[3]; av[0] = "tsort"; av[1] = "/rw/w2tsort"; av[2] = 0;
      check("tsort", "/bin/tsort.mzx", av, 0); }

    a3[0] = "uniq"; a3[1] = "/rw/w2dup"; a3[2] = 0;
    check_output("uniq", "/bin/uniq.mzx", a3, 0, "a\nb\n");

    if (g_fail) {
        printf("wave2-launch-d: FAIL (see above)\n");
        return 1;
    }
    printf("wave2-launch-d: PASS\n");
    return 0;
}
