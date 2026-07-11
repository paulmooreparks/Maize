/* maize-76 AC 7352: string.h core self-check. Exercises the mem* family, strlen,
 * the comparators, the copiers (including strncpy padding + truncation), strcat,
 * and strchr/strrchr hit-and-miss. Overlapping memmove is checked in BOTH
 * directions (dst<src forward, dst>src backward). Prints a single "str PASS" line
 * (or "str FAIL" on the first failing check), the capstone self-check pattern. */
#include "string.h"
#include "stdio.h"

static int ok = 1;

static void check(int cond) { if (!cond) ok = 0; }

int
main(void)
{
    char buf[32];
    char b2[32];

    /* memcpy */
    memcpy(buf, "hello", 6);
    check(memcmp(buf, "hello", 6) == 0);

    /* memset */
    memset(buf, 'x', 4);
    buf[4] = '\0';
    check(strcmp(buf, "xxxx") == 0);

    /* memmove forward overlap (dst < src): shift "ABCDEF" left by 2 -> "CDEF" */
    memcpy(buf, "ABCDEF", 7);
    memmove(buf, buf + 2, 5);            /* copies "CDEF\0" over the front */
    check(strcmp(buf, "CDEF") == 0);

    /* memmove backward overlap (dst > src): shift "12345" right by 1 */
    memcpy(buf, "12345", 6);
    memmove(buf + 1, buf, 5);            /* buf becomes "112345"? no: 1 + "12345" */
    check(buf[0] == '1' && buf[1] == '1' && buf[2] == '2' &&
          buf[3] == '3' && buf[4] == '4' && buf[5] == '5');

    /* memcmp ordering */
    check(memcmp("abc", "abd", 3) < 0);
    check(memcmp("abd", "abc", 3) > 0);

    /* strlen */
    check(strlen("") == 0);
    check(strlen("maize") == 5);

    /* strcmp / strncmp */
    check(strcmp("abc", "abc") == 0);
    check(strcmp("abc", "abd") < 0);
    check(strncmp("abcXX", "abcYY", 3) == 0);
    check(strncmp("abcXX", "abcYY", 4) != 0);

    /* strcpy */
    strcpy(b2, "copyme");
    check(strcmp(b2, "copyme") == 0);

    /* strncpy truncation (no NUL when src longer than n) + padding (NUL fill) */
    memset(b2, '?', sizeof b2);
    strncpy(b2, "abcdef", 3);            /* truncates: b2[0..2] = "abc", no NUL added */
    check(b2[0] == 'a' && b2[1] == 'b' && b2[2] == 'c' && b2[3] == '?');
    memset(b2, '?', sizeof b2);
    strncpy(b2, "hi", 5);                /* pads: "hi" then 3 NULs */
    check(b2[0] == 'h' && b2[1] == 'i' && b2[2] == '\0' &&
          b2[3] == '\0' && b2[4] == '\0');

    /* strcat / strncat */
    strcpy(b2, "foo");
    strcat(b2, "bar");
    check(strcmp(b2, "foobar") == 0);
    strcpy(b2, "foo");
    strncat(b2, "barbaz", 3);
    check(strcmp(b2, "foobar") == 0);

    /* strchr / strrchr hit and miss. `s` is a LOCAL array (a stack address), not a
       string literal: comparing strchr's result against s + N must stay a runtime
       stack-relative address. A string-literal source would let qbe copy-propagate
       the literal's symbol address and fold s + N into a `$sym + N` operand, which
       the pinned qbe -t maize backend cannot emit (emit.c: nonzero address offset).
       That backend gap is a fixture-authoring constraint here, not a library limit. */
    {
        char sbuf[] = "banana";
        const char *s = sbuf;
        check(strchr(s, 'b') == s);
        check(strchr(s, 'n') == s + 2);
        check(strrchr(s, 'n') == s + 4);
        check(strchr(s, 'z') == (void *)0);
        check(strrchr(s, 'z') == (void *)0);
        check(strchr(s, '\0') == s + 6);   /* strchr finds the terminator */
    }

    /* memchr hit and miss */
    check(memchr("abcdef", 'd', 6) != (void *)0);
    check(memchr("abcdef", 'z', 6) == (void *)0);

    puts(ok ? "str PASS" : "str FAIL");
    return 0;
}
