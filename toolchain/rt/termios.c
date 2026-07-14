/* toolchain/rt/termios.c -- POSIX-named termios wrappers over the raw SYS $F1/$F2 stubs
 * (maize-140).
 *
 * The errno-translating layer above sys_tcgetattr / sys_tcsetattr, mirroring the
 * open/close/read/write wrappers in errno.c: each calls the raw stub and passes the
 * result through __syscall_ret, so a failure (no window console bound, i.e. -EBADF)
 * surfaces as errno + a -1 return. cfmakeraw is pure field manipulation, no syscall.
 *
 * Compiled through the same cproc -> qbe -t maize -> mazm -c path as the other C runtime
 * modules and linked into every C image (cc-maize.sh RT set).
 */
#include "termios.h"
#include "syscall.h"

int
tcgetattr(int fd, struct termios *termios_p)
{
	return (int)__syscall_ret(sys_tcgetattr(fd, termios_p));
}

int
tcsetattr(int fd, int optional_actions, const struct termios *termios_p)
{
	return (int)__syscall_ret(sys_tcsetattr(fd, optional_actions, (void *)termios_p));
}

void
cfmakeraw(struct termios *termios_p)
{
	termios_p->c_iflag &= ~(tcflag_t)ICRNL;
	termios_p->c_oflag &= ~(tcflag_t)(OPOST | ONLCR);
	termios_p->c_lflag &= ~(tcflag_t)(ISIG | ICANON | ECHO);
	termios_p->c_cc[VMIN] = 1;
	termios_p->c_cc[VTIME] = 0;
}
