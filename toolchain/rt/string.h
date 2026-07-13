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

#endif /* MAIZE_STRING_H */
