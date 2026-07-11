/* maize-98: varargs / stdarg ABI self-check. Exercises the ABI surfaces the
 * backend gained under Option C (six GP arg registers R0..R5, decision 7598): the
 * register-resident path (a variadic call with <=6 total args), va_arg over mixed
 * scalar classes (int / long / char* / a promoted char and short), and a >=7-arg
 * call that crosses the register->overflow boundary so some variadic args resolve
 * from the register save area and some from the overflow_arg_area. va_copy is
 * exercised too (a copied va_list walked independently must reproduce the same
 * sequence). A plain non-variadic 8-arg call exercises the named-stack-parameter
 * path and confirms the rega assert is gone for 7..N-arg calls. Prints a single
 * "varargs PASS" line via puts (no printf; variadic printf is maize-99), or
 * "varargs FAIL" on the first failing check, the capstone self-check pattern. */
#include <stdarg.h>
#include "stdio.h"
#include "string.h"

static int ok = 1;

static void check(int cond) { if (!cond) ok = 0; }

/* (1) Register-resident integer sum: n names the count, the rest are variadic
 * ints read back with va_arg. */
static int
sum(int n, ...)
{
    va_list ap;
    int total, k;

    va_start(ap, n);
    total = 0;
    for (k = 0; k < n; k++)
        total += va_arg(ap, int);
    va_end(ap);
    return total;
}

/* (2) Mixed width / string / promoted sub-int. Default argument promotions send
 * char and short through as int, so both are read back with va_arg(ap, int); a
 * wrong sign or width would corrupt the checked value. */
static void
mixed(const char *label, ...)
{
    va_list ap;
    int i;
    long l;
    const char *s;
    int promoted_c, promoted_s;

    check(strcmp(label, "mix") == 0);
    va_start(ap, label);
    i = va_arg(ap, int);
    l = va_arg(ap, long);
    s = va_arg(ap, const char *);
    promoted_c = va_arg(ap, int);   /* a 'char' argument, promoted to int */
    promoted_s = va_arg(ap, int);   /* a 'short' argument, promoted to int */
    va_end(ap);

    check(i == -42);
    check(l == 5000000000L);        /* > 2^32: proves the full 64-bit read */
    check(strcmp(s, "hello") == 0);
    check(promoted_c == 'Z');
    check(promoted_s == -7);        /* sign preserved through promotion */
}

/* (3) >=7 total args plus va_copy. suml has one named arg (n in R0); under Option
 * C the first five variadic longs fill R1..R5 and the remaining seven overflow to
 * stack slots, so va_arg must read across the register save area (gp_offset < 48)
 * and the overflow area. ap2 is a va_copy of ap walked separately over the
 * identical sequence. */
static long
suml(int n, ...)
{
    va_list ap, ap2;
    long a, b;
    int k;

    va_start(ap, n);
    va_copy(ap2, ap);
    a = 0;
    for (k = 0; k < n; k++)
        a += va_arg(ap, long);
    b = 0;
    for (k = 0; k < n; k++)
        b += va_arg(ap2, long);
    va_end(ap2);
    va_end(ap);
    check(a == b);                  /* va_copy reproduced the same walk */
    return a;
}

/* (4) Plain non-variadic 8-arg call: args a..f pass in R0..R5, g and h overflow to
 * the stack. This exercises the caller-side overflow push (selcall) and the callee
 * named-stack-parameter read (selpar reads [BP+16] and [BP+24]), and confirms the
 * old rega assert (arguments could not land in callee-saved R6..R9) is gone now
 * that R0..R5 are the caller-saved argument registers (decision 7598). */
static long
sum8(long a, long b, long c, long d, long e, long f, long g, long h)
{
    return a + b + c + d + e + f + g + h;
}

int
main(void)
{
    char c = 'Z';
    short sh = -7;

    /* (1) register path. */
    check(sum(3, 10, 20, 30) == 60);
    check(sum(5, 1, 2, 3, 4, 5) == 15);
    check(sum(0) == 0);

    /* (2) mixed classes + promotion. */
    mixed("mix", -42, 5000000000L, "hello", c, sh);

    /* (3) register->overflow boundary + va_copy. 1+2+...+12 == 78. */
    check(suml(12, 1L, 2L, 3L, 4L, 5L, 6L, 7L, 8L, 9L, 10L, 11L, 12L) == 78);

    /* (4) non-variadic overflow: a..f in R0..R5, g/h on the stack. */
    check(sum8(1L, 2L, 3L, 4L, 5L, 6L, 7L, 8L) == 36);

    puts(ok ? "varargs PASS" : "varargs FAIL");
    return 0;
}
