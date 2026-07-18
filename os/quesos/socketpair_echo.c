/* socketpair_echo.c -- maize-238 Family A AC fixture (AC 9187), run UNDER quesOS.
 *
 * Proves AF_UNIX SOCK_STREAM socketpair() full duplex over the cross-wired ring pair:
 *   - socketpair() creates two connected fds;
 *   - a write on one is read back on the other in BOTH directions (full duplex);
 *   - closing one end makes the peer's subsequent read return 0 (EOF) and the peer's
 *     subsequent write fail (-EPIPE).
 *
 * Uses the POSIX wrappers from sys/socket.h + unistd read/write/close (errno.c), the
 * same surface an unmodified client would link against. Output on success:
 *   socketpair-echo: PASS
 */
#include "sys/socket.h"

int  printf(const char *, ...);
long read(int fd, void *buf, unsigned long count);
long write(int fd, const void *buf, unsigned long count);
int  close(int fd);

int main(void) {
    int sv[2];
    char buf[16];
    long n;

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        printf("socketpair-echo: FAIL create\n");
        return 0;
    }

    /* Direction A: sv[0] -> sv[1]. */
    if (write(sv[0], "ping", 4) != 4) { printf("socketpair-echo: FAIL writeA\n"); return 0; }
    n = read(sv[1], buf, sizeof buf);
    if (n != 4 || buf[0] != 'p' || buf[1] != 'i' || buf[2] != 'n' || buf[3] != 'g') {
        printf("socketpair-echo: FAIL readA\n");
        return 0;
    }

    /* Direction B: sv[1] -> sv[0] (proves full duplex, not a one-way tube). */
    if (write(sv[1], "PONG", 4) != 4) { printf("socketpair-echo: FAIL writeB\n"); return 0; }
    n = read(sv[0], buf, sizeof buf);
    if (n != 4 || buf[0] != 'P' || buf[1] != 'O' || buf[2] != 'N' || buf[3] != 'G') {
        printf("socketpair-echo: FAIL readB\n");
        return 0;
    }

    /* Close sv[1]: sv[0] now sees EOF on read and -EPIPE on write. */
    close(sv[1]);
    n = read(sv[0], buf, sizeof buf);
    if (n != 0) { printf("socketpair-echo: FAIL not-eof\n"); return 0; }
    n = write(sv[0], "x", 1);
    if (n >= 0) { printf("socketpair-echo: FAIL not-epipe\n"); return 0; }

    printf("socketpair-echo: PASS\n");
    return 0;
}
