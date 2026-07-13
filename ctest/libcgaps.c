/* maize-144: self-check for the RT libc gaps the DOOM boot needs -- printf/sprintf
 * PRECISION (the load-bearing fix) plus strdup / getenv / qsort / atof.
 *
 * Discipline mirrors strtol.c / str.c: an `ok` accumulator over silent checks, each
 * expected value computed INLINE (a literal or an independent expression), never by
 * calling the library a second time, so a bug in the RT cannot be masked by the same
 * bug here. The verdict is emitted with puts (NOT printf), so the precision fix under
 * test never gets to judge its own verdict line. Prints "libcgaps PASS" / "... FAIL".
 *
 * printf precision proven through snprintf/sprintf + strcmp (silent), matching the
 * DOOM shape sprintf(name,"STCFN%.3d",j) that produced the wrong lump name before. */
#include "stdio.h"
#include "stdlib.h"
#include "string.h"

static int ok = 1;

static void check(int cond) { if (!cond) ok = 0; }

/* int-compare callback for qsort (ascending). Reads through void* per the ABI. */
static int
int_cmp(const void *a, const void *b)
{
    int x = *(const int *)a;
    int y = *(const int *)b;
    return (x > y) - (x < y);
}

/* |a - b| < eps, computed without <math.h> (the slice has no fabs). The absolute
 * value uses double subtraction (0.0 - d), not a float unary minus: cproc lowers a
 * float unary minus to the QBE `neg` op, which the pinned qbe -t maize cannot parse
 * (maize-137 covers double subtraction, so 0.0 - d is safe). */
static int
close_enough(double a, double b)
{
    double d = a - b;
    if (d < 0.0)
        d = 0.0 - d;
    return d < 1e-9;
}

int
main(void)
{
    char buf[64];
    int r;

    /* --- integer precision: minimum digit count, zero-filled after the sign ---- */
    r = snprintf(buf, sizeof buf, "%.3d", 7);
    check(strcmp(buf, "007") == 0);
    check(r == 3);                       /* return == would-be length */

    /* the exact DOOM console-font lump shape via sprintf. */
    sprintf(buf, "STCFN%.3d", 65);
    check(strcmp(buf, "STCFN065") == 0);

    snprintf(buf, sizeof buf, "%.5d", 42);
    check(strcmp(buf, "00042") == 0);

    /* negative value: the precision zeros land AFTER the '-' sign. */
    snprintf(buf, sizeof buf, "%.3d", -7);
    check(strcmp(buf, "-007") == 0);

    /* combined width + precision: space-padded to 8, the '0' flag ignored. */
    snprintf(buf, sizeof buf, "%8.3d", 7);
    check(strcmp(buf, "     007") == 0);

    /* %.0d of 0 emits NO digits (empty). */
    snprintf(buf, sizeof buf, "%.0d", 0);
    check(strcmp(buf, "") == 0);

    /* regression: the pre-existing %05d -> "-0007" path must be untouched. */
    snprintf(buf, sizeof buf, "%05d", -7);
    check(strcmp(buf, "-0007") == 0);

    /* --- string precision: maximum characters (truncation) -------------------- */
    snprintf(buf, sizeof buf, "%.3s", "hello");
    check(strcmp(buf, "hel") == 0);

    /* --- strdup: independent, mutable, free-able copy ------------------------- */
    {
        const char *src = "maize";
        char *dup = strdup(src);
        check(dup != NULL);
        check(strcmp(dup, "maize") == 0);   /* round-trips */
        check(dup != src);                  /* distinct buffer */
        dup[0] = 'M';                        /* mutate the copy... */
        check(strcmp(src, "maize") == 0);   /* ...source unchanged */
        check(dup[0] == 'M');
        free(dup);                           /* free-able */
    }

    /* --- qsort: sort a small int[] into ascending order ----------------------- */
    {
        int a[7];
        a[0] = 5; a[1] = 3; a[2] = 8; a[3] = 1; a[4] = 9; a[5] = 2; a[6] = 7;
        qsort(a, 7, sizeof a[0], int_cmp);
        check(a[0] == 1 && a[1] == 2 && a[2] == 3 && a[3] == 5
              && a[4] == 7 && a[5] == 8 && a[6] == 9);
    }

    /* --- atof: simple decimals within a small epsilon ------------------------- */
    /* The -2 reference is built as (double)(-2): an int literal (constant-folded)
     * through the maize-137 signed int->double path, so no float `neg` is emitted
     * (a negative float LITERAL lowers to the QBE `neg` the pinned qbe cannot parse). */
    check(close_enough(atof("3.14"), 3.14));
    check(close_enough(atof("-2"), (double)(-2)));
    check(close_enough(atof("0.5"), 0.5));

    /* --- getenv: empty environment, NULL without crashing --------------------- */
    check(getenv("PATH") == NULL);

    puts(ok ? "libcgaps PASS" : "libcgaps FAIL");
    return 0;
}
