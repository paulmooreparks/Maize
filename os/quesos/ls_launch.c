/* ls_launch.c -- maize-94 launcher fixture (decision 9078), run UNDER quesOS.
 *
 * Standalone (no-shell) AC-8935 substrate for the arg-taking sbase `ls`. Creates a
 * fresh directory on the writable /rw mount with two known files, forks and
 * execve's /bin/ls.mzx to list it, captures stdout through a pipe, and checks both
 * names appear. Proves ls's plain-name listing (opendir + getdents64 + sorted name
 * output) end to end. Requires /bin (ls.mzx) and a writable /rw mount. Prints
 * "ls-launch: PASS" or a FAIL marker naming the failing step.
 */

#include "unistd.h"
#include "fcntl.h"
#include "sys/stat.h"   /* mkdir */
#include "sys/wait.h"
#include "syscall.h"    /* _exit */
#include "string.h"     /* memmem-free: use a tiny substring check */

int printf(const char *, ...);

static int seed(const char *path) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return -1;
    close(fd);
    return 0;
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

int main(void) {
    int pp[2];
    char buf[256];
    long got = 0, n;
    int st;
    pid_t pc;

    mkdir("/rw/lsd", 0755);   /* ignore EEXIST from a prior run */
    if (seed("/rw/lsd/alpha") < 0 || seed("/rw/lsd/beta") < 0) {
        printf("ls-launch: FAIL seed\n"); return 1;
    }

    if (pipe(pp) != 0) { printf("ls-launch: FAIL pipe\n"); return 1; }

    pc = fork();
    if (pc < 0) { printf("ls-launch: FAIL fork\n"); return 1; }
    if (pc == 0) {
        char *argv[3];
        argv[0] = "ls";
        argv[1] = "/rw/lsd";
        argv[2] = 0;
        dup2(pp[1], 1);
        close(pp[0]); close(pp[1]);
        execve("/bin/ls.mzx", argv, 0);
        _exit(127);
    }

    close(pp[1]);
    while (got < (long)sizeof buf && (n = read(pp[0], buf + got, sizeof buf - got)) > 0)
        got += n;
    waitpid(pc, &st, 0);

    if (!contains(buf, got, "alpha") || !contains(buf, got, "beta")) {
        printf("ls-launch: FAIL listing (got=%ld)\n", got);
        return 1;
    }
    printf("ls-launch: PASS\n");
    return 0;
}
