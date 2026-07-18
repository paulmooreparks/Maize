/* unix_listen_close.c -- maize-238 Family A AC fixture (AC 9189), run UNDER quesOS.
 *
 * Proves listen-socket close semantics: closing an OFD_SOCK_LISTEN while a connector is
 * still parked in its pending queue wakes that connector with -ECONNREFUSED, and the bound
 * path becomes available for a fresh bind() afterward. A sync pipe orders the child's
 * parked connect() before the parent's close(). Output on success:
 *   unix-listen-close: PASS
 */
#include "sys/socket.h"
#include "sys/un.h"

int  printf(const char *, ...);
long sys_fork(void);
long sys_pipe(int *fds);
long read(int fd, void *buf, unsigned long count);
long write(int fd, const void *buf, unsigned long count);
int  close(int fd);
long sys_wait4(long pid, int *status, long options, long rusage);

static void set_addr(struct sockaddr_un *a, const char *p) {
    int i;
    a->sun_family = AF_UNIX;
    for (i = 0; p[i]; ++i) { a->sun_path[i] = p[i]; }
    a->sun_path[i] = 0;
}

int main(void) {
    struct sockaddr_un addr;
    int sync[2];
    long ls, pid, ls2, rc2;
    char c;
    set_addr(&addr, "/x238b.sock");

    sys_pipe(sync);
    ls = socket(AF_UNIX, SOCK_STREAM, 0);
    if (bind(ls, (const struct sockaddr *)&addr, sizeof addr) != 0) {
        printf("unix-listen-close: FAIL bind\n"); return 0;
    }
    if (listen(ls, 4) != 0) { printf("unix-listen-close: FAIL listen\n"); return 0; }

    pid = sys_fork();
    if (pid == 0) {
        long cs, rc;
        close(sync[0]);
        close(ls);                       /* drop the child's inherited listen ref */
        write(sync[1], "1", 1);          /* signal: about to connect */
        cs = socket(AF_UNIX, SOCK_STREAM, 0);
        rc = connect(cs, (const struct sockaddr *)&addr, sizeof addr);  /* parks */
        write(sync[1], (rc < 0) ? "R" : "X", 1);   /* R = refused (expected) */
        return 0;
    }

    close(sync[1]);
    read(sync[0], &c, 1);                /* "1": the child is now parked in connect() */
    close(ls);                           /* last listen ref: wakes the connector -ECONNREFUSED */
    read(sync[0], &c, 1);                /* "R": the child's connect() was refused */

    ls2 = socket(AF_UNIX, SOCK_STREAM, 0);
    rc2 = bind(ls2, (const struct sockaddr *)&addr, sizeof addr);   /* path must be free again */

    printf((c == 'R' && rc2 == 0) ? "unix-listen-close: PASS\n"
                                  : "unix-listen-close: FAIL\n");
    { int st; sys_wait4(pid, &st, 0, 0); }
    return 0;
}
