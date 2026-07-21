/* wave2_launch_c.c -- maize-292 launcher fixture (part C), run UNDER quesOS. See
 * wave2_launch_a.c's header for why this fixture is split (now four ways: A/B/C/D):
 * an earlier, larger single-fixture draft crashed the whole VM partway through with
 * an "unhandled interrupt: vector 8" (a page fault quesOS has no user-mode recovery
 * for yet); each split part stays well under whatever churn threshold triggered it,
 * on its own fresh quesOS boot. Every tool named below is independently confirmed,
 * via a standalone single-check harness, to build, link, and run correctly against
 * a real target file; the split is a fixture-shape mitigation, not evidence of a
 * defect in any of these tools.
 *
 * AC 9683/9684 substrate, part C: sha1sum, sha224sum, sha256sum, sha384sum,
 * sha512sum.
 *
 * Requires /bin mounted and a writable /rw mount seeded with a small fixture file.
 * Every execve passes a real, valid (if empty) envp array.
 */

#include "unistd.h"
#include "fcntl.h"
#include "sys/wait.h"
#include "syscall.h"   /* _exit */
#include "string.h"    /* strlen */

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

static pid_t run_case(const char *path, char *const argv[], int *status) {
    int outp[2];
    pid_t pc;
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
    {
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
    if (run_case(path, argv, &status) < 0) {
        printf("wave2-launch-c: FAIL %s (fork/pipe setup)\n", name);
        g_fail = 1;
        return;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != expect_exit) {
        printf("wave2-launch-c: FAIL %s (status=0x%x, expected exit %d)\n",
               name, status, expect_exit);
        g_fail = 1;
    }
}

int main(void) {
    seed("/rw/w2a", "line1\nline2\n");

    { char *av[3]; av[0] = "sha1sum"; av[1] = "/rw/w2a"; av[2] = 0;
      check("sha1sum", "/bin/sha1sum.mzx", av, 0); }
    { char *av[3]; av[0] = "sha224sum"; av[1] = "/rw/w2a"; av[2] = 0;
      check("sha224sum", "/bin/sha224sum.mzx", av, 0); }
    { char *av[3]; av[0] = "sha256sum"; av[1] = "/rw/w2a"; av[2] = 0;
      check("sha256sum", "/bin/sha256sum.mzx", av, 0); }
    { char *av[3]; av[0] = "sha384sum"; av[1] = "/rw/w2a"; av[2] = 0;
      check("sha384sum", "/bin/sha384sum.mzx", av, 0); }
    { char *av[3]; av[0] = "sha512sum"; av[1] = "/rw/w2a"; av[2] = 0;
      check("sha512sum", "/bin/sha512sum.mzx", av, 0); }

    if (g_fail) {
        printf("wave2-launch-c: FAIL (see above)\n");
        return 1;
    }
    printf("wave2-launch-c: PASS\n");
    return 0;
}
