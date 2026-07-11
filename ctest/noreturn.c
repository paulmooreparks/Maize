/* maize-102: a _Noreturn C function must compile through qbe -t maize (whose
 * parser now knows the `hlt` block terminator) and terminate correctly.
 * EXIT-STATUS fixture: no stdout; run-ctest asserts $? == 57. The line after
 * die() must never execute. */
#include "stdlib.h"

static _Noreturn void die(int code)
{
	exit(code);            /* die's end block is `hlt` (a _Noreturn defn) */
}

int
main(void)
{
	die(57);
	return 0;              /* unreachable */
}
