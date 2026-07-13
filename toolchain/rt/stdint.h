/* toolchain/rt/stdint.h -- freestanding <stdint.h> for the Maize C runtime
 * (maize-146).
 *
 * cc-maize.sh preprocesses with `cpp -nostdinc -I toolchain/rt`, so no system
 * <stdint.h> is visible; the DOOM/doomgeneric tree (maize-145) uses the
 * fixed-width types and the *_MAX / *_MIN / *_C macros directly, so this is the
 * first guaranteed gap. Values are the LP64 contract for the pinned qbe-maize
 * backend (int=32, long=64, pointer=64; char is signed, targ.c signedchar=1),
 * kept consistent with stddef.h (size_t = unsigned long, ptrdiff_t = long).
 * Header only, no runtime object.
 */
#ifndef MAIZE_STDINT_H
#define MAIZE_STDINT_H

/* Exact-width, LP64 (int=32, long=64, ptr=64). char is signed on this target. */
typedef signed char        int8_t;
typedef short              int16_t;
typedef int                int32_t;
typedef long               int64_t;
typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long      uint64_t;

/* Pointer-sized and maximum-width (both 64-bit here). */
typedef long               intptr_t;
typedef unsigned long      uintptr_t;
typedef long               intmax_t;
typedef unsigned long      uintmax_t;

/* least/fast: alias straight to the exact-width types (all present, no wider need). */
typedef int8_t   int_least8_t;   typedef uint8_t   uint_least8_t;
typedef int16_t  int_least16_t;  typedef uint16_t  uint_least16_t;
typedef int32_t  int_least32_t;  typedef uint32_t  uint_least32_t;
typedef int64_t  int_least64_t;  typedef uint64_t  uint_least64_t;
typedef int8_t   int_fast8_t;    typedef uint8_t   uint_fast8_t;
typedef int16_t  int_fast16_t;   typedef uint16_t  uint_fast16_t;
typedef int32_t  int_fast32_t;   typedef uint32_t  uint_fast32_t;
typedef int64_t  int_fast64_t;   typedef uint64_t  uint_fast64_t;

/* Limits. MIN of the two widest widths is spelled (-MAX - 1) to dodge the
 * "negative literal is negate-of-a-positive" size trap (same trap ctest/printf.c
 * documents); 8/16-bit positives fit in int, so plain forms are safe there. */
#define INT8_MAX    127
#define INT8_MIN    (-128)
#define UINT8_MAX   255
#define INT16_MAX   32767
#define INT16_MIN   (-32768)
#define UINT16_MAX  65535
#define INT32_MAX   2147483647
#define INT32_MIN   (-2147483647 - 1)
#define UINT32_MAX  4294967295U
#define INT64_MAX   9223372036854775807L
#define INT64_MIN   (-9223372036854775807L - 1L)
#define UINT64_MAX  18446744073709551615UL

#define INTPTR_MAX  INT64_MAX
#define INTPTR_MIN  INT64_MIN
#define UINTPTR_MAX UINT64_MAX
#define INTMAX_MAX  INT64_MAX
#define INTMAX_MIN  INT64_MIN
#define UINTMAX_MAX UINT64_MAX
#define SIZE_MAX    UINT64_MAX
#define PTRDIFF_MAX INT64_MAX
#define PTRDIFF_MIN INT64_MIN

/* least/fast limits alias the exact-width limits (types are identical). */
#define INT_LEAST8_MAX   INT8_MAX
#define INT_LEAST8_MIN   INT8_MIN
#define UINT_LEAST8_MAX  UINT8_MAX
#define INT_LEAST16_MAX  INT16_MAX
#define INT_LEAST16_MIN  INT16_MIN
#define UINT_LEAST16_MAX UINT16_MAX
#define INT_LEAST32_MAX  INT32_MAX
#define INT_LEAST32_MIN  INT32_MIN
#define UINT_LEAST32_MAX UINT32_MAX
#define INT_LEAST64_MAX  INT64_MAX
#define INT_LEAST64_MIN  INT64_MIN
#define UINT_LEAST64_MAX UINT64_MAX

#define INT_FAST8_MAX    INT8_MAX
#define INT_FAST8_MIN    INT8_MIN
#define UINT_FAST8_MAX   UINT8_MAX
#define INT_FAST16_MAX   INT16_MAX
#define INT_FAST16_MIN   INT16_MIN
#define UINT_FAST16_MAX  UINT16_MAX
#define INT_FAST32_MAX   INT32_MAX
#define INT_FAST32_MIN   INT32_MIN
#define UINT_FAST32_MAX  UINT32_MAX
#define INT_FAST64_MAX   INT64_MAX
#define INT_FAST64_MIN   INT64_MIN
#define UINT_FAST64_MAX  UINT64_MAX

/* Constant-suffix macros (glibc LP64 forms): append the suffix that gives the
 * argument the type of int_leastN_t / uint_leastN_t. */
#define INT8_C(c)    c
#define INT16_C(c)   c
#define INT32_C(c)   c
#define INT64_C(c)   c ## L
#define UINT8_C(c)   c
#define UINT16_C(c)  c
#define UINT32_C(c)  c ## U
#define UINT64_C(c)  c ## UL
#define INTMAX_C(c)  c ## L
#define UINTMAX_C(c) c ## UL

#endif /* MAIZE_STDINT_H */
