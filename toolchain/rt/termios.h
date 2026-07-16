/* toolchain/rt/termios.h -- freestanding <termios.h> slice for the Maize C runtime
 * (maize-140).
 *
 * The guest side of the window console's line discipline. A stdio program needs nothing
 * here: cooked line input and glyph output work with plain read()/write() when the
 * console is bound (maizeg --display / --console-dump). This header is for programs that
 * drive the discipline directly, notably a raw-mode editor (kilo, maize-172): the struct
 * and flag values mirror Linux termios so a Linux-written editor ports with no field
 * surgery, and the WIRE LAYOUT (four 32-bit flag words then NCCS control bytes, 36 bytes
 * total) is frozen to match the host console's termios image (src/console_io.h). tcgetattr
 * / tcsetattr copy that image across SYS $F1 / $F2; cfmakeraw is the usual convenience.
 */
#ifndef MAIZE_TERMIOS_H
#define MAIZE_TERMIOS_H

typedef unsigned int tcflag_t;
typedef unsigned char cc_t;
typedef unsigned int speed_t;

#define NCCS 20

/* Frozen 36-byte wire image: c_iflag/c_oflag/c_cflag/c_lflag (4 x 4 bytes), then c_cc[]. */
struct termios {
	tcflag_t c_iflag;
	tcflag_t c_oflag;
	tcflag_t c_cflag;
	tcflag_t c_lflag;
	cc_t c_cc[NCCS];
};

/* c_cc indices (Linux values; the console honors VERASE / VEOF / VMIN / VTIME). */
#define VERASE 2
#define VEOF   4
#define VTIME  5
#define VMIN   6

/* c_lflag bits (Linux values). */
#define ISIG   0x0001
#define ICANON 0x0002
#define ECHO   0x0008

/* c_iflag bits (Linux values). ICRNL maps input CR to NL; the remainder are the
   input flags a raw-mode editor (kilo, maize-172) clears when it hand-rolls raw
   mode instead of calling cfmakeraw. The console models the ICANON / ECHO / ISIG
   line discipline; the extra input bits below have no host effect (there is no
   parity, break, or XON/XOFF layer), so clearing them is a defined no-op. They
   are defined here only so a Linux-written editor compiles without field surgery. */
#define ICRNL  0x0100
#define BRKINT 0x0002
#define INPCK  0x0010
#define ISTRIP 0x0020
#define IXON   0x0400

/* c_oflag bits (Linux values). */
#define OPOST  0x0001
#define ONLCR  0x0004

/* c_cflag bit (Linux value): 8-bit characters. The console is always 8-bit clean,
   so CS8 is a defined no-op, defined for source compatibility with hand-rolled
   raw-mode setup. */
#define CS8    0x0030

/* c_lflag extended-input bit (Linux value): the console has no IEXTEN layer, so
   clearing it is a defined no-op. Defined for source compatibility. */
#define IEXTEN 0x8000

/* tcsetattr optional_actions (all equivalent for this line discipline). */
#define TCSANOW   0
#define TCSADRAIN 1
#define TCSAFLUSH 2

int tcgetattr(int fd, struct termios *termios_p);
int tcsetattr(int fd, int optional_actions, const struct termios *termios_p);

/* Put the terminal into raw mode (clear the canonical/echo/signal + CR/NL translation
   flags, deliver each byte). Mirrors the common cfmakeraw helper. */
void cfmakeraw(struct termios *termios_p);

#endif /* MAIZE_TERMIOS_H */
