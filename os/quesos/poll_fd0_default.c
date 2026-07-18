/* poll_fd0_default.c -- maize-238 Branch A AC fixture (AC 9199), run UNDER quesOS on the
 * plain DEFAULT invocation (no --input flag). Proves poll()/select() on fd 0 wake on
 * console input on the operator's primary invocation (previously this deadlocked: the
 * default path never drove g_con_count). poll(fd0, POLLIN, -1) blocks until a keystroke
 * arrives via the migrated IRQ/readiness model, then a read drains it. A second leg uses
 * select() to prove both surfaces. Output on success: poll-fd0-default: PASS
 */
#include "poll.h"
#include "sys/select.h"

int  printf(const char *, ...);
long read(int fd, void *buf, unsigned long count);

int main(void) {
    struct pollfd f;
    char c;
    long r, n;

    f.fd = 0; f.events = POLLIN; f.revents = 0;
    r = poll(&f, 1, -1);   /* blocks until the console byte arrives (no deadlock) */
    if (r < 1 || (f.revents & POLLIN) == 0) { printf("poll-fd0-default: FAIL poll\n"); return 0; }
    n = read(0, &c, 1);
    if (n != 1) { printf("poll-fd0-default: FAIL read\n"); return 0; }

    /* Second byte via select(), proving the same readiness path drives select on fd 0. */
    {
        fd_set rf;
        FD_ZERO(&rf); FD_SET(0, &rf);
        r = select(1, &rf, 0, 0, 0);
        if (r < 1 || !FD_ISSET(0, &rf)) { printf("poll-fd0-default: FAIL select\n"); return 0; }
        n = read(0, &c, 1);
        if (n != 1) { printf("poll-fd0-default: FAIL read2\n"); return 0; }
    }

    printf("poll-fd0-default: PASS\n");
    return 0;
}
