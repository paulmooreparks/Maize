/* rm_launch.c -- maize-94 launcher fixture (decision 9078), run UNDER quesOS.
 *
 * Standalone (no-shell) AC-8935 substrate for the arg-taking sbase `rm`. Writes a
 * file into the writable /rw mount, forks and execve's /bin/rm.mzx to remove it,
 * waits, then checks the file is gone (a fresh open fails). Proves rm's plain-file
 * unlink path end to end. Requires /bin (rm.mzx) and a writable /rw mount. Prints
 * "rm-launch: PASS" or a FAIL marker naming the failing step.
 */

#include "unistd.h"
#include "fcntl.h"
#include "sys/wait.h"
#include "syscall.h"   /* _exit */

int printf(const char *, ...);

static const char PAYLOAD[] = "rm-payload\n";
#define PLEN (sizeof(PAYLOAD) - 1)

int main(void) {
    int fd;
    int st;
    pid_t pc;

    if ((fd = open("/rw/rm_tgt.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0) {
        printf("rm-launch: FAIL create-tgt\n"); return 1;
    }
    if (write(fd, PAYLOAD, PLEN) != (long)PLEN) { printf("rm-launch: FAIL write-tgt\n"); return 1; }
    close(fd);

    /* Sanity: the file exists before rm. */
    if ((fd = open("/rw/rm_tgt.txt", O_RDONLY)) < 0) {
        printf("rm-launch: FAIL tgt-missing-pre\n"); return 1;
    }
    close(fd);

    pc = fork();
    if (pc < 0) { printf("rm-launch: FAIL fork\n"); return 1; }
    if (pc == 0) {
        char *argv[3];
        argv[0] = "rm";
        argv[1] = "/rw/rm_tgt.txt";
        argv[2] = 0;
        execve("/bin/rm.mzx", argv, 0);
        _exit(127);
    }
    waitpid(pc, &st, 0);

    /* The file must be gone after rm. */
    if ((fd = open("/rw/rm_tgt.txt", O_RDONLY)) >= 0) {
        close(fd);
        printf("rm-launch: FAIL tgt-still-present\n");
        return 1;
    }

    printf("rm-launch: PASS\n");
    return 0;
}
