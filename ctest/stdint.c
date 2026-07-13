/* maize-146: freestanding stdint.h / limits.h / stdbool.h / inttypes.h self-check.
 *
 * Exercises all four RT headers through the real cc-maize.sh pipeline: the
 * fixed-width typedefs (sizeof proves the LP64 widths 1/2/4/8), the limit and
 * constant macros in checked computations, the limits.h CHAR_BIT / INT_MAX /
 * LONG_MAX / CHAR_MIN values, bool/true/false from stdbool.h, and the inttypes.h
 * PRI* format macros driving the Maize printf. A wrong width, value, or macro
 * suffix flips the accumulator and corrupts the "stdint: PASS" verdict line
 * (itself printed via printf("%s")). See ctest/stdint.expected for the exact
 * bytes. */
#include "stdint.h"
#include "limits.h"
#include "stdbool.h"
#include "inttypes.h"
#include "stdio.h"

static int ok = 1;

static void check(int cond) { if (!cond) ok = 0; }

int
main(void)
{
    /* One variable of each exact-width type; sizeof references them (so no
     * unused-variable warning) and proves the LP64 widths. */
    int8_t   a8;   int16_t  a16;  int32_t  a32;  int64_t  a64;
    uint8_t  u8;   uint16_t u16;  uint32_t u32;  uint64_t u64;
    intptr_t ip;   uintptr_t up;

    check(sizeof(a8)  == 1);
    check(sizeof(a16) == 2);
    check(sizeof(a32) == 4);
    check(sizeof(a64) == 8);
    check(sizeof(u8)  == 1);
    check(sizeof(u16) == 2);
    check(sizeof(u32) == 4);
    check(sizeof(u64) == 8);
    check(sizeof(ip)  == 8);
    check(sizeof(up)  == 8);

    /* Limit + constant macros in a checked computation. */
    {
        uint64_t big = UINT64_C(0xFFFFFFFFFFFFFFFF);
        check(big == UINT64_MAX);
        check(INT32_MAX == 2147483647);
        check(INT64_MIN < INT64_MAX);
        check(UINT32_MAX == 4294967295U);
        check(INT8_MAX == 127 && INT8_MIN == -128);
        check(INT16_MAX == 32767 && UINT16_MAX == 65535);
    }

    /* limits.h values, including the signed-char assertion. */
    check(CHAR_BIT == 8);
    check(INT_MAX == 2147483647);
    check(LONG_MAX == 9223372036854775807L);
    check(CHAR_MIN == -128);
    check(SCHAR_MIN == CHAR_MIN && SCHAR_MAX == CHAR_MAX);

    /* stdbool.h */
    {
        bool flag = true;
        check(flag == true);
        check(false == 0);
    }

    /* inttypes.h PRI macros over the Maize printf: one signed 64-bit and one
     * hex 64-bit. The exact bytes are the check (byte-diffed vs .expected). */
    printf("v=%" PRId64 "\n", (int64_t)-5);
    printf("h=%" PRIx64 "\n", (uint64_t)0xABCDEF);

    printf("stdint: %s\n", ok ? "PASS" : "FAIL");
    return 0;
}
