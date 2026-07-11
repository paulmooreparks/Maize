/* maize-114 hostfs acceptance: cat a file from a :ro mount (doc section 8).
 *
 * Opens /ro/payload.txt (a :ro mount the run-ctest harness grants over a host
 * fixture directory), reads it in a loop, and reproduces its bytes on stdout
 * byte-for-byte. The harness compares stdout against the exact bytes it wrote into
 * the host fixture. Calls the raw sys_* stubs directly (operator ruling OQ 7851);
 * a [-4095,-1] result on open is a failure. */
#include "syscall.h"

int main(void) {
    long fd = sys_open("/ro/payload.txt", 0 /* O_RDONLY */, 0);
    if (fd < 0) {
        sys_write(2, "cat: open failed\n", 17);
        return 1;
    }
    char buf[512];
    for (;;) {
        long n = sys_read((int)fd, buf, sizeof buf);
        if (n <= 0) {
            break;
        }
        sys_write(1, buf, (unsigned long)n);
    }
    sys_close((int)fd);
    return 0;
}
