/* wave2_launch_a.c -- maize-292 launcher fixture (part A), run UNDER quesOS, modeled
 * on maize-94's sbase_launch.c/ls_launch.c (fork + pipe + dup2 + execve + waitpid).
 *
 * Split from a single larger fixture into two (part A here: the 12 non-sum/non-
 * classifier tools; part B in wave2_launch_b.c: the remaining 20) after an earlier
 * single-fixture draft covering all 32 tools crashed the whole VM partway through
 * with an uncaught "unhandled interrupt: vector 8" (a page fault under quesOS's
 * paging model, cause 8 per docs/spec/trap-model.md); quesOS has no user-mode page-
 * fault recovery yet, so ANY guest fault (this fixture's own or an sbase tool's)
 * halts the whole VM rather than killing just the faulting process. Splitting into
 * two smaller worklist programs, each its own quesOS boot (hence its own fresh
 * process/pipe-pool state), keeps each fixture's own fork/pipe churn well below
 * whatever threshold triggered it, and is closer to the wave-1 precedent's OWN
 * shape (many small fixtures, not one large one) besides.
 *
 * AC 9683/9684 substrate, part A: basename, dirname, cal, cksum, logname, mkdir,
 * printenv, sleep, sponge, tee, unlink, yes.
 *
 * Two deliberate non-zero-exit exceptions, both pre-existing, honest quesOS
 * deviations rather than defects this card introduces or must paper over:
 *   - logname: quesOS has no login/user database (toolchain/rt/unistd.c's own
 *     getlogin() comment), so getlogin() always returns NULL and logname always
 *     eprintf's "no login name" and exits 1. Same class as decision 9698's already-
 *     accepted cal sentinel-date deviation: the tool builds, links, and runs; its
 *     environment-shaped answer is honestly wrong, not faked.
 *   - yes: runs forever by design (no file/stdin dependency, no natural exit). Its
 *     own case below reads a few bytes of the repeating output then SIGKILLs it
 *     rather than waiting on an exit status.
 *
 * Requires /bin mounted (the wave-1 + wave-2 + oksh set), a writable /rw mount
 * seeded with a small fixture file, and a writable /tmp mount (sponge.c's mkstemp
 * target is hardcoded to /tmp regardless of its own file argument). Every execve
 * passes a real, valid (if empty) envp array, `{ 0 }`, never a literal NULL: some
 * wave-2 tools (printenv) dereference `environ` unconditionally, and a literal NULL
 * envp leaves `environ` itself NULL, which crashes on the first dereference (see
 * the Convention counterexamples doc, Entry 22).
 */

#include "unistd.h"
#include "fcntl.h"
#include "sys/wait.h"
#include "signal.h"
#include "syscall.h"   /* _exit */
#include "string.h"    /* strcmp, memcmp */

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
        printf("wave2-launch-a: FAIL %s (fork/pipe setup)\n", name);
        g_fail = 1;
        return;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != expect_exit) {
        printf("wave2-launch-a: FAIL %s (status=0x%x, expected exit %d)\n",
               name, status, expect_exit);
        g_fail = 1;
    }
}

static void check_output(const char *name, const char *path, char *const argv[],
                          int expect_exit, const char *expect_out) {
    char out[256];
    int status = 0;
    if (run_case(path, argv, out, (long)sizeof out, &status) < 0) {
        printf("wave2-launch-a: FAIL %s (fork/pipe setup)\n", name);
        g_fail = 1;
        return;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != expect_exit) {
        printf("wave2-launch-a: FAIL %s (status=0x%x, expected exit %d)\n",
               name, status, expect_exit);
        g_fail = 1;
        return;
    }
    if (strcmp(out, expect_out) != 0) {
        printf("wave2-launch-a: FAIL %s (output mismatch, got %s)\n", name, out);
        g_fail = 1;
    }
}

/* yes: runs forever by design. Read a few bytes of the repeating "y\n" output
 * through a real pipe (proving it starts and produces the expected content), then
 * SIGKILL it and reap it: PASS is "content matched, process was killable", not an
 * exit status (yes has none to give voluntarily). */
static void check_yes(void) {
    int outp[2];
    pid_t pc;
    char buf[8];
    long got = 0, n;
    int status;
    char *argv[2];
    char *envp[1];
    argv[0] = "yes"; argv[1] = 0;
    envp[0] = 0;

    if (pipe(outp) != 0) {
        printf("wave2-launch-a: FAIL yes (pipe)\n"); g_fail = 1; return;
    }

    pc = fork();
    if (pc < 0) { printf("wave2-launch-a: FAIL yes (fork)\n"); g_fail = 1; return; }
    if (pc == 0) {
        dup2(outp[1], 1);
        close(outp[0]); close(outp[1]);
        execve("/bin/yes.mzx", argv, envp);
        _exit(127);
    }
    close(outp[1]);

    while (got < 6 && (n = read(outp[0], buf + got, sizeof buf - got)) > 0)
        got += n;
    close(outp[0]);
    kill(pc, SIGKILL);
    waitpid(pc, &status, 0);

    if (got < 6 || memcmp(buf, "y\ny\ny\n", 6) != 0) {
        printf("wave2-launch-a: FAIL yes (output mismatch, got %ld bytes)\n", got);
        g_fail = 1;
    }
}

int main(void) {
    char *a1[2], *a2[2];

    seed("/rw/w2a", "line1\nline2\n");

    a2[0] = "basename"; a2[1] = "/a/b/c";
    check_output("basename", "/bin/basename.mzx", a2, 0, "c\n");

    a2[0] = "dirname"; a2[1] = "/a/b/c";
    check_output("dirname", "/bin/dirname.mzx", a2, 0, "/a/b\n");

    a1[0] = "cal"; a1[1] = 0;
    check("cal", "/bin/cal.mzx", a1, 0);

    { char *av[3]; av[0] = "cksum"; av[1] = "/rw/w2a"; av[2] = 0;
      check("cksum", "/bin/cksum.mzx", av, 0); }

    a1[0] = "logname"; a1[1] = 0;
    check("logname", "/bin/logname.mzx", a1, 1); /* quesOS has no login db: honest exit 1 */

    { char *av[3]; av[0] = "mkdir"; av[1] = "/rw/w2mkdirtest"; av[2] = 0;
      check("mkdir", "/bin/mkdir.mzx", av, 0); }

    a1[0] = "printenv"; a1[1] = 0;
    check("printenv", "/bin/printenv.mzx", a1, 0);

    { char *av[3]; av[0] = "sleep"; av[1] = "0"; av[2] = 0;
      check("sleep", "/bin/sleep.mzx", av, 0); }

    { char *av[3]; av[0] = "sponge"; av[1] = "/rw/w2spongeout"; av[2] = 0;
      check("sponge", "/bin/sponge.mzx", av, 0); }

    { char *av[3]; av[0] = "tee"; av[1] = "/rw/w2teeout"; av[2] = 0;
      check("tee", "/bin/tee.mzx", av, 0); }

    seed("/rw/w2unlinktarget", "x");
    { char *av[3]; av[0] = "unlink"; av[1] = "/rw/w2unlinktarget"; av[2] = 0;
      check("unlink", "/bin/unlink.mzx", av, 0); }

    check_yes();

    if (g_fail) {
        printf("wave2-launch-a: FAIL (see above)\n");
        return 1;
    }
    printf("wave2-launch-a: PASS\n");
    return 0;
}
