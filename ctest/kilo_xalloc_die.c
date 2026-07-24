/* maize-350 AC2: prove die() both reports and terminates nonzero on a forced
 * allocation failure, without a real memory-exhaustion condition.
 *
 * Building with KILO_XALLOC_TESTING swaps in the test-only allocator
 * indirection from kilo_xalloc.h: a call counter plus a configurable failure
 * point (kilo_xalloc_fail_at) forces a NULL return from the underlying
 * allocator on a chosen call, and KILO_DIE_STREAM is retargeted to stdout so
 * run_ctest's stdout-only compare can assert the exact die() message text.
 *
 * fail_at = 2 lets the first xmalloc succeed (proving the wrapper passes
 * allocations through normally) and forces the second to fail, so die() runs.
 * die() prints its message and calls exit(1); this same compiled binary is
 * also driven by run_exit_status_test to assert the process exit status is 1. */
#define KILO_XALLOC_TESTING
#include "../demos/kilo/kilo_xalloc.h"

int main(void) {
    kilo_xalloc_fail_at = 2;

    void *ok = xmalloc(32);   /* call 1: succeeds */
    (void)ok;

    void *fail = xmalloc(128);/* call 2: forced NULL -> die() -> exit(1) */
    (void)fail;

    /* Unreachable: die() exits before returning. Print a distinct marker so a
     * regression that let xmalloc return past a NULL would fail the compare. */
    printf("UNREACHABLE: die did not terminate\n");
    return 0;
}
