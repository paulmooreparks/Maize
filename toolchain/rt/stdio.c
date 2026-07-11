/* toolchain/rt/stdio.c -- fully unbuffered stdio core (maize-76, decisions 7341 /
 * 7359) plus variadic printf (maize-99, decisions 7758-7761). puts moved here from
 * the retired asm puts.mazm (decision 7345).
 *
 * Every non-printf write is one sys_write on the stream's fd. puts writes the
 * string then a single '\n', matching the observable stdout of the old asm puts
 * byte for byte, so hello/capstone/globals/ptrdata are unchanged.
 *
 * printf family (maize-99): ONE core formatter (vformat) parameterised by an
 * output sink (struct fmtout). fd == -1 is snprintf's count-and-store-to-n mode;
 * fd >= 0 is printf/fprintf's format-into-a-256-byte-stack-buffer-and-chunked-
 * flush mode (decision 7761: a line longer than the buffer emits in FULL across
 * multiple sys_writes, never truncated). out_ch + fmt_finish are the whole output
 * contract, so the conversion switch is written exactly once against out_ch.
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

/* --- variadic printf family (maize-99) ------------------------------------- */

/* PRINTF_BUFSZ (decision 7760): with chunked flush the size governs only
 * sys_write granularity, never correctness, so it is a pure constant. 256 keeps
 * the common single-write case cheap while bounding stack footprint. */
#define PRINTF_BUFSZ 256

/* The %s-of-NULL sentinel (decision 7761). Hoisted to file scope on purpose: an
 * inline "(null)" literal assigned inside the vformat parse loop makes qbe-maize
 * (the pinned backend this card must not touch) die emitting a label-offset
 * memory address. A file-scope const array is loaded plainly and sidesteps it. */
static const char fmt_nullstr[] = "(null)";

/* The output sink (decision 7759). buf/cap/pos are the write cursor; total is the
 * running character count (the eventual return value); fd selects the mode. */
struct fmtout {
    char   *buf;    /* user buffer (snprintf) or a stack buffer (printf/fprintf) */
    size_t  cap;    /* n (snprintf) or PRINTF_BUFSZ (printf/fprintf) */
    size_t  pos;    /* live bytes in buf */
    size_t  total;  /* total chars produced == return value */
    int     fd;     /* target fd in flush-mode; -1 == pure-buffer (snprintf) mode */
};

/* Append one char. In flush-mode the full buffer is written out and reset when it
 * fills (chunked flush); in buffer-mode the char is stored only while a NUL still
 * fits, but total keeps counting so the return value is the would-be length. */
static void
out_ch(struct fmtout *o, char c)
{
    o->total++;
    if (o->fd >= 0) {                       /* flush-mode: printf/fprintf */
        o->buf[o->pos++] = c;
        if (o->pos == o->cap) {
            sys_write(o->fd, o->buf, o->pos);
            o->pos = 0;
        }
    } else {                                /* buffer-mode: snprintf */
        if (o->pos + 1 < o->cap)            /* leave room for the NUL */
            o->buf[o->pos++] = c;
    }
}

/* Flush any remainder (flush-mode) or NUL-terminate (buffer-mode) and return the
 * total. In buffer-mode pos <= cap-1 always holds, so buf[pos] is in bounds; when
 * cap == 0 nothing is stored and str may be NULL. */
static int
fmt_finish(struct fmtout *o)
{
    if (o->fd >= 0) {
        if (o->pos > 0)
            sys_write(o->fd, o->buf, o->pos);
    } else {
        if (o->cap > 0)
            o->buf[o->pos] = '\0';
    }
    return (int)o->total;
}

/* Emit `n` copies of a single char, and emit `n` bytes from a buffer. These are
 * split out of emit_field deliberately: qbe-maize (the pinned backend this card
 * must not modify) miscompiles a function that holds TWO sequential loops where
 * the second indexes a pointer (it spills a loop value and then asserts on a
 * copy-to-stack-slot in emitcopy). One loop per function sidesteps that gap while
 * keeping the "write the emit logic once" intent. */
static void
out_rep(struct fmtout *o, char c, size_t n)
{
    size_t k;
    for (k = 0; k < n; k++)
        out_ch(o, c);
}

static void
out_bytes(struct fmtout *o, const char *body, size_t blen)
{
    size_t k;
    for (k = 0; k < blen; k++)
        out_ch(o, body[k]);
}

/* Emit a pre-built body of blen bytes, with an optional sign char (0 == none),
 * left-padded to `width`. When zero is set the pad is '0' inserted AFTER the sign
 * (%05d of -7 -> "-0007"); otherwise the pad is spaces before the sign. This is
 * the single field-emit helper shared by every conversion. */
static void
emit_field(struct fmtout *o, char sign, const char *body, size_t blen,
           int width, int zero)
{
    size_t signlen = sign ? 1u : 0u;
    size_t total   = blen + signlen;
    size_t pad     = (width > 0 && (size_t)width > total)
                     ? (size_t)width - total : 0u;

    if (zero) {
        if (sign)
            out_ch(o, sign);
        out_rep(o, '0', pad);
        out_bytes(o, body, blen);
    } else {
        out_rep(o, ' ', pad);
        if (sign)
            out_ch(o, sign);
        out_bytes(o, body, blen);
    }
}

/* Render mag in the given base into out (which must hold >= 20 bytes; 20 decimal
 * digits is the widest 64-bit value) most-significant-digit first, returning the
 * digit count. Zero renders as a single '0'. */
static size_t
u_to_digits(unsigned long mag, unsigned base, int upper, char *out)
{
    static const char lower[] = "0123456789abcdef";
    static const char upperd[] = "0123456789ABCDEF";
    const char *dig = upper ? upperd : lower;
    char tmp[20];
    int i = 0;
    size_t n, j;

    if (mag == 0)
        tmp[i++] = '0';
    while (mag) {
        tmp[i++] = dig[mag % base];
        mag /= base;
    }
    n = (size_t)i;
    for (j = 0; j < n; j++)
        out[j] = tmp[i - 1 - (int)j];
    return n;
}

/* Unsigned numeric conversion (%u/%x/%X and the magnitude of %d/%i). */
static void
emit_uint(struct fmtout *o, unsigned long mag, unsigned base, int upper,
          char sign, int width, int zero)
{
    char body[20];
    size_t n = u_to_digits(mag, base, upper, body);
    emit_field(o, sign, body, n, width, zero);
}

/* Signed decimal via the unsigned-negate idiom so INT_MIN / LONG_MIN do not
 * overflow (a naive -v is UB at the extreme). v is already widened to long. */
static void
emit_signed(struct fmtout *o, long v, int width, int zero)
{
    unsigned long mag = (unsigned long)v;
    char sign = 0;

    if (v < 0) {
        sign = '-';
        mag = -mag;             /* modular negate: correct for LONG_MIN too */
    }
    emit_uint(o, mag, 10, 0, sign, width, zero);
}

/* The %c / %s / %p / %% cases live in their own helpers rather than inline in the
 * switch. This is not decorative: with every case inlined, vformat's live-temp
 * count (the %p path alone needs an 18-byte stack array) pushes qbe-maize past its
 * register budget, and the pinned backend this card must not touch cannot emit the
 * resulting spill-slot memory operands (it dies in memaddrreg on an RSlot address).
 * Extracting the bodies keeps vformat's frame small enough to stay spill-free while
 * leaving emit_field the single shared field emitter. */
static void
conv_c(struct fmtout *o, int v, int width)
{
    char ch = (char)v;
    emit_field(o, 0, &ch, 1, width, 0);                 /* zero-pad n/a for %c */
}

static void
conv_s(struct fmtout *o, const char *s, int width)
{
    if (s == NULL)
        s = fmt_nullstr;
    emit_field(o, 0, s, strlen(s), width, 0);           /* zero-pad n/a for %s */
}

static void
conv_p(struct fmtout *o, void *ptr, int width)
{
    char pbuf[2 + 16];                                  /* "0x" + 16 hex digits */
    size_t n;
    pbuf[0] = '0';
    pbuf[1] = 'x';
    n = u_to_digits((unsigned long)ptr, 16, 0, pbuf + 2);
    emit_field(o, 0, pbuf, 2 + n, width, 0);
}

static void
conv_pct(struct fmtout *o, int width)
{
    char pct = '%';
    emit_field(o, 0, &pct, 1, width, 0);
}

/* The one conversion loop. Walks fmt: literal bytes go straight to out_ch; on '%'
 * it parses [ '0' ] [ 1*DIGIT ] [ 'l' ] conv and emits via out_ch. */
static void
vformat(struct fmtout *o, const char *fmt, va_list ap)
{
    const char *p = fmt;

    while (*p) {
        char c = *p++;
        int zero, width, lng;
        char conv;

        if (c != '%') {
            out_ch(o, c);
            continue;
        }
        if (*p == '\0') {               /* trailing lone '%' -> literal '%' */
            out_ch(o, '%');
            break;
        }

        zero = 0;
        width = 0;
        lng = 0;
        if (*p == '0') { zero = 1; p++; }               /* zero-pad flag */
        while (*p >= '0' && *p <= '9')                  /* minimum field width */
            width = width * 10 + (*p++ - '0');
        if (*p == 'l') { lng = 1; p++; }                /* long length modifier */

        conv = *p;
        if (conv != '\0')
            p++;

        switch (conv) {
        case 'd':
        case 'i':
            emit_signed(o, lng ? va_arg(ap, long) : (long)va_arg(ap, int),
                        width, zero);
            break;
        case 'u':
            emit_uint(o, lng ? va_arg(ap, unsigned long)
                             : (unsigned long)va_arg(ap, unsigned int),
                      10, 0, 0, width, zero);
            break;
        case 'x':
            emit_uint(o, lng ? va_arg(ap, unsigned long)
                             : (unsigned long)va_arg(ap, unsigned int),
                      16, 0, 0, width, zero);
            break;
        case 'X':
            emit_uint(o, lng ? va_arg(ap, unsigned long)
                             : (unsigned long)va_arg(ap, unsigned int),
                      16, 1, 0, width, zero);
            break;
        case 'c':
            conv_c(o, va_arg(ap, int), width);
            break;
        case 's':
            conv_s(o, va_arg(ap, const char *), width);
            break;
        case 'p':
            conv_p(o, va_arg(ap, void *), width);
            break;
        case '%':
            conv_pct(o, width);
            break;
        default:
            /* Unrecognised conversion (incl. conv == '\0'): emit '%' plus the
             * offending byte verbatim so a format bug stays visible. */
            out_ch(o, '%');
            if (conv != '\0')
                out_ch(o, conv);
            break;
        }
    }
}

int
vsnprintf(char *str, size_t n, const char *fmt, va_list ap)
{
    struct fmtout o;

    o.buf = str;
    o.cap = n;
    o.pos = 0;
    o.total = 0;
    o.fd = -1;                  /* buffer-mode */
    vformat(&o, fmt, ap);
    return fmt_finish(&o);
}

int
vfprintf(FILE *stream, const char *fmt, va_list ap)
{
    char sbuf[PRINTF_BUFSZ];
    struct fmtout o;

    o.buf = sbuf;
    o.cap = PRINTF_BUFSZ;
    o.pos = 0;
    o.total = 0;
    o.fd = stream->fd;         /* flush-mode */
    vformat(&o, fmt, ap);
    return fmt_finish(&o);
}

int
vprintf(const char *fmt, va_list ap)
{
    return vfprintf(stdout, fmt, ap);
}

int
printf(const char *fmt, ...)
{
    va_list ap;
    int r;

    va_start(ap, fmt);
    r = vprintf(fmt, ap);
    va_end(ap);
    return r;
}

int
fprintf(FILE *stream, const char *fmt, ...)
{
    va_list ap;
    int r;

    va_start(ap, fmt);
    r = vfprintf(stream, fmt, ap);
    va_end(ap);
    return r;
}

int
snprintf(char *str, size_t n, const char *fmt, ...)
{
    va_list ap;
    int r;

    va_start(ap, fmt);
    r = vsnprintf(str, n, fmt, ap);
    va_end(ap);
    return r;
}
