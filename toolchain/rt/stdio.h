/* toolchain/rt/stdio.h -- non-variadic <stdio.h> core for the Maize C runtime
 * (maize-76, decisions 7341 / 7359).
 *
 * Fully UNBUFFERED (decision 7341): each putchar is one sys_write; fputs is one
 * sys_write of the whole string. There is no persistent stream buffer, so exit()
 * and abort() need no flush hook.
 *
 * FILE is the minimal M1 stream (decision 7359): a struct wrapping just the fd,
 * with static stdout/stderr objects (fd 1 / fd 2) reached through extern pointers.
 * putc/fputc/fputs are fixed-arity (a stream argument, not varargs), so they
 * compile on the current backend. A future M4 file-backed stdio can widen the
 * struct without breaking this API.
 *
 * Variadic printf is NOT here: it needs the varargs backend (maize-98) and is
 * filed as maize-99. This card ships no number formatting.
 */
#ifndef MAIZE_STDIO_H
#define MAIZE_STDIO_H

#include "stddef.h"

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#define EOF (-1)

typedef struct {
    int fd;
} FILE;

extern FILE *stdout;
extern FILE *stderr;

int putchar(int c);
int putc(int c, FILE *stream);
int fputc(int c, FILE *stream);
int puts(const char *s);
int fputs(const char *s, FILE *stream);

#endif /* MAIZE_STDIO_H */
