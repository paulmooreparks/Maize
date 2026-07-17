/* cp_launch.c -- maize-94 launcher fixture (decision 9078), run UNDER quesOS.
 *
 * Standalone (no-shell) AC-8935 substrate for the arg-taking sbase `cp`. Writes a
 * source file into the writable /rw mount, forks and execve's /bin/cp.mzx to copy
 * it to a destination, waits, then reads the destination back and checks the bytes
 * match. Proves cp's plain content copy (open + creat + concat) end to end.
 * Requires /bin (cp.mzx) and a writable /rw mount. Prints "cp-launch: PASS" or a
 * FAIL marker naming the failing step.
 */

#include "unistd.h"
#include "fcntl.h"
#include "sys/wait.h"
#include "syscall.h"   /* _exit */
#include "string.h"    /* memcmp */

int printf(const char *, ...);

static const char PAYLOAD[] = "cp-payload\n";
#define PLEN (sizeof(PAYLOAD) - 1)

int main(void) {
    int fd;
    long got, n;
    int st;
    pid_t pc;
    char buf[64];

    /* Seed the source file on the writable mount. */
    if ((fd = open("/rw/cp_src.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0) {
        printf("cp-launch: FAIL create-src\n"); return 1;
    }
    if (write(fd, PAYLOAD, PLEN) != (long)PLEN) { printf("cp-launch: FAIL write-src\n"); return 1; }
    close(fd);

    pc = fork();
    if (pc < 0) { printf("cp-launch: FAIL fork\n"); return 1; }
    if (pc == 0) {
        char *argv[4];
        argv[0] = "cp";
        argv[1] = "/rw/cp_src.txt";
        argv[2] = "/rw/cp_dst.txt";
        argv[3] = 0;
        execve("/bin/cp.mzx", argv, 0);
        _exit(127);
    }
    waitpid(pc, &st, 0);

    if ((fd = open("/rw/cp_dst.txt", O_RDONLY)) < 0) {
        printf("cp-launch: FAIL open-dst\n"); return 1;
    }
    got = 0;
    while (got < (long)sizeof buf && (n = read(fd, buf + got, sizeof buf - got)) > 0)
        got += n;
    close(fd);

    if (got != (long)PLEN || memcmp(buf, PAYLOAD, PLEN) != 0) {
        printf("cp-launch: FAIL content (got=%ld)\n", got);
        return 1;
    }
    printf("cp-launch: PASS\n");
    return 0;
}
