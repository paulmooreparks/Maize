/* unix_listen_connect.c -- maize-238 Family A AC fixture (AC 9188), run UNDER quesOS.
 *
 * Two processes via fork: the parent bind()s + listen()s at a path and forks; the child
 * connect()s to that path and is observably blocked (BLK_CONNECT) until the parent
 * accept()s. Once accepted, data flows both ways. The child sends its reply byte only
 * AFTER connect() returns, and connect() returns only after the parent's accept()
 * completes the handshake -- so the parent reading the child's reply proves connect() did
 * not return before accept() completed. Output on success:
 *   unix-listen-connect: PASS
 */
#include "sys/socket.h"
#include "sys/un.h"

int  printf(const char *, ...);
long sys_fork(void);
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
    long ls, pid, as, n;
    char c;
    set_addr(&addr, "/x238a.sock");

    ls = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ls < 0) { printf("unix-listen-connect: FAIL socket\n"); return 0; }
    if (bind(ls, (const struct sockaddr *)&addr, sizeof addr) != 0) {
        printf("unix-listen-connect: FAIL bind\n"); return 0;
    }
    if (listen(ls, 4) != 0) { printf("unix-listen-connect: FAIL listen\n"); return 0; }

    pid = sys_fork();
    if (pid == 0) {
        long cs = socket(AF_UNIX, SOCK_STREAM, 0);
        if (connect(cs, (const struct sockaddr *)&addr, sizeof addr) != 0) {
            printf("unix-listen-connect: FAIL child-connect\n"); return 0;
        }
        /* connect() has returned, which can only happen after the parent accept()ed. */
        if (read(cs, &c, 1) == 1) { write(cs, "C", 1); }
        return 0;
    }

    as = accept(ls, 0, 0);   /* parks in BLK_ACCEPT until the child connects */
    if (as < 0) { printf("unix-listen-connect: FAIL accept\n"); return 0; }
    write(as, "P", 1);       /* message the child now that the handshake is complete */
    n = read(as, &c, 1);     /* the child's reply, sent only after its connect() returned */
    printf((n == 1 && c == 'C') ? "unix-listen-connect: PASS\n"
                                : "unix-listen-connect: FAIL exchange\n");
    { int st; sys_wait4(pid, &st, 0, 0); }
    return 0;
}
