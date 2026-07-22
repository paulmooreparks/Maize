/* maize-297: cproc/qbe miscompiled MIN(LLONG_MAX, SIZE_MAX) to -1.
 *
 * Root cause: typecommonreal() (toolchain/cproc/type.c) computed the usual-
 * arithmetic-conversions common type for the equal-width, lower-rank-unsigned
 * pair (unsigned long, long long) as the SIGNED long long, when C11 6.3.1.8
 * requires the UNSIGNED counterpart (the sibling TYPEINT/TYPELONG arms already
 * did this correctly). This silently miscompiled `MIN(LLONG_MAX, SIZE_MAX)`
 * (`(a) < (b) ? (a) : (b)`) to -1 instead of LLONG_MAX: with a signed common
 * type, `0x7FFF...F < 0xFFFF...F` reinterprets the right side as -1 and
 * evaluates FALSE, selecting SIZE_MAX. Carried as a build-time source patch
 * over the pinned cproc submodule (toolchain/cproc-patches/); see
 * toolchain/VENDORING.md.
 *
 * Covers, end to end through cproc -> qbe -t maize -> mazm -c -> mzld -> maize:
 *   - the constant-folded form of the reported repro (literal LLONG_MAX/
 *     SIZE_MAX macros, cproc's eval.c fold path)
 *   - a runtime form (operands in `volatile` variables, forcing qbe.c codegen
 *     rather than constant folding)
 *   - the full mixed long-long-vs-unsigned-long relational matrix at the
 *     64-bit boundary (< > <= >=, both operand orders)
 *   - == / != controls, which were already correct and must stay so (equality
 *     is bitwise regardless of signedness)
 *   - the over-fix guard: long long vs unsigned INT is a DIFFERENT, unaffected
 *     type pair (narrower unsigned already converts correctly to the wider
 *     signed type), and must stay a SIGNED comparison so `(long long)-1 <
 *     (unsigned int)0u` reads TRUE, not FALSE (which is what an over-broad fix
 *     would produce by reinterpreting -1 as an unsigned 64-bit value).
 *
 * Output is a fixed sequence of check lines: the two repro values, then one
 * "PASS"/"FAIL" line per relational/equality check, ending in a single
 * "minmax_signedness: PASS" (see minmax_signedness.expected). Only printf/puts
 * are used.
 */
#include <stddef.h>
#include <limits.h>
#include <stdint.h>

int printf(const char *, ...);
int puts(const char *);

#define MIN(a, b) ((a) < (b) ? (a) : (b))

static int fails = 0;

static void check(const char *name, int got, int want) {
    if (got == want) {
        printf("%s: PASS\n", name);
    } else {
        printf("%s: FAIL (got %d, want %d)\n", name, got, want);
        fails = 1;
    }
}

int main(void) {
    /* AC 9757/9758: the reported minimal repro, constant-folded form. */
    long long const_min = MIN((long long)LLONG_MAX, (size_t)SIZE_MAX);
    printf("const_min: %lld\n", const_min);
    check("const_min correct", const_min == LLONG_MAX, 1);

    /* AC 9759: the same comparison, runtime form (volatile operands defeat
     * constant folding, forcing the qbe.c codegen path). */
    volatile long long rt_a = LLONG_MAX;
    volatile size_t rt_b = SIZE_MAX;
    long long runtime_min = (rt_a < rt_b) ? (long long)rt_a : (long long)rt_b;
    printf("runtime_min: %lld\n", runtime_min);
    check("runtime_min correct", runtime_min == LLONG_MAX, 1);

    /* AC 9760: mixed long-long-vs-unsigned-long relational matrix at the
     * 64-bit boundary, both operand orders. Equal width + one operand
     * unsigned means the comparison is UNSIGNED, so LLONG_MAX (bit pattern
     * 0x7FFF...F) compares less than ULONG_MAX/SIZE_MAX (0xFFFF...F). */
    volatile long long ll_max = LLONG_MAX;
    volatile unsigned long ul_max = SIZE_MAX;
    volatile unsigned long ul_zero = 0UL;
    volatile long long ll_zero = 0LL;

    check("ll_max < ul_max",  ll_max < ul_max,  1);
    check("ul_max < ll_max",  ul_max < ll_max,  0);
    check("ll_max > ul_max",  ll_max > ul_max,  0);
    check("ul_max > ll_max",  ul_max > ll_max,  1);
    check("ll_max <= ul_max", ll_max <= ul_max, 1);
    check("ul_max <= ll_max", ul_max <= ll_max, 0);
    check("ll_max >= ul_max", ll_max >= ul_max, 0);
    check("ul_max >= ll_max", ul_max >= ll_max, 1);
    check("ul_zero < ll_max", ul_zero < ll_max, 1);
    check("ll_max < ul_zero", ll_max < ul_zero, 0);
    check("ul_zero < ul_max", ul_zero < ul_max, 1);
    check("ul_max < ul_zero", ul_max < ul_zero, 0);

    /* AC 9761: == and != controls, already correct, must remain so. LLONG_MAX
     * and SIZE_MAX are NOT the same bit pattern, so exercise both a genuinely
     * equal pair and the genuinely unequal ll_max/ul_max pair. */
    volatile long long ll_five = 5LL;
    volatile unsigned long ul_five = 5UL;
    check("ll_five == ul_five (same value)",  ll_five == ul_five, 1);
    check("ll_five != ul_five (same value)",  ll_five != ul_five, 0);
    check("ll_zero == ul_zero",               ll_zero == ul_zero, 1);
    check("ll_max == ul_max (different bits)", ll_max == ul_max, 0);
    check("ll_max != ul_max (different bits)", ll_max != ul_max, 1);

    /* AC 9762: over-fix guard (review advisory: sign-distinguishing). long
     * long vs unsigned int is a DIFFERENT, unaffected type pair (the guards
     * at type.c:240/243 already route it to the signed path); must stay
     * SIGNED so -1 < 0u is TRUE, not reinterpreted as a huge unsigned value. */
    volatile long long neg_one = -1LL;
    volatile unsigned int zero_u = 0u;
    check("(long long)-1 < (unsigned int)0u (signed, TRUE)", neg_one < zero_u, 1);
    check("(unsigned int)0u < (long long)-1 (signed, FALSE)", zero_u < neg_one, 0);

    if (!fails) {
        puts("minmax_signedness: PASS");
    }
    return fails;
}
