/* maize-74 AC 7292: error-range translation through the wrapper.
 *
 * write() to an invalid fd (99, not open) makes the VM return its error value,
 * which lands in [-4095, -1]; __syscall_ret turns that into errno = -result and a
 * -1 return. The fixture asserts BOTH the -1 return and a nonzero errno, then
 * reports the verdict as a fixed string via a valid-fd write. (Until maize-75 the
 * VM returns a bare -1, so errno is 1 here; the -1/errno MECHANISM is what this
 * proves, not the specific code.) Expected stdout: "errno ok". */
#include "syscall.h"

int main(void) {
    long r = write(99, "x", 1);
    if (r == -1 && errno != 0)
        write(1, "errno ok\n", 9);
    else
        write(1, "errno BAD\n", 10);
    return 0;
}
