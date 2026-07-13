/* toolchain/rt/stdio.h -- <stdio.h> core for the Maize C runtime (maize-76,
 * decisions 7341 / 7359; file-backed FILE* layer maize-120).
 *
 * stdout/stderr stay UNBUFFERED (decision 7341): each putchar is one sys_write and
 * fputs is one sys_write of the whole string, so every existing fixture's stdout is
 * byte-for-byte identical. fopen'd streams (maize-120) are FULLY BUFFERED with one
 * BUFSIZ single-direction buffer. Because a buffered write stream can hold undrained
 * bytes when main returns, stdio registers __stdio_flush_all on maize-100's atexit
 * registry at first fopen; exit() therefore flushes it, while _Exit()/abort() (which
 * bypass the registry) do not. stdout/stderr being unbuffered, the hook is never
 * load-bearing for them.
 *
 * FILE was the minimal M1 stream (decision 7359, just an fd); maize-120 widens it to
 * carry the buffer + flags the file-backed layer needs, as decision 7359 anticipated
 * ("A future M4 file-backed stdio can widen the struct without breaking this API").
 * putc/fputc/fputs stay fixed-arity so they compile on the pinned backend.
 *
 * Variadic printf (maize-99) rides on the maize-98 stdarg ABI and the existing
 * unbuffered sys_write core. printf/fprintf format into a bounded stack buffer
 * and chunked-flush via sys_write (no persistent stream buffer); snprintf
 * count-and-stores into the caller buffer. One core formatter, six public faces.
 * Conversion subset: %d %i %u %x %X %c %s %p %%, the `l` length modifier, minimal
 * field width + zero-pad, and precision (maize-144): ".N" and ".*" set the integer
 * MINIMUM digit count (zero-filled after the sign, "0" flag suppressed) and the
 * string MAXIMUM character count (truncation). Float printf (%f/%e/%g) is still
 * unsupported: the formatter has no float conversion.
 */
#ifndef MAIZE_STDIO_H
#define MAIZE_STDIO_H

#include "stddef.h"
#include <stdarg.h>

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#define EOF (-1)

/* Full-buffer size for fopen'd streams (classic single-direction stdio buffer). */
#define BUFSIZ 4096

struct _FILE {
    int            fd;
    int            flags;   /* bitset: readable / writable / eof / error / unbuffered */
    int            mode;    /* 0 = idle, 1 = last op was read, 2 = last op was write */
    unsigned char *buf;     /* NULL for the unbuffered stdout/stderr statics */
    long           bufcap;  /* buffer capacity (BUFSIZ for fopen'd streams) */
    long           bufpos;  /* cursor within buf */
    long           buflen;  /* valid bytes in buf (read mode) */
    struct _FILE  *next;    /* open-stream list link (fopen'd streams only) */
};
typedef struct _FILE FILE;

extern FILE *stdout;
extern FILE *stderr;

int putchar(int c);
int putc(int c, FILE *stream);
int fputc(int c, FILE *stream);
int puts(const char *s);
int fputs(const char *s, FILE *stream);

int printf(const char *fmt, ...);
int fprintf(FILE *stream, const char *fmt, ...);
int snprintf(char *str, size_t n, const char *fmt, ...);
int sprintf(char *str, const char *fmt, ...);
int vprintf(const char *fmt, va_list ap);
int vfprintf(FILE *stream, const char *fmt, va_list ap);
int vsnprintf(char *str, size_t n, const char *fmt, va_list ap);

/* maize-120 file-backed FILE* layer. fopen'd streams are fully buffered; the mode
 * string accepts and ignores a 'b' (Maize does no text-mode translation, so binary
 * and text streams are identical, which is what makes WAD reads byte-safe). */
FILE  *fopen(const char *path, const char *mode);
int    fclose(FILE *stream);
size_t fread (void *ptr, size_t size, size_t nmemb, FILE *stream);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream);
char  *fgets(char *s, int n, FILE *stream);
int    fseek(FILE *stream, long offset, int whence);
long   ftell(FILE *stream);
int    fflush(FILE *stream);   /* NULL flushes every open buffered stream */
int    feof(FILE *stream);
int    ferror(FILE *stream);
void   clearerr(FILE *stream);

/* Registered on the atexit registry at first fopen (maize-120): walks the open-stream
 * list and flushes each buffered write stream, so bytes land even if main returns
 * without fclose. _Exit()/abort() bypass atexit and so do not flush. */
void   __stdio_flush_all(void);

#endif /* MAIZE_STDIO_H */
