/* toolchain/rt/inttypes.h -- freestanding <inttypes.h> for the Maize C runtime
 * (maize-146; precautionary).
 *
 * cc-maize.sh preprocesses with `cpp -nostdinc -I toolchain/rt`, so no system
 * <inttypes.h> is visible. Included precautionarily: DOOM-style logging plausibly
 * references the PRI* 64-bit format macros. Includes stdint.h and defines the
 * PRI* format-string macros mapped onto the Maize printf (see stdio.h): it
 * supports %d %i %u %x %X plus the `l` length modifier only. Exact widths 8/16/32
 * promote to int in a varargs call, so they take NO length modifier; 64-bit / PTR
 * / MAX map to the `l` modifier. Deliberately omitted: %o / PRIo* (no octal
 * conversion in the printf) and all SCN* / scanf macros (no scanf in the runtime).
 * Header only, no runtime object.
 */
#ifndef MAIZE_INTTYPES_H
#define MAIZE_INTTYPES_H

#include "stdint.h"

/* Length-modifier prefixes for the Maize printf (%d/%i/%u/%x/%X + `l`). */
#define __PRI8   ""
#define __PRI16  ""
#define __PRI32  ""
#define __PRI64  "l"
#define __PRIPTR "l"
#define __PRIMAX "l"

/* Exact-width 8. */
#define PRId8   __PRI8  "d"
#define PRIi8   __PRI8  "i"
#define PRIu8   __PRI8  "u"
#define PRIx8   __PRI8  "x"
#define PRIX8   __PRI8  "X"

/* Exact-width 16. */
#define PRId16  __PRI16 "d"
#define PRIi16  __PRI16 "i"
#define PRIu16  __PRI16 "u"
#define PRIx16  __PRI16 "x"
#define PRIX16  __PRI16 "X"

/* Exact-width 32. */
#define PRId32  __PRI32 "d"
#define PRIi32  __PRI32 "i"
#define PRIu32  __PRI32 "u"
#define PRIx32  __PRI32 "x"
#define PRIX32  __PRI32 "X"

/* Exact-width 64. */
#define PRId64  __PRI64 "d"
#define PRIi64  __PRI64 "i"
#define PRIu64  __PRI64 "u"
#define PRIx64  __PRI64 "x"
#define PRIX64  __PRI64 "X"

/* Pointer-sized and maximum-width. */
#define PRIdPTR __PRIPTR "d"
#define PRIiPTR __PRIPTR "i"
#define PRIuPTR __PRIPTR "u"
#define PRIxPTR __PRIPTR "x"
#define PRIXPTR __PRIPTR "X"
#define PRIdMAX __PRIMAX "d"
#define PRIiMAX __PRIMAX "i"
#define PRIuMAX __PRIMAX "u"
#define PRIxMAX __PRIMAX "x"
#define PRIXMAX __PRIMAX "X"

/* LEAST PRI macros alias the exact-width ones (types identical). */
#define PRIdLEAST8   PRId8
#define PRIiLEAST8   PRIi8
#define PRIuLEAST8   PRIu8
#define PRIxLEAST8   PRIx8
#define PRIXLEAST8   PRIX8
#define PRIdLEAST16  PRId16
#define PRIiLEAST16  PRIi16
#define PRIuLEAST16  PRIu16
#define PRIxLEAST16  PRIx16
#define PRIXLEAST16  PRIX16
#define PRIdLEAST32  PRId32
#define PRIiLEAST32  PRIi32
#define PRIuLEAST32  PRIu32
#define PRIxLEAST32  PRIx32
#define PRIXLEAST32  PRIX32
#define PRIdLEAST64  PRId64
#define PRIiLEAST64  PRIi64
#define PRIuLEAST64  PRIu64
#define PRIxLEAST64  PRIx64
#define PRIXLEAST64  PRIX64

/* FAST PRI macros alias the exact-width ones (types identical). */
#define PRIdFAST8    PRId8
#define PRIiFAST8    PRIi8
#define PRIuFAST8    PRIu8
#define PRIxFAST8    PRIx8
#define PRIXFAST8    PRIX8
#define PRIdFAST16   PRId16
#define PRIiFAST16   PRIi16
#define PRIuFAST16   PRIu16
#define PRIxFAST16   PRIx16
#define PRIXFAST16   PRIX16
#define PRIdFAST32   PRId32
#define PRIiFAST32   PRIi32
#define PRIuFAST32   PRIu32
#define PRIxFAST32   PRIx32
#define PRIXFAST32   PRIX32
#define PRIdFAST64   PRId64
#define PRIiFAST64   PRIi64
#define PRIuFAST64   PRIu64
#define PRIxFAST64   PRIx64
#define PRIXFAST64   PRIX64

#endif /* MAIZE_INTTYPES_H */
