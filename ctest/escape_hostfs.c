/* maize-114 hostfs acceptance: confinement (doc sections 4, 8).
 *
 * Two escape attempts must both be denied: a `..` traversal out of the mount root,
 * and a host symlink whose target is outside the mount. On Linux openat2's
 * RESOLVE_BENEATH turns both into EXDEV/ELOOP, which the backend maps to EACCES (13);
 * a truly absent path is ENOENT (2). Either negative result counts as denied. A
 * non-negative fd (the escape succeeded) is a failure. The harness creates the escape
 * target and (on Linux) the symlink; on Windows the symlink is simply absent, which
 * still opens as a denied (ENOENT) path, so the fixture stays portable. */
#include "syscall.h"

int main(void) {
    long a = sys_open("/esc/../escape_target.txt", 0 /* O_RDONLY */, 0);
    long b = sys_open("/esc/esclink", 0 /* O_RDONLY */, 0);

    if (a < 0 && b < 0) {
        sys_write(1, "escape: PASS\n", 13);
    } else {
        sys_write(1, "escape: FAIL\n", 13);
        if (a >= 0) {
            sys_close((int)a);
        }
        if (b >= 0) {
            sys_close((int)b);
        }
    }
    return 0;
}
