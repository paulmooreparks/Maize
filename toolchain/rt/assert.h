/* toolchain/rt/assert.h -- freestanding <assert.h> for the Maize C runtime
 * (maize-147).
 *
 * <assert.h> is the one standard header defined to be re-includable with DIFFERENT
 * effect per the NDEBUG state at each inclusion, so there is deliberately NO permanent
 * include guard around the assert macro: the #include's are guarded once, then assert
 * is #undef'd and redefined on every inclusion against the current NDEBUG.
 *
 * The failure branch composes the EXISTING, already-linked runtime fprintf + abort
 * (stdio.h / stdlib.h). That keeps this card header-only and adds ZERO work to the
 * sibling libc card (maize-148): no new __assert_fail symbol is introduced. __func__
 * is deliberately omitted (pinned-cproc C99-predefined-identifier safety); __FILE__ /
 * __LINE__ are system-cpp builtins and expand fine under cc-maize.sh's system cpp.
 */
#ifndef MAIZE_ASSERT_INCLUDES
#define MAIZE_ASSERT_INCLUDES
#include "stdio.h"
#include "stdlib.h"
#endif /* MAIZE_ASSERT_INCLUDES */

#undef assert
#ifdef NDEBUG
#define assert(expr) ((void)0)
#else
#define assert(expr) \
    ((expr) ? (void)0 \
            : (fprintf(stderr, "%s:%d: Assertion `%s' failed.\n", \
                       __FILE__, __LINE__, #expr), abort()))
#endif
