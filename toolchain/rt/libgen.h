/* toolchain/rt/libgen.h -- freestanding <libgen.h> for the Maize C runtime
 * (maize-94).
 *
 * POSIX path splitters basename()/dirname(), added WITH their first consumer:
 * borrowed sbase's libutil/enmasse.c (the cp/mv destination-path builder) does
 * #include <libgen.h> and calls basename(argv[i]). The bodies live in string.c
 * (already in cc-maize.sh's fixed RT object set), so no new RT object is added.
 *
 * Both take a modifiable char * (POSIX allows the call to modify the buffer and to
 * return a pointer into it, so callers must not pass a string literal); they return
 * the final component (basename) or the parent directory (dirname), or "." when the
 * path has no directory part. This is the XPG/POSIX libgen basename, distinct from
 * the GNU <string.h> basename that never modifies its argument.
 */
#ifndef MAIZE_LIBGEN_H
#define MAIZE_LIBGEN_H

char *basename(char *path);
char *dirname(char *path);

#endif /* MAIZE_LIBGEN_H */
