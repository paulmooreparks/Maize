/* maize-142 AC 8255: combined self-check for the stdlib numeric conversions
 * atoi / abs / labs / strtol. Each expected result is computed inline (a literal
 * or an independent expression), never by calling the library a second time, so a
 * copy-paste error in stdlib.c cannot be masked by the same error here (the ctype.c
 * fixture precedent). Prints a single "strtol PASS" / "strtol FAIL" line.
 *
 * long is 64-bit on Maize; LONG_MAX / LONG_MIN are reproduced here as local macros
 * (the slice has no limits.h, decision 8257) purely as reference constants. */
#include "stdlib.h"
#include "errno.h"
#include "stdio.h"

#define REF_LONG_MAX 0x7fffffffffffffffL          /* 9223372036854775807 */
#define REF_LONG_MIN (-REF_LONG_MAX - 1L)         /* -9223372036854775808 */

static int ok = 1;

static void check(int cond) { if (!cond) ok = 0; }

int
main(void)
{
    char *end;

    /* --- atoi (AC 8249) ------------------------------------------------------- */
    check(atoi("42") == 42);
    check(atoi(" -42abc") == -42);   /* leading ws skipped, trailing junk stops it */
    check(atoi("0") == 0);
    check(atoi("+7") == 7);

    /* --- abs / labs (AC 8250) ------------------------------------------------- */
    check(abs(-7) == 7);
    check(abs(7) == 7);
    check(abs(0) == 0);
    /* argument and result both exceed int range, proving 64-bit long width. */
    check(labs(-3000000000L) == 3000000000L);
    check(labs(3000000000L) == 3000000000L);

    /* --- strtol base 10 (AC 8251) --------------------------------------------- */
    check(strtol("123", &end, 10) == 123 && *end == '\0');
    check(strtol("-123", &end, 10) == -123 && *end == '\0');
    check(strtol("+123", &end, 10) == 123 && *end == '\0');
    end = 0;
    check(strtol("  -123xyz", &end, 10) == -123 && *end == 'x');

    /* --- strtol base 16 and base 0 auto-detect (AC 8252) ---------------------- */
    check(strtol("0x1A", &end, 16) == 26 && *end == '\0');   /* prefix consumed */
    check(strtol("1A", &end, 16) == 26 && *end == '\0');     /* prefix optional */
    check(strtol("0x10", &end, 0) == 16 && *end == '\0');    /* base 0 -> hex */
    check(strtol("010", &end, 0) == 8 && *end == '\0');      /* base 0 -> octal */
    check(strtol("10", &end, 0) == 10 && *end == '\0');      /* base 0 -> decimal */

    /* --- strtol overflow clamp + ERANGE (AC 8253) ----------------------------- */
    errno = 0;
    check(strtol("9999999999999999999999", &end, 10) == REF_LONG_MAX
          && errno == ERANGE);
    errno = 0;
    check(strtol("-9999999999999999999999", &end, 10) == REF_LONG_MIN
          && errno == ERANGE);
    /* exact-boundary strings convert without ERANGE. */
    errno = 0;
    check(strtol("9223372036854775807", &end, 10) == REF_LONG_MAX && errno == 0);
    errno = 0;
    check(strtol("-9223372036854775808", &end, 10) == REF_LONG_MIN && errno == 0);

    /* --- strtol endptr / no-conversion (AC 8254) ------------------------------ */
    /* endptr lands ON the first unconsumed char: dereference it and compare the
       character (a symbol+offset address constant is not emittable by the maize
       backend, so we check *end rather than end == nptr + N). */
    {
        const char *s = "56rest";
        check(strtol(s, &end, 10) == 56 && *end == 'r');    /* first unconsumed */
    }
    {
        const char *s = "abc";
        check(strtol(s, &end, 10) == 0 && end == s);        /* endptr == nptr */
    }

    /* --- open_question 8279 nit (a): invalid base -> EINVAL -------------------- */
    {
        const char *s = "10";
        errno = 0;
        check(strtol(s, &end, 1) == 0 && end == s && errno == EINVAL);
        errno = 0;
        check(strtol(s, &end, 37) == 0 && end == s && errno == EINVAL);
    }

    /* --- open_question 8279 nit (b): bare "0x" / "0"-no-following-digit -------- */
    /* No hex digit follows "0x": only the leading '0' converts, so endptr lands on
       the 'x' (a wrong impl that consumed nothing would leave *end == '0'). */
    check(strtol("0x", &end, 16) == 0 && *end == 'x');
    check(strtol("0x", &end, 0) == 0 && *end == 'x');
    /* base 0 -> octal; '8' is not an octal digit, so only "0" converts (*end='8'). */
    check(strtol("08", &end, 0) == 0 && *end == '8');
    /* bare "0" converts to 0 and consumes it, leaving endptr on the terminator. */
    check(strtol("0", &end, 10) == 0 && *end == '\0');

    puts(ok ? "strtol PASS" : "strtol FAIL");
    return 0;
}
