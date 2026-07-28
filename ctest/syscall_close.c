/* maize-327: closing a standard descriptor through the VM's native syscall
 * provider succeeds.
 *
 * Before the fix, case $03 in src/sys.cpp routed every fd into hostfs_close,
 * whose slot index is fd - HOSTFS_FD_BASE (3), so fd 0/1/2 fell off the bottom
 * of the table and came back -EBADF (-9) even though the three stdio
 * reservations are validly open. This fixture is a raw-stub fixture in the
 * syscall_raw / syscall_write shape (#include "syscall.h", no libc), so it
 * exercises the native dispatch directly: run-ctest.sh runs every image through
 * a --bare wrapper, which keeps quesOS (whose own ofd_unref already guards
 * native_fd > 2) out of the path.
 *
 * Every line below is written to fd 1 AFTER sys_close(1) has been called, so the
 * stdout diff against syscall_close.expected is itself the proof of the recorded
 * semantics decision (a): closing a std fd is bare success, the reservation is
 * not deallocated, and a subsequent write on the same fd keeps working.
 */
#include "syscall.h"

#define WR(s) sys_write(1, (s), sizeof (s) - 1)

/* rc == 0 is the pass condition; anything else (notably the old -9) prints FAIL,
 * which fails the byte-exact stdout compare. */
static void report(const char *label, unsigned long len, long rc)
{
    sys_write(1, label, len);
    if (rc == 0)
        sys_write(1, " ok\n", 4);
    else
        sys_write(1, " FAIL\n", 6);
}

int main(void)
{
    long rc0, rc1, rc2, rc3;

    rc0 = sys_close(0);
    rc1 = sys_close(1);
    rc2 = sys_close(2);

    /* Boundary check, and the control that keeps the three assertions above from
     * being vacuous: fd 3 is the first hostfs slot, so it must still route into
     * the fd table and still be rejected when nothing is open there (this image
     * runs with no --mount grant). A 0 here would mean the carve-out had grown
     * past the stdio reservations and was swallowing real descriptors. The exact
     * -errno is not asserted, only that the result stays an error. */
    rc3 = sys_close(3);

    report("close(0)", 8, rc0);
    report("close(1)", 8, rc1);
    report("close(2)", 8, rc2);
    if (rc3 != 0)
        WR("close(3) rejected\n");
    else
        WR("close(3) FAIL\n");
    WR("write(1) after close(1) ok\n");
    return 0;
}
