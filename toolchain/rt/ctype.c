/* toolchain/rt/ctype.c -- freestanding <ctype.h> core (maize-76, decision 7343).
 *
 * ASCII / "C" locale. Predicates are written against explicit code-point ranges
 * rather than a lookup table: the set is small and the ranges are unambiguous, so
 * this keeps the module free of any static data section.
 */
#include "ctype.h"

int
isdigit(int c)
{
    return c >= '0' && c <= '9';
}

int
isalpha(int c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

int
isalnum(int c)
{
    return isalpha(c) || isdigit(c);
}

int
isspace(int c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

int
isupper(int c)
{
    return c >= 'A' && c <= 'Z';
}

int
islower(int c)
{
    return c >= 'a' && c <= 'z';
}

int
isxdigit(int c)
{
    return isdigit(c) || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
}

int
isprint(int c)
{
    return c >= 0x20 && c <= 0x7E;
}

int
ispunct(int c)
{
    /* Printable, not a space, not alphanumeric. */
    return isprint(c) && c != ' ' && !isalnum(c);
}

int
iscntrl(int c)
{
    return (c >= 0 && c <= 0x1F) || c == 0x7F;
}

int
isblank(int c)
{
    return c == ' ' || c == '\t';
}

int
isgraph(int c)
{
    /* Printable and not a space: the visible-glyph subset of isprint. */
    return isprint(c) && c != ' ';
}

int
toupper(int c)
{
    if (c >= 'a' && c <= 'z')
        return c - ('a' - 'A');
    return c;
}

int
tolower(int c)
{
    if (c >= 'A' && c <= 'Z')
        return c + ('a' - 'A');
    return c;
}

/* isascii (maize-94): true for a 7-bit character value (0..127). */
int
isascii(int c)
{
	return (c >= 0 && c < 128);
}
