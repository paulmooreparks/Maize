/* toolchain/rt/string.c -- freestanding <string.h> core (maize-76, decision 7343).
 *
 * Byte-at-a-time implementations: correctness over speed for the M1 slice. memmove
 * is overlap-correct (copies backward when the destination overlaps ahead of the
 * source). Comparisons return the unsigned-char difference, per the C standard.
 */
#include "string.h"

void *
memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--)
        *d++ = *s++;
    return dst;
}

void *
memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (d == s || n == 0)
        return dst;
    if (d < s) {
        while (n--)
            *d++ = *s++;
    } else {
        /* dst overlaps ahead of src: copy from the top down. */
        d += n;
        s += n;
        while (n--)
            *--d = *--s;
    }
    return dst;
}

void *
memset(void *dst, int c, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    unsigned char v = (unsigned char)c;
    while (n--)
        *d++ = v;
    return dst;
}

int
memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *pa = (const unsigned char *)a;
    const unsigned char *pb = (const unsigned char *)b;
    while (n--) {
        if (*pa != *pb)
            return (int)*pa - (int)*pb;
        pa++;
        pb++;
    }
    return 0;
}

void *
memchr(const void *s, int c, size_t n)
{
    const unsigned char *p = (const unsigned char *)s;
    unsigned char v = (unsigned char)c;
    while (n--) {
        if (*p == v)
            return (void *)p;
        p++;
    }
    return NULL;
}

size_t
strlen(const char *s)
{
    const char *p = s;
    while (*p)
        p++;
    return (size_t)(p - s);
}

int
strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int
strncmp(const char *a, const char *b, size_t n)
{
    while (n && *a && *a == *b) {
        a++;
        b++;
        n--;
    }
    if (n == 0)
        return 0;
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

char *
strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++) != '\0')
        ;
    return dst;
}

char *
strncpy(char *dst, const char *src, size_t n)
{
    char *d = dst;
    while (n && *src) {
        *d++ = *src++;
        n--;
    }
    /* Pad the remainder with NUL (strncpy does not guarantee termination but
       does NUL-fill any slack). */
    while (n--)
        *d++ = '\0';
    return dst;
}

char *
strcat(char *dst, const char *src)
{
    char *d = dst;
    while (*d)
        d++;
    while ((*d++ = *src++) != '\0')
        ;
    return dst;
}

char *
strncat(char *dst, const char *src, size_t n)
{
    char *d = dst;
    while (*d)
        d++;
    while (n && *src) {
        *d++ = *src++;
        n--;
    }
    *d = '\0';
    return dst;
}

char *
strchr(const char *s, int c)
{
    char ch = (char)c;
    for (;; s++) {
        if (*s == ch)
            return (char *)s;
        if (*s == '\0')
            return NULL;
    }
}

char *
strrchr(const char *s, int c)
{
    char ch = (char)c;
    const char *last = NULL;
    for (;; s++) {
        if (*s == ch)
            last = s;
        if (*s == '\0')
            return (char *)last;
    }
}

/* --- search / tokenize (maize-100) -------------------------------------------
 * Byte-at-a-time, no allocation, C-locale. Results are plain runtime pointers
 * into the caller's buffers, so the pinned qbe -t maize backend never has to
 * fold a `$sym + N` offset (the strchr authoring note above). */

char *
strstr(const char *haystack, const char *needle)
{
    /* An empty needle matches at the start of the haystack (C contract). */
    if (*needle == '\0')
        return (char *)haystack;

    for (; *haystack; haystack++) {
        const char *h = haystack;
        const char *n = needle;
        while (*h && *n && *h == *n) {
            h++;
            n++;
        }
        if (*n == '\0')
            return (char *)haystack;   /* whole needle matched */
        /* Mismatch: restart the needle one byte further along (backtrack). */
    }
    return NULL;
}

char *
strpbrk(const char *s, const char *accept)
{
    for (; *s; s++) {
        const char *a = accept;
        while (*a) {
            if (*s == *a)
                return (char *)s;
            a++;
        }
    }
    return NULL;   /* also the empty-accept case */
}

size_t
strspn(const char *s, const char *accept)
{
    const char *p = s;
    for (; *p; p++) {
        const char *a = accept;
        while (*a && *a != *p)
            a++;
        if (*a == '\0')
            break;   /* *p is not in accept: the initial run ends here */
    }
    return (size_t)(p - s);
}

size_t
strcspn(const char *s, const char *reject)
{
    const char *p = s;
    for (; *p; p++) {
        const char *r = reject;
        while (*r) {
            if (*r == *p)
                return (size_t)(p - s);   /* first rejected byte */
            r++;
        }
    }
    return (size_t)(p - s);   /* no reject byte found: full length */
}

char *
strtok_r(char *str, const char *delim, char **saveptr)
{
    char *start;
    char *p;

    if (str == NULL)
        str = *saveptr;   /* continuation: resume where we left off */

    /* Skip any leading delimiter bytes (collapses consecutive delimiters). */
    while (*str) {
        const char *d = delim;
        int is_delim = 0;
        while (*d) {
            if (*str == *d) {
                is_delim = 1;
                break;
            }
            d++;
        }
        if (!is_delim)
            break;
        str++;
    }

    if (*str == '\0') {
        *saveptr = str;   /* exhausted */
        return NULL;
    }

    start = str;

    /* Scan to the next delimiter, NUL-terminate the token, and record the
       resume point just past it. */
    for (p = start; *p; p++) {
        const char *d = delim;
        while (*d) {
            if (*p == *d) {
                *p = '\0';
                *saveptr = p + 1;
                return start;
            }
            d++;
        }
    }

    /* No trailing delimiter: the token runs to the end of the string. */
    *saveptr = p;   /* points at the terminating NUL */
    return start;
}

/* strtok is the non-reentrant wrapper: the same tokenizer over one file-scope
   saveptr, so the scanning logic lives only in strtok_r. */
static char *g_strtok_save;

char *
strtok(char *str, const char *delim)
{
    return strtok_r(str, delim, &g_strtok_save);
}
