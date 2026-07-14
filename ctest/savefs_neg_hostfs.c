/* maize-151 hostfs security: the path-mutating syscalls honor the write-gate and cannot
 * escape a mount. Run against a default sandbox root (mounted "/" rw) plus a :ro overlay
 * at /ro (NOT --no-root), the guest-observable guarantees are:
 *   1. mkdir on a :ro mount           -> EROFS (30)
 *   2. rename on a :ro mount          -> EROFS (30)
 *   3. a `..` path that would climb to a host file OUTSIDE every mount is denied: the
 *      core's normalization clamps `..` at "/", so /ro/../escape_target.txt resolves to
 *      /escape_target.txt inside the sandbox (where no such file exists) rather than the
 *      /ro mount's host parent, and the open fails.
 *   4. a `..`-laden mkdir that tries to climb above the sandbox root cannot create a host
 *      entry outside it (the harness verifies the host escape locations stay empty).
 * Prints a single PASS/FAIL line for the guest-observable checks; the harness adds the
 * host-side containment assertions. */
#include "syscall.h"

#define O_RDONLY 0

int main(void) {
    int ok = 1;

    /* 1. mkdir on the :ro mount is rejected with EROFS before any host touch. */
    long a = sys_mkdir("/ro/newdir", 0755);
    if (!(a < 0 && (int)(-a) == 30)) {
        ok = 0;
    }

    /* 2. rename on the :ro mount is likewise EROFS. */
    long b = sys_rename("/ro/a.txt", "/ro/b.txt");
    if (!(b < 0 && (int)(-b) == 30)) {
        ok = 0;
    }

    /* 3. `..` cannot climb from /ro into the /ro mount's host parent directory. */
    long c = sys_open("/ro/../escape_target.txt", O_RDONLY, 0);
    if (c >= 0) {
        ok = 0;
        sys_close((int)c);
    }

    /* 4. A `..`-laden mkdir aimed above the sandbox root: normalization clamps it, so it
       can only ever land inside the sandbox, never at the host location the harness
       probes. The return code is not asserted here (it may be a contained success or a
       missing-parent ENOENT); the harness proves nothing was created OUTSIDE the
       sandbox on the host. */
    (void)sys_mkdir("/ro/../../pwned", 0755);

    sys_write(1, ok ? "savefsneg: PASS\n" : "savefsneg: FAIL\n", 16);
    return 0;
}
