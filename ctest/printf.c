/* maize-99: variadic printf / fprintf / snprintf self-check.
 *
 * Two complementary layers (spec, decisions 7758-7761):
 *
 *   1. Direct-emit correctness. A fixed series of printf / fprintf(stdout, ...)
 *      calls covering every conversion, whose exact stdout bytes ARE the check,
 *      matched byte-for-byte against ctest/printf.expected. Because the final
 *      verdict line is itself printed via printf("%s"), a broken printf corrupts
 *      the diffed verdict too.
 *
 *   2. snprintf return / truncation self-check via an `ok` accumulator over
 *      snprintf-into-buffer comparisons (return value AND content), so the
 *      machine-checkable cases do not depend on printf to judge printf.
 *
 * No <limits.h> exists in the freestanding rt include set and this card must not
 * add one, so INT_MIN / LONG_MIN are spelled here as (-MAX - 1) to dodge the
 * "negative literal is negate-of-a-positive" size trap. */
#include "stdio.h"
#include "string.h"   /* memset, strcmp */

#define INT_MIN_  (-2147483647 - 1)             /* -2147483648 */
#define LONG_MIN_ (-9223372036854775807L - 1L)  /* -9223372036854775808 */

static int ok = 1;

static void
check(int cond)
{
    if (!cond)
        ok = 0;
}

int
main(void)
{
    char buf[64];
    char big[301];
    int r;

    /* ---- layer 1: direct-emit correctness (byte-checked vs printf.expected) -- */

    printf("d=%d i=%i u=%u\n", -5, 42, 100u);
    printf("x=%x X=%X\n", 255u, 255u);
    printf("c=%c s=%s\n", 'A', "hi");
    printf("nullstr=%s\n", (char *)0);
    printf("p=%p null=%p\n", (void *)0x1234, (void *)0);
    printf("pct=%%\n");
    printf("ld=%ld lu=%lu lx=%lx\n", 5000000000L, 5000000000UL, 5000000000UL);
    printf("[%08x][%5d][%05d]\n", 255u, 42, -7);
    printf("min=%d lmin=%ld\n", INT_MIN_, LONG_MIN_);
    printf("[%5s][%3c][%2d]\n", "ab", 'x', 12345);
    fprintf(stdout, "fp=%d\n", 7);
    printf("unk=%z end\n");

    /* Chunked-flush proof: a single printf line longer than PRINTF_BUFSZ (256)
     * must appear COMPLETE in stdout. 300 'A' + newline == 301 bytes > 256. */
    memset(big, 'A', 300);
    big[300] = '\0';
    printf("%s\n", big);

    /* ---- layer 2: snprintf return / truncation self-check (silent) ---------- */

    /* fitting call: exact length + full content, NUL-terminated. */
    r = snprintf(buf, sizeof buf, "%d-%s", 42, "hi");
    check(r == 5);
    check(strcmp(buf, "42-hi") == 0);

    /* truncation: returns the would-be length, stores n-1 chars + NUL. */
    r = snprintf(buf, 4, "%d", 12345);
    check(r == 5);
    check(strcmp(buf, "123") == 0);   /* NUL landed at index 3 */

    /* n == 0 with a NULL destination: counts, writes nothing, no deref. */
    r = snprintf((char *)0, 0, "%d", 12345);
    check(r == 5);

    /* zero-pad-after-sign, width-zero-pad, and 64-bit long edges through the
     * same core the direct-emit lines exercised. */
    r = snprintf(buf, sizeof buf, "%05d", -7);
    check(r == 5);
    check(strcmp(buf, "-0007") == 0);

    r = snprintf(buf, sizeof buf, "%08x", 255u);
    check(r == 8);
    check(strcmp(buf, "000000ff") == 0);

    r = snprintf(buf, sizeof buf, "%ld", LONG_MIN_);
    check(r == 20);
    check(strcmp(buf, "-9223372036854775808") == 0);

    r = snprintf(buf, sizeof buf, "%lx", 5000000000UL);
    check(r == 9);
    check(strcmp(buf, "12a05f200") == 0);

    /* Verdict via printf("%s") so a broken printf also corrupts this line. */
    printf("selfcheck %s\n", ok ? "PASS" : "FAIL");
    return 0;
}
