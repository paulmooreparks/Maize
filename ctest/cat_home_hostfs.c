/* maize-252 hostfs acceptance: cat a file from the --mount-home / config
 * mount-home= grant (doc section 1, card maize-252). Modeled directly on
 * ctest/cat_hostfs.c, differing only in the fixed guest path: --mount-home and
 * its config-file equivalent always grant /home/user (never a caller-chosen
 * guest path), so this fixture reads /home/user/payload.txt rather than
 * /ro/payload.txt. The harness compares stdout against the exact bytes it
 * wrote into whichever host directory the mount-home grant resolved to; a
 * missing/unmounted grant (e.g. mount-home=false, an explicit no-op) surfaces
 * as the "cat: open failed" line and a nonzero exit, which the harness also
 * asserts on for that case. */
#include "syscall.h"

int main(void) {
    long fd = sys_open("/home/user/payload.txt", 0 /* O_RDONLY */, 0);
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
