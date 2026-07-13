/* toolchain/rt/strings.h -- freestanding <strings.h> case-insensitive compare
 * declarations for the Maize C runtime (maize-147).
 *
 * DOOM's doomtype.h includes <strings.h> for strcasecmp/strncasecmp; strict cproc
 * needs a visible declaration at each call site. This header DECLARES exactly those
 * two; the bodies live in the sibling libc card (maize-148). Scoped to the two the
 * DOOM tree references: ffs/bcopy/bzero are deliberately NOT declared (not used).
 * size_t comes from stddef.h (maize-146), the LP64 unsigned long.
 */
#ifndef MAIZE_STRINGS_H
#define MAIZE_STRINGS_H

#include "stddef.h"

int strcasecmp(const char *s1, const char *s2);
int strncasecmp(const char *s1, const char *s2, size_t n);

#endif /* MAIZE_STRINGS_H */
