/* toolchain/rt/limits.h -- freestanding <limits.h> for the Maize C runtime
 * (maize-146).
 *
 * cc-maize.sh preprocesses with `cpp -nostdinc -I toolchain/rt`, so no system
 * <limits.h> is visible; the DOOM tree (maize-145) references CHAR_BIT, INT_MAX,
 * LONG_MAX and friends. Values are the LP64 contract (int=32, long=64) confirmed
 * against toolchain/cproc/type.c; char is signed on this target (targ.c
 * signedchar=1), so CHAR_* == SCHAR_*. The MINs use the (-MAX - 1) idiom to dodge
 * the "negative literal is negate-of-a-positive" size trap. Header only.
 */
#ifndef MAIZE_LIMITS_H
#define MAIZE_LIMITS_H

#define CHAR_BIT   8
#define SCHAR_MIN  (-128)
#define SCHAR_MAX  127
#define UCHAR_MAX  255
/* char is SIGNED on this target (targ.c signedchar=1), so CHAR_* == SCHAR_*. */
#define CHAR_MIN   SCHAR_MIN
#define CHAR_MAX   SCHAR_MAX
#define SHRT_MIN   (-32768)
#define SHRT_MAX   32767
#define USHRT_MAX  65535
#define INT_MIN    (-2147483647 - 1)
#define INT_MAX    2147483647
#define UINT_MAX   4294967295U
#define LONG_MIN   (-9223372036854775807L - 1L)
#define LONG_MAX   9223372036854775807L
#define ULONG_MAX  18446744073709551615UL
#define LLONG_MIN  (-9223372036854775807LL - 1LL)
#define LLONG_MAX  9223372036854775807LL
#define ULLONG_MAX 18446744073709551615ULL
#define MB_LEN_MAX 1

#endif /* MAIZE_LIMITS_H */
