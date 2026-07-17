/* toolchain/rt/string.h -- freestanding <string.h> core for the Maize C runtime
 * (maize-76, decision 7343).
 *
 * The committed set is pure computation (no syscalls): the mem* family, strlen,
 * the strcmp/strncmp comparators, the strcpy/strncpy/strcat/strncat copiers, and
 * strchr/strrchr/memchr scanners. The search/tokenize helpers (strstr, strpbrk,
 * strspn, strcspn, strtok_r, strtok) landed with maize-100, also pure computation.
 */
#ifndef MAIZE_STRING_H
#define MAIZE_STRING_H

#include "stddef.h"

void   *memcpy(void *dst, const void *src, size_t n);
void   *memmove(void *dst, const void *src, size_t n);
void   *memset(void *dst, int c, size_t n);
int     memcmp(const void *a, const void *b, size_t n);
void   *memchr(const void *s, int c, size_t n);

size_t  strlen(const char *s);
int     strcmp(const char *a, const char *b);
int     strncmp(const char *a, const char *b, size_t n);
char   *strcpy(char *dst, const char *src);
char   *strncpy(char *dst, const char *src, size_t n);
char   *strcat(char *dst, const char *src);
char   *strncat(char *dst, const char *src, size_t n);
char   *strchr(const char *s, int c);
char   *strrchr(const char *s, int c);

char   *strstr(const char *haystack, const char *needle);
char   *strpbrk(const char *s, const char *accept);
size_t  strspn(const char *s, const char *accept);
size_t  strcspn(const char *s, const char *reject);
char   *strtok_r(char *str, const char *delim, char **saveptr);
char   *strtok(char *str, const char *delim);

/* strdup (maize-144): return a malloc'd, independently free-able copy of s (the
 * caller owns and frees it), or NULL on allocation failure. POSIX places strdup in
 * <string.h>; string.c gains #include "stdlib.h" for malloc. */
char   *strdup(const char *s);

/* strndup (maize-94): like strdup but copies at most n bytes (stopping at a NUL),
 * always NUL-terminating. Borrowed sbase (libutil/ealloc.c enstrndup) needs it. */
char   *strndup(const char *s, size_t n);

/* strerror (maize-172): a static message for the errno codes the runtime names
 * (errno.h). Unknown codes return a fixed "Unknown error". kilo prints it via
 * perror / editorSave's I/O-error status line. The returned pointer is a static
 * string; the caller must not free or modify it. */
char   *strerror(int errnum);

/* strsignal (maize-94): a static message for a signal number, for borrowed oksh's
 * trap.c diagnostics. Returns a fixed "Signal N"-style string (body in string.c); the
 * pointer is static, the caller must not free or modify it. */
char   *strsignal(int sig);

/* strcasecmp / strncasecmp (maize-94): the case-insensitive compares live in <strings.h>
 * (bodies in strings.c), but borrowed oksh reaches for them through <string.h> (trap.c).
 * Re-declared here with identical prototypes (a duplicate typedef-free redeclaration is
 * legal C, cproc-tolerated) so either include resolves the call. */
int     strcasecmp(const char *s1, const char *s2);
int     strncasecmp(const char *s1, const char *s2, size_t n);

#endif /* MAIZE_STRING_H */
