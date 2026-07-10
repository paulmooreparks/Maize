/* maize-74 AC 7291: the errno wrapper `write` on a SUCCESS path returns the byte
 * count (a raw RV result NOT in the error range, passed through __syscall_ret
 * verbatim). The fixture writes a line, then asserts the returned count equals the
 * length it asked for and reports the result as a fixed string (no number
 * formatting exists yet). Expected stdout: the line, then "count ok". */
#include "syscall.h"

int main(void) {
    static const char msg[] = "wrapper write\n";
    long n = write(1, msg, sizeof msg - 1);
    if (n == (long)(sizeof msg - 1))
        write(1, "count ok\n", 9);
    else
        write(1, "count BAD\n", 10);
    return 0;
}
