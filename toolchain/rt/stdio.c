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
#include "string.h"   /* strlen, memcpy, strerror (perror, maize-172) */
#include "stdlib.h"   /* malloc/free/realloc, atexit (maize-120) */
#include "syscall.h"  /* sys_write, read/write/lseek/close (maize-120) */
#include "fcntl.h"    /* O_* open flags, SEEK_* whence (maize-120) */
#include "ctype.h"    /* isspace (sscanf whitespace handling, maize-148) */
#include "errno.h"    /* errno (perror, maize-172) */

/* FILE::flags bits (maize-120). readable/writable are the mode; eof/error are the
 * sticky status bits feof/ferror report; unbuffered marks stdout/stderr (direct
 * sys_write, no buffer) apart from the fully buffered fopen'd streams. */
#define _F_READ   0x01
#define _F_WRITE  0x02
#define _F_EOF    0x04
#define _F_ERR    0x08
#define _F_UNBUF  0x10

/* Static stream objects; the public handles are pointers to them (decision 7359).
 * Both are unbuffered write streams, so the widened tail is zero/NULL and only the
 * flags carry _F_WRITE|_F_UNBUF. Field order matches struct _FILE in stdio.h. */
static FILE _stdout = { STDOUT_FILENO, _F_WRITE | _F_UNBUF, 0, NULL, 0, 0, 0, NULL };
static FILE _stderr = { STDERR_FILENO, _F_WRITE | _F_UNBUF, 0, NULL, 0, 0, 0, NULL };
FILE *stdout = &_stdout;
FILE *stderr = &_stderr;

/* stdin (maize-292): unlike stdout/stderr, fread/fill_rbuf have no unbuffered-read
 * fast path (they always read into stream->buf sized stream->bufcap), so stdin needs
 * a REAL backing buffer, not NULL. Read-only: not threaded onto g_streams (that list
 * exists for buffered WRITE streams; __stdio_flush_all only flushes mode == 2
 * entries), matching the precedent stdout/stderr already set by staying off it too. */
static unsigned char _stdin_buf[BUFSIZ];
static FILE _stdin = { STDIN_FILENO, _F_READ, 0, _stdin_buf, BUFSIZ, 0, 0, NULL };
FILE *stdin = &_stdin;

/* Head of the open (fopen'd) buffered-stream list, and the one-shot guard that arms
 * the atexit flush hook the first time a buffered stream is created (maize-120). */
static FILE *g_streams = NULL;
static int   g_flush_armed = 0;

int
fputc(int c, FILE *stream)
{
    unsigned char ch = (unsigned char)c;
    if (stream->flags & _F_UNBUF) {                 /* stdout/stderr: direct write */
        if (sys_write(stream->fd, &ch, 1) != 1)
            return EOF;
        return (int)ch;
    }
    if (fwrite(&ch, 1, 1, stream) != 1)             /* buffered: through the buffer */
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
    if (n == 0)
        return 0;
    if (stream->flags & _F_UNBUF) {                 /* stdout/stderr: direct write */
        if (sys_write(stream->fd, s, n) != (long)n)
            return EOF;
        return 0;
    }
    if (fwrite(s, 1, n, stream) != n)               /* buffered: through the buffer */
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
 * left-padded to `width`. Two distinct pads compose here (maize-144): the
 * PRECISION pad zero-fills the body to at least `prec` digits and lands AFTER the
 * sign (%.3d of -7 -> "-007"); the WIDTH pad fills to `width`. Precision applies
 * only to the integer conversions (callers that ignore it pass prec == -1). Per C,
 * a present precision (prec >= 0) suppresses the '0' (zero-pad-to-width) flag, so
 * the flag zero-pad is taken only when prec is absent (usez); otherwise the width
 * pad is spaces before the sign. This preserves the %05d of -7 -> "-0007" path
 * (prec absent, usez true) while adding the precision path. Single shared emitter. */
static void
emit_field(struct fmtout *o, char sign, const char *body, size_t blen,
           int width, int zero, int prec)
{
    size_t signlen = sign ? 1u : 0u;
    size_t zpad    = (prec > (int)blen) ? (size_t)prec - blen : 0u; /* precision min-digits */
    int    usez    = zero && prec < 0;                             /* precision disables 0-flag */
    size_t content = signlen + zpad + blen;
    size_t wpad    = (width > 0 && (size_t)width > content)
                     ? (size_t)width - content : 0u;

    if (usez) {
        if (sign)
            out_ch(o, sign);
        out_rep(o, '0', wpad);          /* width zero-pad, after sign */
        out_bytes(o, body, blen);
    } else {
        out_rep(o, ' ', wpad);          /* width space-pad, before sign */
        if (sign)
            out_ch(o, sign);
        out_rep(o, '0', zpad);          /* precision zeros, after sign */
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

/* Unsigned numeric conversion (%u/%x/%X and the magnitude of %d/%i). prec is the
 * minimum digit count, threaded to emit_field. The C corner "%.0d of 0 emits no
 * digits" is handled here: precision 0 of a zero magnitude yields an empty body. */
static void
emit_uint(struct fmtout *o, unsigned long mag, unsigned base, int upper,
          char sign, int width, int zero, int prec)
{
    char body[20];
    size_t n = u_to_digits(mag, base, upper, body);
    if (prec == 0 && mag == 0)          /* %.0d of 0 -> no digits */
        n = 0;
    emit_field(o, sign, body, n, width, zero, prec);
}

/* Signed decimal via the unsigned-negate idiom so INT_MIN / LONG_MIN do not
 * overflow (a naive -v is UB at the extreme). v is already widened to long. */
static void
emit_signed(struct fmtout *o, long v, int width, int zero, int prec)
{
    unsigned long mag = (unsigned long)v;
    char sign = 0;

    if (v < 0) {
        sign = '-';
        mag = -mag;             /* modular negate: correct for LONG_MIN too */
    }
    emit_uint(o, mag, 10, 0, sign, width, zero, prec);
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
    emit_field(o, 0, &ch, 1, width, 0, -1);             /* precision/zero n/a for %c */
}

/* String conversion. precision (prec >= 0) is the MAXIMUM characters emitted, so a
 * "%.3s" of "hello" prints "hel"; cap the length before the field emit. The field
 * emitter is called with prec == -1 because integer-precision zero-fill must never
 * apply to a string body (its own precision was already consumed as the cap here). */
static void
conv_s(struct fmtout *o, const char *s, int width, int prec)
{
    size_t len;
    if (s == NULL)
        s = fmt_nullstr;
    len = strlen(s);
    if (prec >= 0 && (size_t)prec < len)
        len = (size_t)prec;
    emit_field(o, 0, s, len, width, 0, -1);
}

static void
conv_p(struct fmtout *o, void *ptr, int width)
{
    char pbuf[2 + 16];                                  /* "0x" + 16 hex digits */
    size_t n;
    pbuf[0] = '0';
    pbuf[1] = 'x';
    n = u_to_digits((unsigned long)ptr, 16, 0, pbuf + 2);
    emit_field(o, 0, pbuf, 2 + n, width, 0, -1);        /* precision n/a for %p */
}

static void
conv_pct(struct fmtout *o, int width)
{
    char pct = '%';
    emit_field(o, 0, &pct, 1, width, 0, -1);            /* precision n/a for %% */
}

/* The one conversion loop. Walks fmt: literal bytes go straight to out_ch; on '%'
 * it parses [ '0' ] [ 1*DIGIT width ] [ '.' precision ] [ 'l' ] conv and emits via
 * out_ch. precision follows C's %[flags][width][.precision][length]conv order: a
 * bare "%.d" is precision 0, ".N" is literal, ".*" pulls the precision from an int
 * arg (maize-144), and a negative .* precision counts as absent (prec == -1). */
static void
vformat(struct fmtout *o, const char *fmt, va_list ap)
{
    const char *p = fmt;

    while (*p) {
        char c = *p++;
        int zero, width, lng, prec;
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
        prec = -1;                                      /* -1 == precision absent */
        if (*p == '0') { zero = 1; p++; }               /* zero-pad flag */
        if (*p == '*') { width = va_arg(ap, int); p++; } /* width from an int arg (%*d/%*s) */
        else while (*p >= '0' && *p <= '9')             /* minimum field width */
            width = width * 10 + (*p++ - '0');
        if (*p == '.') {                                /* precision (.N or .*) */
            p++;
            prec = 0;                                   /* bare "%.d" == precision 0 */
            if (*p == '*') { prec = va_arg(ap, int); p++; }
            else while (*p >= '0' && *p <= '9')
                prec = prec * 10 + (*p++ - '0');
            if (prec < 0) prec = -1;                    /* C rule: negative .* == omitted */
        }
        /* Length modifier (maize-94): l / ll (long, long long) and z / j / t
         * (size_t, intmax_t, ptrdiff_t) are all 64-bit on Maize's LP64 ABI, so
         * each selects the wide (long) va_arg fetch; borrowed sbase printf emits
         * "%*.*lld" and friends. h / hh (short, char) are consumed but leave the
         * default int fetch, since varargs already promote them to int. */
        while (*p == 'l' || *p == 'z' || *p == 'j' || *p == 't') { lng = 1; p++; }
        while (*p == 'h') { p++; }

        conv = *p;
        if (conv != '\0')
            p++;

        switch (conv) {
        case 'd':
        case 'i':
            emit_signed(o, lng ? va_arg(ap, long) : (long)va_arg(ap, int),
                        width, zero, prec);
            break;
        case 'u':
            emit_uint(o, lng ? va_arg(ap, unsigned long)
                             : (unsigned long)va_arg(ap, unsigned int),
                      10, 0, 0, width, zero, prec);
            break;
        case 'x':
            emit_uint(o, lng ? va_arg(ap, unsigned long)
                             : (unsigned long)va_arg(ap, unsigned int),
                      16, 0, 0, width, zero, prec);
            break;
        case 'X':
            emit_uint(o, lng ? va_arg(ap, unsigned long)
                             : (unsigned long)va_arg(ap, unsigned int),
                      16, 1, 0, width, zero, prec);
            break;
        case 'c':
            conv_c(o, va_arg(ap, int), width);
            break;
        case 's':
            conv_s(o, va_arg(ap, const char *), width, prec);
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

    /* On a buffered stream, drain any pending write-buffer bytes first so formatted
     * output cannot land ahead of earlier fwrite bytes (decision 8288); then emit via
     * the existing chunked direct-to-fd path. For unbuffered stdout/stderr this branch
     * is skipped and the body is byte-for-byte the pre-maize-120 formatter. */
    if (!(stream->flags & _F_UNBUF))
        fflush(stream);

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

/* sprintf (maize-120): vsnprintf with an effectively-unbounded cap. vsnprintf's
 * buffer-mode NUL-terminates and counts correctly, so (size_t)-1 as n makes it behave
 * as an unbounded vsprintf while reusing the single formatter. DOOM uses it heavily. */
int
sprintf(char *str, const char *fmt, ...)
{
    va_list ap;
    int r;

    va_start(ap, fmt);
    r = vsnprintf(str, (size_t)-1, fmt, ap);
    va_end(ap);
    return r;
}

/* ------------------------------------------------------------------------------ */
/* sscanf: a scanf core over a NUL-terminated string source (maize-148).          */
/*                                                                                */
/* Conversion set: %d %i %u %o %x %f %s %c %%, plus optional field width, the `*`  */
/* assignment-suppression flag, and the `l` length modifier (%ld -> long*,         */
/* %lf -> double*); h/ll/hh are parsed and tolerated. %e/%g and the %[...] scanset  */
/* are out of scope (DOOM's m_config.c parser uses the %d/%f/%s core). Returns the  */
/* number of successful ASSIGNMENTS (suppressed conversions and %% do not count),   */
/* or EOF if the input was exhausted before the first conversion produced a value.  */
/*                                                                                */
/* Per the pinned qbe -t maize authoring budget (the vformat precedent above), the  */
/* work is split into small static helpers with ONE primary loop each; scan_str /   */
/* scan_char call scan_ws rather than inlining a second pointer-indexing loop.      */
/* ------------------------------------------------------------------------------ */

/* Map a character to its digit value in `base`, or -1 if it is not a valid digit in
 * that base (0-9, then a-z / A-Z for 10..35). No loop. */
static int
scan_digit(int c, int base)
{
    int d;
    if (c >= '0' && c <= '9')
        d = c - '0';
    else if (c >= 'a' && c <= 'z')
        d = c - 'a' + 10;
    else if (c >= 'A' && c <= 'Z')
        d = c - 'A' + 10;
    else
        return -1;
    return d < base ? d : -1;
}

/* Advance *pp past a run of whitespace. The one loop for both the driver's format
 * whitespace directive and the leading-whitespace skip of scan_str/scan_char. */
static void
scan_ws(const char **pp)
{
    const char *p = *pp;
    while (isspace((unsigned char)*p))
        p++;
    *pp = p;
}

/* Parse an optional sign + base-`base` digits, bounded by `width` (-1 == unlimited),
 * reporting the unsigned magnitude and sign. `base == 0` (from %i) autodetects: a 0x
 * prefix -> 16, a leading 0 -> 8, else 10; a 0x prefix is also consumed for base 16.
 * Returns 1 if at least one digit was consumed, else 0. One primary loop. */
static int
scan_int(const char **pp, int base, int width, int *neg, unsigned long *mag)
{
    const char *p = *pp;
    int w = width;                  /* remaining allowance; -1 stays unlimited */
    int sign = 0;
    int any = 0;
    unsigned long acc = 0;

    if (w != 0 && (*p == '+' || *p == '-')) {
        sign = (*p == '-');
        p++;
        if (w > 0) w--;
    }

    /* Resolve base 0 (%i) and consume an optional 0x/0X prefix for hex. The width
     * guard (w < 0 || w >= 3) keeps a positive width from underflowing past the
     * prefix; a bare leading 0 stays for the digit loop to consume as octal 0. */
    if (base == 0) {
        if ((w < 0 || w >= 3) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')
            && scan_digit((unsigned char)p[2], 16) >= 0) {
            base = 16;
            p += 2;
            if (w > 0) w -= 2;
        } else if (p[0] == '0') {
            base = 8;
        } else {
            base = 10;
        }
    } else if (base == 16) {
        if ((w < 0 || w >= 3) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')
            && scan_digit((unsigned char)p[2], 16) >= 0) {
            p += 2;
            if (w > 0) w -= 2;
        }
    }

    for (;;) {
        int d;
        if (w == 0)
            break;
        d = scan_digit((unsigned char)*p, base);
        if (d < 0)
            break;
        acc = acc * (unsigned long)base + (unsigned long)d;
        any = 1;
        p++;
        if (w > 0) w--;
    }

    if (!any)
        return 0;
    *neg = sign;
    *mag = acc;
    *pp = p;
    return 1;
}

/* Parse a decimal float (the maize-144 atof algorithm, advancing *pp and reporting
 * success): optional sign, integer digits, optional '.'+fraction, bounded by `width`
 * (-1 == unlimited). Signed-int -> double path only, negate via 0.0 - x (never the
 * unsigned->float or `neg` the backend lacks). Returns 1 if any digit was consumed. */
static int
scan_float(const char **pp, int width, double *out)
{
    const char *p = *pp;
    int w = width;
    int neg = 0;
    int any = 0;
    double result = 0.0;
    double scale;

    if (w != 0 && (*p == '+' || *p == '-')) {
        neg = (*p == '-');
        p++;
        if (w > 0) w--;
    }
    while (w != 0 && *p >= '0' && *p <= '9') {      /* integer digits */
        int d = *p - '0';
        result = result * 10.0 + (double)d;
        any = 1;
        p++;
        if (w > 0) w--;
    }
    if (w != 0 && *p == '.') {                      /* optional '.'+fraction */
        p++;
        if (w > 0) w--;
        scale = 0.1;
        while (w != 0 && *p >= '0' && *p <= '9') {
            int d = *p - '0';
            result = result + (double)d * scale;
            scale = scale * 0.1;
            any = 1;
            p++;
            if (w > 0) w--;
        }
    }
    if (!any)
        return 0;
    *out = neg ? (0.0 - result) : result;
    *pp = p;
    return 1;
}

/* Copy a whitespace-delimited token into dst (NUL-terminated), up to `width` chars
 * (-1 == unlimited); dst == NULL suppresses the store. Leading whitespace is skipped
 * via scan_ws (a call, so this body keeps ONE loop). Returns the char count. */
static int
scan_str(const char **pp, char *dst, int width)
{
    const char *p;
    int w = width;
    int n = 0;

    scan_ws(pp);
    p = *pp;
    while (w != 0 && *p != '\0' && !isspace((unsigned char)*p)) {
        if (dst)
            dst[n] = *p;
        n++;
        p++;
        if (w > 0) w--;
    }
    if (dst)
        dst[n] = '\0';
    *pp = p;
    return n;
}

/* Copy exactly `width` chars (default 1) with NO leading-whitespace skip and NO NUL
 * terminator; dst == NULL suppresses the store. Returns the char count. One loop. */
static int
scan_char(const char **pp, char *dst, int width)
{
    const char *p = *pp;
    int w = (width < 0) ? 1 : width;
    int n = 0;

    while (n < w && *p != '\0') {
        if (dst)
            dst[n] = *p;
        n++;
        p++;
    }
    *pp = p;
    return n;
}

int
sscanf(const char *str, const char *format, ...)
{
    va_list ap;
    const char *ip = str;
    const char *fp = format;
    int count = 0;          /* successful assignments (the return value) */
    int stop = 0;           /* set on a matching failure to end the scan */
    int attempted = 0;      /* a real conversion (not %%) was attempted */

    va_start(ap, format);

    while (*fp != '\0' && !stop) {
        char fc = *fp;
        int suppress, width, lng;
        char conv;

        if (isspace((unsigned char)fc)) {           /* format whitespace: skip input ws */
            scan_ws(&ip);
            fp++;
            continue;
        }
        if (fc != '%') {                            /* ordinary char: must match input */
            if (*ip != fc) {
                stop = 1;
                break;
            }
            ip++;
            fp++;
            continue;
        }

        /* fc == '%': parse [*] [width] [l|h|ll|hh] conv. */
        fp++;
        suppress = 0;
        width = -1;                                 /* -1 == no width */
        lng = 0;
        if (*fp == '*') { suppress = 1; fp++; }
        if (*fp >= '0' && *fp <= '9') {
            width = 0;
            while (*fp >= '0' && *fp <= '9')
                width = width * 10 + (*fp++ - '0');
        }
        while (*fp == 'l' || *fp == 'h') {          /* length modifiers */
            if (*fp == 'l') lng = 1;                /* h/hh best-effort ignored */
            fp++;
        }
        conv = *fp;
        if (conv != '\0')
            fp++;

        if (conv == '%') {
            if (*ip == '%')
                ip++;
            else
                stop = 1;
            continue;
        }

        attempted = 1;

        if (conv == 'd' || conv == 'i' || conv == 'u'
            || conv == 'o' || conv == 'x') {
            int base;
            unsigned long mag;
            int neg;

            if (conv == 'i')      base = 0;
            else if (conv == 'o') base = 8;
            else if (conv == 'x') base = 16;
            else                  base = 10;

            scan_ws(&ip);
            if (!scan_int(&ip, base, width, &neg, &mag)) {
                stop = 1;
            } else if (!suppress) {
                unsigned long uval = neg ? (unsigned long)(-(long)mag) : mag;
                void *dst = va_arg(ap, void *);
                if (lng)
                    *(long *)dst = (long)uval;
                else
                    *(int *)dst = (int)uval;
                count++;
            }
        } else if (conv == 'f') {
            double dv;
            scan_ws(&ip);
            if (!scan_float(&ip, width, &dv)) {
                stop = 1;
            } else if (!suppress) {
                void *dst = va_arg(ap, void *);
                if (lng)
                    *(double *)dst = dv;
                else
                    *(float *)dst = (float)dv;
                count++;
            }
        } else if (conv == 's') {
            char *dst = suppress ? (char *)0 : va_arg(ap, char *);
            if (scan_str(&ip, dst, width) == 0)
                stop = 1;
            else if (!suppress)
                count++;
        } else if (conv == 'c') {
            char *dst = suppress ? (char *)0 : va_arg(ap, char *);
            if (scan_char(&ip, dst, width) == 0)
                stop = 1;
            else if (!suppress)
                count++;
        } else {
            stop = 1;                               /* unsupported conversion */
        }
    }

    va_end(ap);

    /* Input-failure (EOF) rule: no assignment made and a conversion was attempted on
     * an exhausted input. A matching failure on PRESENT input returns 0, not EOF. */
    if (count == 0 && attempted && *ip == '\0')
        return EOF;
    return count;
}

/* ------------------------------------------------------------------------------ */
/* File-backed FILE* layer (maize-120).                                           */
/*                                                                                */
/* fopen'd streams are fully buffered: one BUFSIZ buffer used one direction at a  */
/* time (mode 1 = read, 2 = write). stdout/stderr stay unbuffered (_F_UNBUF), so  */
/* every helper below short-circuits them to the direct sys_write path and the    */
/* existing fixtures stay byte-identical. Each function is kept small and uses     */
/* memcpy for bulk copies rather than an open-coded second loop, staying inside    */
/* the pinned qbe-maize backend's budget (see the header comment on stdio.c).      */
/* ------------------------------------------------------------------------------ */

/* Parse an fopen mode string into O_* open flags and the FILE readable/writable
 * bits. A 'b' is accepted and ignored (no text-mode translation); a '+' anywhere
 * makes the stream read+write. Returns 0 on an unrecognized leading char. */
static int
parse_mode(const char *m, int *oflags, int *fbits)
{
    char c = m[0];
    int plus = 0;
    const char *p = m;

    while (*p) {
        if (*p == '+')
            plus = 1;                       /* 'b' / other trailing chars ignored */
        p++;
    }
    if (c == 'r') {
        *oflags = plus ? O_RDWR : O_RDONLY;
        *fbits  = plus ? (_F_READ | _F_WRITE) : _F_READ;
    } else if (c == 'w') {
        *oflags = plus ? (O_RDWR | O_CREAT | O_TRUNC)
                       : (O_WRONLY | O_CREAT | O_TRUNC);
        *fbits  = plus ? (_F_READ | _F_WRITE) : _F_WRITE;
    } else if (c == 'a') {
        *oflags = plus ? (O_RDWR | O_CREAT | O_APPEND)
                       : (O_WRONLY | O_CREAT | O_APPEND);
        *fbits  = plus ? (_F_READ | _F_WRITE) : _F_WRITE;
    } else {
        return 0;
    }
    return 1;
}

/* Refill the read buffer with one read(). Returns bytes read (>0), 0 at EOF (sets
 * the EOF flag), or -1 on error (sets the error flag). */
static long
fill_rbuf(FILE *f)
{
    long n = read(f->fd, f->buf, (unsigned long)f->bufcap);

    f->bufpos = 0;
    if (n < 0) {
        f->flags |= _F_ERR;
        f->buflen = 0;
        return -1;
    }
    if (n == 0) {
        f->flags |= _F_EOF;
        f->buflen = 0;
        return 0;
    }
    f->buflen = n;
    return n;
}

/* Write out any pending write-buffer bytes. Returns 0 on success (or nothing to
 * flush), -1 on a short/failed write (sets the error flag). */
static int
flush_wbuf(FILE *f)
{
    if (f->bufpos > 0) {
        long w = write(f->fd, f->buf, (unsigned long)f->bufpos);
        if (w != f->bufpos) {
            f->flags |= _F_ERR;
            return -1;
        }
        f->bufpos = 0;
    }
    return 0;
}

/* Pull one byte through the read buffer; EOF at end/error. */
static int
rbuf_getc(FILE *f)
{
    if (f->bufpos >= f->buflen) {
        if (fill_rbuf(f) <= 0)
            return EOF;
    }
    return (int)f->buf[f->bufpos++];
}

FILE *
fopen(const char *path, const char *mode)
{
    int oflags, fbits, fd;
    FILE *f;
    unsigned char *b;

    if (!parse_mode(mode, &oflags, &fbits))
        return NULL;
    fd = open(path, oflags, 0644);
    if (fd < 0)
        return NULL;
    f = malloc(sizeof(FILE));
    if (f == NULL) {
        close(fd);
        return NULL;
    }
    b = malloc(BUFSIZ);
    if (b == NULL) {
        close(fd);
        free(f);
        return NULL;
    }
    f->fd = fd;
    f->flags = fbits;
    f->mode = 0;
    f->buf = b;
    f->bufcap = BUFSIZ;
    f->bufpos = 0;
    f->buflen = 0;
    f->next = g_streams;                    /* thread onto the open-stream list */
    g_streams = f;

    /* Arm the atexit flush hook once, now that a buffered stream exists (maize-120,
     * decision 8283): exit() then flushes on a return-from-main; _Exit/abort bypass. */
    if (!g_flush_armed) {
        g_flush_armed = 1;
        atexit(__stdio_flush_all);
    }
    return f;
}

/* True for the three static stream objects (stdin/stdout/stderr, maize-292 cycle 2
 * fix): none of them is heap-allocated, so fclose must never free their buffer or
 * the FILE object itself. Before maize-292 this was a safe no-op only because
 * stdout/stderr's buf was NULL (free(NULL) is a no-op) and the process typically
 * exited right after; stdin's new real backing buffer (_stdin_buf) turned the same
 * unconditional free() pair into live memory corruption, since fshut(stdin, ...) is
 * called unconditionally by every stdin-consuming sbase tool on a normal run. */
static int
is_static_stream(FILE *stream)
{
    return stream == &_stdin || stream == &_stdout || stream == &_stderr;
}

int
fclose(FILE *stream)
{
    int rc = 0;
    FILE *p;

    if (stream->mode == 2 && flush_wbuf(stream) != 0)
        rc = -1;

    /* Unlink from the open-stream list. */
    if (g_streams == stream) {
        g_streams = stream->next;
    } else {
        p = g_streams;
        while (p != NULL && p->next != stream)
            p = p->next;
        if (p != NULL)
            p->next = stream->next;
    }

    if (close(stream->fd) != 0)
        rc = -1;
    if (!is_static_stream(stream)) {
        free(stream->buf);
        free(stream);
    }
    return rc;
}

size_t
fread(void *ptr, size_t size, size_t nmemb, FILE *stream)
{
    size_t total = size * nmemb;
    size_t got = 0;
    unsigned char *out = (unsigned char *)ptr;

    if (total == 0)
        return 0;
    stream->mode = 1;
    while (got < total) {
        long avail = stream->buflen - stream->bufpos;
        long chunk;

        if (avail <= 0) {
            if (fill_rbuf(stream) <= 0)
                break;                       /* EOF or error: return short count */
            avail = stream->buflen - stream->bufpos;
        }
        chunk = avail;
        if ((size_t)chunk > total - got)
            chunk = (long)(total - got);
        memcpy(out + got, stream->buf + stream->bufpos, (size_t)chunk);
        stream->bufpos += chunk;
        got += (size_t)chunk;
    }
    return got / size;
}

size_t
fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream)
{
    size_t total = size * nmemb;
    size_t put = 0;
    const unsigned char *in = (const unsigned char *)ptr;

    if (total == 0)
        return 0;

    /* Unbuffered stdout/stderr: one direct write, byte-identical to the old path. */
    if (stream->flags & _F_UNBUF) {
        long w = write(stream->fd, ptr, total);
        if (w < 0) {
            stream->flags |= _F_ERR;
            return 0;
        }
        return (size_t)w / size;
    }

    stream->mode = 2;
    while (put < total) {
        long room = stream->bufcap - stream->bufpos;
        long chunk;

        if (room <= 0) {
            if (flush_wbuf(stream) != 0)
                break;
            room = stream->bufcap - stream->bufpos;
        }
        chunk = room;
        if ((size_t)chunk > total - put)
            chunk = (long)(total - put);
        memcpy(stream->buf + stream->bufpos, in + put, (size_t)chunk);
        stream->bufpos += chunk;
        put += (size_t)chunk;
    }
    return put / size;
}

char *
fgets(char *s, int n, FILE *stream)
{
    int i = 0;

    if (n <= 0)
        return NULL;
    stream->mode = 1;
    while (i < n - 1) {
        int c = rbuf_getc(stream);
        if (c == EOF)
            break;
        s[i++] = (char)c;
        if (c == '\n')
            break;
    }
    if (i == 0)
        return NULL;                         /* EOF/error before any char */
    s[i] = '\0';
    return s;
}

/* getline (maize-172): read one line through rbuf_getc, growing *lineptr as needed.
 * Doubles the buffer (from a 128-byte floor) on demand and always keeps room for the
 * NUL. Returns the character count (including the newline), or -1 with nothing read. */
ssize_t
getline(char **lineptr, size_t *n, FILE *stream)
{
    size_t len = 0;
    int c;

    if (lineptr == NULL || n == NULL)
        return -1;
    if (*lineptr == NULL || *n == 0) {
        size_t cap = 128;
        char *b = malloc(cap);
        if (b == NULL)
            return -1;
        *lineptr = b;
        *n = cap;
    }

    stream->mode = 1;
    for (;;) {
        c = rbuf_getc(stream);
        if (c == EOF)
            break;
        /* Ensure room for this char plus the terminating NUL. */
        if (len + 1 >= *n) {
            size_t newcap = *n * 2;
            char *nb = realloc(*lineptr, newcap);
            if (nb == NULL)
                return -1;
            *lineptr = nb;
            *n = newcap;
        }
        (*lineptr)[len++] = (char)c;
        if (c == '\n')
            break;
    }

    if (len == 0)
        return -1;                           /* EOF before any char */
    (*lineptr)[len] = '\0';
    return (ssize_t)len;
}

int
fseek(FILE *stream, long offset, int whence)
{
    if (stream->mode == 2 && flush_wbuf(stream) != 0)
        return -1;
    stream->bufpos = 0;                      /* discard buffered read data */
    stream->buflen = 0;
    stream->mode = 0;
    if (lseek(stream->fd, offset, whence) < 0)
        return -1;
    stream->flags &= ~_F_EOF;
    return 0;
}

long
ftell(FILE *stream)
{
    long pos = lseek(stream->fd, 0, SEEK_CUR);

    if (pos < 0)
        return -1;
    if (stream->mode == 1)                   /* buffered-but-unconsumed read bytes */
        pos -= (stream->buflen - stream->bufpos);
    else if (stream->mode == 2)              /* buffered-but-unwritten write bytes */
        pos += stream->bufpos;
    return pos;
}

int
fflush(FILE *stream)
{
    FILE *p;
    int rc = 0;

    if (stream == NULL) {                    /* flush every open buffered stream */
        p = g_streams;
        while (p != NULL) {
            if (p->mode == 2 && flush_wbuf(p) != 0)
                rc = -1;
            p = p->next;
        }
        return rc;
    }
    if (stream->flags & _F_UNBUF)
        return 0;                            /* nothing buffered */
    if (stream->mode == 2)
        return flush_wbuf(stream);
    return 0;
}

int
feof(FILE *stream)
{
    return (stream->flags & _F_EOF) ? 1 : 0;
}

int
ferror(FILE *stream)
{
    return (stream->flags & _F_ERR) ? 1 : 0;
}

void
clearerr(FILE *stream)
{
    stream->flags &= ~(_F_EOF | _F_ERR);
}

/* Registered on the atexit registry by the first fopen (maize-120). Walks the open-
 * stream list and flushes each buffered write stream so a program that returns from
 * main without fclose still lands its bytes on the host. _Exit/abort bypass atexit. */
void
__stdio_flush_all(void)
{
    FILE *p = g_streams;

    while (p != NULL) {
        if (p->mode == 2)
            flush_wbuf(p);
        p = p->next;
    }
}

/* perror (maize-172): "s: <errno message>\n" on stderr, prefix omitted when s is
 * NULL/empty. One fprintf keeps the two forms on the single formatter. */
void
perror(const char *s)
{
    if (s != NULL && s[0] != '\0')
        fprintf(stderr, "%s: %s\n", s, strerror(errno));
    else
        fprintf(stderr, "%s\n", strerror(errno));
}

/* fileno (maize-94): the descriptor backing a stream (oksh history.c / io.c). */
int
fileno(FILE *stream)
{
	if (stream == 0) {
		return -1;
	}
	return stream->fd;
}

/* rewind (maize-94): fseek to the start, ignoring the return (ISO C). */
void
rewind(FILE *stream)
{
	if (stream != 0) {
		(void)fseek(stream, 0L, 0 /* SEEK_SET */);
	}
}

/* fgetc / getc / getchar (maize-94): single-byte reads. fgetc/getc go through the
 * buffered fread path; getchar still reads fd 0 directly rather than routing through
 * the maize-292 stdin FILE* (its own long-standing direct-fd contract, unchanged by
 * that card). Return the byte (0..255) or EOF at end/error. */
int
fgetc(FILE *stream)
{
	unsigned char c;
	if (fread(&c, 1, 1, stream) != 1) {
		return EOF;
	}
	return (int)c;
}

int
getc(FILE *stream)
{
	return fgetc(stream);
}

int
getchar(void)
{
	unsigned char c;
	long n = read(0, &c, 1);
	if (n != 1) {
		return EOF;
	}
	return (int)c;
}
