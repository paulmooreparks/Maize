/* maize-100: atexit() registry + exit()-runs-handlers proof. crt0 routes main's
 * return through exit() (decision 7346), so returning from main runs the two
 * registered handlers. They run in LIFO order (B registered last, so it fires
 * first), which the ordered expected output pins:
 *
 *   main done
 *   atexit B
 *   atexit A
 *
 * This single fixture proves BOTH that a registered handler runs at exit AND the
 * LIFO ordering, and it exercises the codegen path under test: exit() calls each
 * handler indirectly through a runtime-indexed function-pointer array. */
#include "stdlib.h"
#include "stdio.h"

static void handler_a(void) { puts("atexit A"); }
static void handler_b(void) { puts("atexit B"); }

int
main(void)
{
    atexit(handler_a);   /* runs second (LIFO) */
    atexit(handler_b);   /* runs first  (LIFO) */
    puts("main done");
    return 0;            /* crt0 funnels this through exit() */
}
