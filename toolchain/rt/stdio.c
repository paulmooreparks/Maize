/* toolchain/rt/stdio.c -- non-variadic, fully unbuffered stdio core (maize-76,
 * decisions 7341 / 7359). puts moves here from the retired asm puts.mazm (decision
 * 7345).
 *
 * Every write is one sys_write on the stream's fd. puts writes the string then a
 * single '\n', matching the observable stdout of the old asm puts byte for byte
 * (string, then newline), so hello/capstone/globals/ptrdata are unchanged.
 */
#include "stdio.h"
#include "string.h"   /* strlen */
#include "syscall.h"  /* sys_write */

/* Static stream objects; the public handles are pointers to them (decision 7359). */
static FILE _stdout = { STDOUT_FILENO };
static FILE _stderr = { STDERR_FILENO };
FILE *stdout = &_stdout;
FILE *stderr = &_stderr;

int
fputc(int c, FILE *stream)
{
    unsigned char ch = (unsigned char)c;
    if (sys_write(stream->fd, &ch, 1) != 1)
        return EOF;
    return (int)ch;
}

int
putc(int c, FILE *stream)
{
    return fputc(c, stream);
}

int
putchar(int c)
{
    return fputc(c, stdout);
}

int
fputs(const char *s, FILE *stream)
{
    size_t n = strlen(s);
    if (n != 0 && sys_write(stream->fd, s, n) != (long)n)
        return EOF;
    return 0;   /* any non-negative value on success */
}

int
puts(const char *s)
{
    if (fputs(s, stdout) == EOF)
        return EOF;
    if (putchar('\n') == EOF)
        return EOF;
    return 0;   /* non-negative on success */
}
