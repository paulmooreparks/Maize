/* userland/include/regex.h -- minimal <regex.h> shim for the Maize userland port
 * (maize-94).
 *
 * sbase's util.h includes <regex.h> unconditionally and declares eregcomp/enregcomp with
 * regex_t*, so EVERY sbase utility needs the type and prototypes to compile, even though
 * only the grep/regex family (out of scope for wave 1) actually calls regcomp/regexec.
 * This shim provides the POSIX types, the REG_* flag/error constants, and the function
 * prototypes so util.h compiles; the functions are intentionally left UNDEFINED. A wave-1
 * utility that never calls them links clean (mzld only errors on REFERENCED undefined
 * symbols); a utility that does call them would fail at link, which is the honest signal
 * that regex support is a later card, not a silent stub.
 *
 * Copied into the scratch checkout by userland/build-userland.sh so cc-maize.sh's
 * per-source `-I <source dir>` resolves the angle-bracket include (kept out of the shared
 * toolchain/rt libc slice, which owns no regex today).
 */
#ifndef MAIZE_USERLAND_REGEX_H
#define MAIZE_USERLAND_REGEX_H

#include <stddef.h>   /* size_t */

typedef long regoff_t;

typedef struct {
    size_t re_nsub;
    void  *__opaque;
} regex_t;

typedef struct {
    regoff_t rm_so;
    regoff_t rm_eo;
} regmatch_t;

/* regcomp cflags. */
#define REG_EXTENDED 0x0001
#define REG_ICASE    0x0002
#define REG_NEWLINE  0x0004
#define REG_NOSUB    0x0008

/* regexec eflags. */
#define REG_NOTBOL   0x0100
#define REG_NOTEOL   0x0200

/* error codes. */
#define REG_NOMATCH  1
#define REG_BADPAT   2

int    regcomp(regex_t *, const char *, int);
int    regexec(const regex_t *, const char *, size_t, regmatch_t *, int);
size_t regerror(int, const regex_t *, char *, size_t);
void   regfree(regex_t *);

#endif /* MAIZE_USERLAND_REGEX_H */
