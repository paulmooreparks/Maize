/* maize-147: RT headers round 2 self-check (strings/math/assert/unistd/sys headers
 * + SEEK_* and EISDIR).
 *
 * Proves the new header contract parses and carries the right values through the real
 * cc-maize.sh pipeline, WITHOUT calling any new function (the bodies are the sibling
 * libc card, maize-148; a call would leave an unresolved symbol at link). Each new
 * declaration is proven to PARSE via sizeof(&fn) (sizeof's operand is unevaluated, so
 * no relocation is emitted). Also asserts the SEEK_*, EISDIR, and S_IF* macro values,
 * off_t/ssize_t/mode_t widths, and the struct stat byte-ABI (sizeof 144; st_nlink@16,
 * st_mode@24, st_size@48, via runtime pointer subtraction since stddef.h omits offsetof
 * under -nostdinc). The assert macro's true branch is exercised and must not abort. A
 * wrong value flips the accumulator and corrupts the "rthdrs2: PASS" verdict line. */
#include "strings.h"
#include "math.h"
#include "unistd.h"
#include "sys/types.h"
#include "sys/stat.h"
#include "errno.h"
#include "stdio.h"
#include "stdlib.h"
#include "assert.h"          /* non-NDEBUG path */

static int ok = 1;

static void check(int c) { if (!c) ok = 0; }

int
main(void)
{
    struct stat s;

    /* macros */
    check(SEEK_SET == 0 && SEEK_CUR == 1 && SEEK_END == 2);
    check(EISDIR == 21);
    check(S_ISDIR(S_IFDIR) && !S_ISDIR(S_IFREG) && S_ISREG(S_IFREG));
    check(S_IFMT == 0170000 && S_IFREG == 0100000 && S_IFDIR == 0040000);

    /* type widths */
    check(sizeof(off_t) == 8 && sizeof(ssize_t) == 8 && sizeof(mode_t) == 4);

    /* struct stat byte-ABI */
    check(sizeof(struct stat) == 144);
    check((char*)&s.st_nlink - (char*)&s == 16);
    check((char*)&s.st_mode  - (char*)&s == 24);
    check((char*)&s.st_size  - (char*)&s == 48);

    /* declarations parse, no link dependency (bodies are maize-148) */
    check(sizeof(&strcasecmp) == 8 && sizeof(&strncasecmp) == 8);
    check(sizeof(&fabs) == 8 && sizeof(&sscanf) == 8 && sizeof(&remove) == 8);
    check(sizeof(&system) == 8 && sizeof(&mkdir) == 8 && sizeof(&usleep) == 8);

    /* assert macro: true branch never aborts */
    assert(1 == 1);

    printf("rthdrs2: %s\n", ok ? "PASS" : "FAIL");
    return 0;
}
