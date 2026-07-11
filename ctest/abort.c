/* maize-76 AC 7355: abort() terminates the process with status 134 (128 +
 * SIGABRT(6); Maize has no signal delivery, an honest deviation). This is an
 * EXIT-STATUS fixture (the abort sibling of exitcode.c): it produces no stdout;
 * run-ctest.sh runs it under run_exit_status_test and asserts $? == 134. The line
 * after abort() must never execute, so a wrong return would return 0, not 134. */
#include "stdlib.h"

int
main(void)
{
    abort();
    return 0;   /* unreachable: abort() does not return */
}
