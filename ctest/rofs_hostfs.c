/* maize-114 hostfs acceptance: read-only enforcement (doc sections 2, 4, 8).
 *
 * On a :ro mount every write-intent op returns EROFS (30), while O_RDONLY|O_APPEND
 * (append alone is not write intent) succeeds. The synthetic root is likewise never
 * writable. Checks, all against a :ro mount /ro the harness grants:
 *   1. open O_WRONLY            -> EROFS
 *   2. open O_RDONLY|O_APPEND   -> succeeds
 *   3. write() on a RDONLY fd   -> EROFS
 *   4. open "/" O_WRONLY        -> EROFS (synthetic root)
 * Prints a single PASS/FAIL line. */
#include "syscall.h"

int main(void) {
    int ok = 1;

    long a = sys_open("/ro/payload.txt", 1 /* O_WRONLY */, 0);
    if (!(a < 0 && (int)(-a) == 30)) {
        ok = 0;
        if (a >= 0) { sys_close((int)a); }
    }

    long b = sys_open("/ro/payload.txt", 0x400 /* O_RDONLY|O_APPEND */, 0);
    if (b < 0) {
        ok = 0;
    }

    long fd = sys_open("/ro/payload.txt", 0 /* O_RDONLY */, 0);
    if (fd >= 0) {
        long w = sys_write((int)fd, "x", 1);
        if (!(w < 0 && (int)(-w) == 30)) {
            ok = 0;
        }
    } else {
        ok = 0;
    }

    long root = sys_open("/", 1 /* O_WRONLY */, 0);
    if (!(root < 0 && (int)(-root) == 30)) {
        ok = 0;
        if (root >= 0) { sys_close((int)root); }
    }

    if (b >= 0) { sys_close((int)b); }
    if (fd >= 0) { sys_close((int)fd); }

    sys_write(1, ok ? "rofs: PASS\n" : "rofs: FAIL\n", 11);
    return 0;
}
