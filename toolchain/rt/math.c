/* toolchain/rt/math.c -- freestanding <math.h> slice for the Maize C runtime
 * (maize-148).
 *
 * fabs is the ONLY math.h function this card defines; math.h's other declarations
 * back DOOM's dead #if 0 trig (maize-147), which has no live caller.
 *
 * fabs clears bit 63 (the IEEE-754 sign bit) of the double's bit pattern via a union
 * type-pun, NOT via -x or a ternary negate: cproc lowers a double unary minus to the
 * QBE `neg` instruction, which the pinned qbe -t maize backend's parser has no keyword
 * for (the same constraint that made maize-144 atof negate via 0.0 - result). The mask
 * leaves NaN/inf magnitudes intact and is correct for -0.0 (yields +0.0). The union
 * pun and the 64-bit AND on unsigned long are both exercised elsewhere in the RT/ctest
 * corpus (ctest/fp.c fbits/dbits; stdlib.c free-list SIZEMASK), so the sanctioned
 * memcpy fallback (decision 8440) is not needed.
 */
#include "math.h"

double
fabs(double x)
{
    union { double d; unsigned long u; } v;
    v.d = x;
    v.u &= 0x7fffffffffffffffUL;   /* clear the sign bit; magnitude/NaN payload intact */
    return v.d;
}
