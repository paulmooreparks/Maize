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
 * AC 9683/9684 substrate, part D: sha512-224sum, sha512-256sum, tsort, uniq and
 * uuencode. uuencode was excluded from every part until maize-393, because its
 * process faulted on the guest RT's missing %o conversion: uuencode.c:67 prints
 * "begin %o %s\n", the unrecognised %o consumed no vararg, and the following %s
 * fetched the mode integer as a pointer, so strlen dereferenced near null. With %o
 * in the formatter the tool runs to completion, and the check below is byte-exact
 * on stdout because the property under test is that no '%' survives into it.
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

    /* maize-393: the tool the missing %o conversion made unshippable. The header
     * line carries st.st_mode & 0777 through the now-working %o, and the expected
     * string below is pinned from an observed run rather than assumed, because what
     * hostfs reports for a seed()-created file is not something this fixture gets to
     * decide. The load-bearing property is that the expected string contains no '%'
     * anywhere: before the fix the header line was "begin %o %s" and the process
     * faulted before reaching the body at all. */
    { char *av[4]; av[0] = "uuencode"; av[1] = "/rw/w2a"; av[2] = "w2a"; av[3] = 0;
      check_output("uuencode", "/bin/uuencode.mzx", av, 0,
                   "begin 644 w2a\n,;&EN93$*;&EN93(*\n`\nend\n"); }

    if (g_fail) {
        printf("wave2-launch-d: FAIL (see above)\n");
        return 1;
    }
    printf("wave2-launch-d: PASS\n");
    return 0;
}
