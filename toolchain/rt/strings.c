/* toolchain/rt/strings.c -- freestanding <strings.h> case-insensitive compares for
 * the Maize C runtime (maize-148).
 *
 * DOOM's doomtype.h leans on strcasecmp/strncasecmp (d_iwad, d_main, i_system,
 * m_argv, w_wad, m_misc, r_data, r_things). Both mirror the strcmp/strncmp structure
 * (string.c:86-107) but fold each byte through tolower (C locale, ctype.h) before the
 * compare, and return the difference of the tolower'd unsigned-char values. One
 * primary loop each keeps them inside the pinned qbe -t maize authoring budget.
 */
#include "strings.h"
#include "ctype.h"

int
strcasecmp(const char *s1, const char *s2)
{
    while (*s1 && tolower((unsigned char)*s1) == tolower((unsigned char)*s2)) {
        s1++;
        s2++;
    }
    return tolower((unsigned char)*s1) - tolower((unsigned char)*s2);
}

int
strncasecmp(const char *s1, const char *s2, size_t n)
{
    while (n && *s1 && tolower((unsigned char)*s1) == tolower((unsigned char)*s2)) {
        s1++;
        s2++;
        n--;
    }
    if (n == 0)
        return 0;   /* n bytes matched case-insensitively */
    return tolower((unsigned char)*s1) - tolower((unsigned char)*s2);
}
