/* maize-148 RT libc round 3 for the DOOM Phase A link: a single self-checking fixture
 * over strcasecmp/strncasecmp, fabs (sign-bit mask), sscanf (the scanf core), system,
 * usleep, and the remove/mkdir link-only stubs. Every expected value is computed inline
 * (a literal or an independent expression), never by calling the library a second time,
 * so a copy-paste error in the RT cannot be masked here (the strtol.c discipline).
 * Prints a single "libcgaps3 PASS" / "libcgaps3 FAIL" line (no %f: the Maize printf has
 * no float conversion, so floats are checked by value, never printed).
 */
#include "strings.h"    /* strcasecmp, strncasecmp */
#include "math.h"       /* fabs */
#include "unistd.h"     /* usleep */
#include "stdio.h"      /* sscanf, remove, puts */
#include "stdlib.h"     /* system */
#include "sys/stat.h"   /* mkdir */
#include "string.h"     /* strcmp */

static int ok = 1;

static void check(int cond) { if (!cond) ok = 0; }

int
main(void)
{
    /* --- strcasecmp / strncasecmp (AC 8433) ---------------------------------- */
    check(strcasecmp("DooM", "doom") == 0);   /* case-insensitive equality */
    check(strcasecmp("abc", "abd") < 0);      /* ordering: c < d */
    check(strcasecmp("abd", "abc") > 0);      /* ordering: d > c */
    check(strncasecmp("ABCx", "abcy", 3) == 0);   /* first 3 match, tail ignored */
    check(strncasecmp("ABC", "abd", 3) < 0);      /* c < d within the 3-byte window */

    /* --- fabs via a sign-bit mask (AC 8434) ---------------------------------- */
    /* The negative input is built via subtraction (0.0 - x), never a float unary
     * minus: cproc lowers a double `-x` to the QBE `neg` the pinned backend rejects
     * (the same constraint fabs itself works around; the normalize sed only rewrites
     * integer neg). This keeps the fixture on the same authoring rule as the RT. */
    {
        double pos = 3.5;
        double neg = 0.0 - pos;                /* -3.5 without a float neg instruction */
        check(fabs(neg) == 3.5);
        check(fabs(pos) == 3.5);
    }
    /* -0.0 built from its bit pattern so the input genuinely has bit 63 SET; the
     * mask must clear it. Assert the result is +0.0 both numerically and by bits. */
    {
        union { unsigned long u; double d; } in;
        union { double d; unsigned long u; } out;
        in.u = 0x8000000000000000UL;          /* -0.0: sign bit set, magnitude 0 */
        out.d = fabs(in.d);
        check(out.d == 0.0);                   /* fabs(-0.0) == +0.0 numerically */
        check(out.u == 0UL);                   /* +0.0 bit pattern (bit 63 clear) */
        check((out.u >> 63) == 0UL);           /* bit 63 explicitly clear: proves the mask */
    }

    /* --- sscanf: checked assignment counts AND values (AC 8435) --------------- */
    {
        int a = 0, b = 0, c = 0;
        int n = sscanf("12 -3 0x1f", "%d %d %x", &a, &b, &c);
        check(n == 3);
        check(a == 12);
        check(b == -3);
        check(c == 31);                        /* 0x1f == 31, 0x prefix consumed */
    }
    {
        float f = 0.0f;
        float diff;
        int n = sscanf("3.14", "%f", &f);
        check(n == 1);
        diff = f - 3.14f;
        if (diff < 0.0f)                        /* abs without a float unary minus */
            diff = 0.0f - diff;
        check(diff < 0.0001f);
    }
    {
        char buf[32];
        int n = sscanf("hello world", "%s", buf);
        check(n == 1);
        check(strcmp(buf, "hello") == 0);      /* stops at the first whitespace */
    }
    {
        char ch = 0;
        int n = sscanf("Q", "%c", &ch);
        check(n == 1);
        check(ch == 'Q');
    }
    {
        /* %*d suppresses the first value (not stored, not counted); only the second
         * assignment lands. keep starts at a sentinel so a stray store is visible. */
        int keep = -999;
        int n = sscanf("10 20", "%*d %d", &keep);
        check(n == 1);
        check(keep == 20);
    }
    {
        /* Partial match: the second %d meets non-numeric input, so the count is 1
         * (fewer than the two conversions requested). */
        int x = 0, y = -1;
        int n = sscanf("42 foo", "%d %d", &x, &y);
        check(n == 1);
        check(x == 42);
        check(y == -1);                        /* y untouched by the failed conversion */
    }

    /* --- system: no shell (AC 8436) ------------------------------------------ */
    check(system((const char *)0) == 0);       /* NULL: no command processor -> 0 */
    check(system("true") == -1);               /* any command: cannot execute -> -1 */

    /* --- usleep: no-op stub, must not hang (AC 8436) -------------------------- */
    check(usleep(1000) == 0);

    /* --- remove / mkdir: LINK + execute smoke only (AC 8437) ------------------ */
    /* The interim VM no-op returns 0; the real -errno semantics land on the spawned
     * VM+hostfs card (maize-151), where the checked filesystem acceptance criteria
     * live. Prove only that these link and execute without crashing. */
    (void)remove("maize_libcgaps3_nonexistent.tmp");
    (void)mkdir("maize_libcgaps3_dir", 0755);

    puts(ok ? "libcgaps3 PASS" : "libcgaps3 FAIL");
    return 0;
}
