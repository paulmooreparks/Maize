/* toolchain/rt/ctype.h -- freestanding <ctype.h> core for the Maize C runtime
 * (maize-76, decision 7343).
 *
 * ASCII / "C" locale only; each predicate takes an int (a value representable as
 * unsigned char, or EOF) and returns nonzero/zero. toupper/tolower map the cased
 * letters and pass everything else through. Locale-aware behavior is out of scope
 * (filed on maize-100). Implemented as plain functions (not macros) so they can be
 * called through a pointer and linked as a normal .mzo.
 */
#ifndef MAIZE_CTYPE_H
#define MAIZE_CTYPE_H

int isdigit(int c);
int isalpha(int c);
int isalnum(int c);
int isspace(int c);
int isupper(int c);
int islower(int c);
int isxdigit(int c);
int isprint(int c);
int ispunct(int c);
int iscntrl(int c);
int isblank(int c);
int isgraph(int c);
int toupper(int c);
int tolower(int c);

#endif /* MAIZE_CTYPE_H */
