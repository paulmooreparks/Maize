/* toolchain/rt/sys/ioctl.h -- freestanding <sys/ioctl.h> stub for the Maize C
 * runtime (maize-172).
 *
 * Maize has no ioctl multiplexor: device control that Linux funnels through ioctl
 * (termios TCGETS/TCSETS, window size TIOCGWINSZ) is reached instead through the
 * dedicated tcgetattr/tcsetattr syscalls (SYS $F1/$F2). ioctl() here therefore
 * always fails with ENOTTY. That is exactly the branch kilo (maize-172) is written
 * to expect: when ioctl(TIOCGWINSZ) fails it falls back to the portable VT100
 * cursor-position query (write ESC[999C ESC[999B, then ESC[6n and parse the
 * ESC[row;colR report). The Maize console answers ESC[6n on the input stream
 * (src/devices.cpp csi_dispatch), so the fallback carries window sizing with no
 * ioctl subsystem. struct winsize is defined so the caller's declaration compiles;
 * its fields are never populated by this stub.
 *
 * static inline: no object in the RT link set; only a TU that includes this header
 * (kilo) emits it.
 */
#ifndef MAIZE_SYS_IOCTL_H
#define MAIZE_SYS_IOCTL_H

#include <errno.h>

/* TIOCGWINSZ (Linux value). Defined only so the source compiles; ioctl() below
 * never acts on it. */
#define TIOCGWINSZ 0x5413

struct winsize {
	unsigned short ws_row;
	unsigned short ws_col;
	unsigned short ws_xpixel;
	unsigned short ws_ypixel;
};

static inline int
ioctl(int fd, unsigned long request, ...)
{
	(void)fd;
	(void)request;
	errno = ENOTTY;
	return -1;
}

#endif /* MAIZE_SYS_IOCTL_H */
