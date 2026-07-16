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
#include <stdarg.h>
#include "../syscall.h"

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
	/* maize-228: TIOCGWINSZ is answered by the real host terminal via SYS $F6 on the
	   console binary (so kilo's getWindowSize succeeds via ioctl instead of the ESC[6n
	   cursor-probe, which stalls a cooked read). The windowed console and non-tty host
	   stdio return -ENOTTY here, and the caller keeps its ESC[6n fallback. Every other
	   request still fails ENOTTY: Maize has no ioctl multiplexor. */
	if (request == TIOCGWINSZ) {
		va_list ap;
		void *ws;
		va_start(ap, request);
		ws = va_arg(ap, void *);
		va_end(ap);
		if (sys_ttysize(fd, ws) != 0) {
			errno = ENOTTY;
			return -1;
		}
		return 0;
	}
	(void)fd;
	errno = ENOTTY;
	return -1;
}

#endif /* MAIZE_SYS_IOCTL_H */
