/* mv_launch.c -- maize-94 launcher fixture (decision 9078), run UNDER quesOS.
 *
 * Standalone (no-shell) AC-8935 substrate for the arg-taking sbase `mv`. Writes a
 * source file into the writable /rw mount, forks and execve's /bin/mv.mzx to rename
 * it, waits, then checks the destination holds the bytes AND the source is gone.
 * Proves mv's rename path end to end. Requires /bin (mv.mzx) and a writable /rw
 * mount. Prints "mv-launch: PASS" or a FAIL marker naming the failing step.
 */

#include "unistd.h"
#include "fcntl.h"
#include "sys/wait.h"
#include "syscall.h"   /* _exit */
#include "string.h"    /* memcmp */

int printf(const char *, ...);

static const char PAYLOAD[] = "mv-payload\n";
#define PLEN (sizeof(PAYLOAD) - 1)

int main(void) {
    int fd;
    long got, n;
    int st;
    pid_t pc;
    char buf[64];

    if ((fd = open("/rw/mv_src.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0) {
        printf("mv-launch: FAIL create-src\n"); return 1;
    }
    if (write(fd, PAYLOAD, PLEN) != (long)PLEN) { printf("mv-launch: FAIL write-src\n"); return 1; }
    close(fd);

    pc = fork();
    if (pc < 0) { printf("mv-launch: FAIL fork\n"); return 1; }
    if (pc == 0) {
        char *argv[4];
        argv[0] = "mv";
        argv[1] = "/rw/mv_src.txt";
        argv[2] = "/rw/mv_dst.txt";
        argv[3] = 0;
        execve("/bin/mv.mzx", argv, 0);
        _exit(127);
    }
    waitpid(pc, &st, 0);

    /* Destination must hold the payload. */
    if ((fd = open("/rw/mv_dst.txt", O_RDONLY)) < 0) {
        printf("mv-launch: FAIL open-dst\n"); return 1;
    }
    got = 0;
    while (got < (long)sizeof buf && (n = read(fd, buf + got, sizeof buf - got)) > 0)
        got += n;
    close(fd);
    if (got != (long)PLEN || memcmp(buf, PAYLOAD, PLEN) != 0) {
        printf("mv-launch: FAIL content (got=%ld)\n", got);
        return 1;
    }

    /* Source must be gone (rename removed it). */
    if ((fd = open("/rw/mv_src.txt", O_RDONLY)) >= 0) {
        close(fd);
        printf("mv-launch: FAIL src-still-present\n");
        return 1;
    }

    printf("mv-launch: PASS\n");
    return 0;
}
