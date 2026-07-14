/* toolchain/rt/syscall.h -- C binding for Maize syscalls (maize-74).
 *
 * Two layers, per the recorded convention (toolchain/rt/SYSCALL-ABI.md):
 *
 *   raw stubs   sys_read / sys_write / _exit  (toolchain/rt/syscall.mazm)
 *       Each is `SYS <number>; RET`. Args are the C ABI arg registers R0..R2;
 *       the result is whatever the VM leaves in RV, returned verbatim with NO
 *       error interpretation. A result in [-4095, -1] encodes -errno.
 *
 *   wrappers    read / write                  (toolchain/rt/errno.c)
 *       POSIX-named; each calls the matching raw stub and passes the result
 *       through __syscall_ret, which turns a [-4095, -1] result into
 *       errno = -result and a -1 return, and returns any other value verbatim.
 *
 * The hosted syscall numbers are frozen ABI (mirroring Linux x86-64):
 *   read = 0x00, write = 0x01, exit = 0x3C  (reboot = 0xA9 reserved).
 *
 * This header is preprocessed by the system cpp before cproc-qbe (run-ctest.sh),
 * so ordinary include guards and object-like macros are available.
 */
#ifndef MAIZE_SYSCALL_H
#define MAIZE_SYSCALL_H

/* The E* table and `extern int errno` live in errno.h now (maize-76 decision
 * 7348); syscall.h pulls them in so the wrappers below still see errno. */
#include "errno.h"

/* --- raw stubs (syscall.mazm) ---------------------------------------------- */
long sys_read(int fd, void *buf, unsigned long count);
long sys_write(int fd, const void *buf, unsigned long count);
_Noreturn void _exit(int code);

/* maize-114 hostfs raw stubs (SYS $02/$03/$05/$08/$D9). Each returns RV verbatim: a
 * non-negative result, or a value in [-4095, -1] encoding -errno (test the band with
 * `(unsigned long)r > -4096UL`). The POSIX-named, errno-translating wrappers are
 * deferred to the libc-growth line; the acceptance fixtures call these raw stubs. */
long sys_open(const char *path, int flags, int mode);
long sys_close(int fd);
long sys_fstat(int fd, void *statbuf);
long sys_lseek(int fd, long offset, int whence);
long sys_getdents64(int fd, void *dirp, unsigned long count);

/* sys_brk (SYS $0C, maize-75): R0 = requested break (0 queries); returns the
 * new-or-current break in RV, NEVER -errno. sbrk (stdlib.c) wraps this. */
void *sys_brk(void *addr);

/* sys_clock_ms (SYS $F0, maize-141): monotonic milliseconds since VM start.
 * Returns the count in RV; NEVER -errno (exempt from the errno convention,
 * cf. sys_brk). Monotonic non-decreasing; epoch arbitrary. */
unsigned long sys_clock_ms(void);

/* maize-140 termios raw stubs (SYS $F1 tcgetattr / $F2 tcsetattr). Each returns 0 or a
 * [-4095, -1] -errno (-EBADF when no window console is bound). The POSIX-named wrappers
 * tcgetattr()/tcsetattr() live in termios.c. `termios_p` is a struct termios* (termios.h),
 * declared void* here so syscall.h stays free of the termios type. */
long sys_tcgetattr(int fd, void *termios_p);
long sys_tcsetattr(int fd, int optional_actions, void *termios_p);

/* path-mutating raw stubs (SYS $57 unlink / $53 mkdir / $52 rename). maize-151 wires
 * these through the confined hostfs backends, so each returns RV verbatim as a real
 * non-negative result or a [-4095, -1] -errno (EROFS on a :ro mount or the synthetic
 * root, EXDEV on a cross-mount rename). remove()/mkdir()/rename() (errno.c) wrap these. */
long sys_unlink(const char *path);
long sys_mkdir(const char *path, int mode);
long sys_rename(const char *old, const char *new);

/* --- errno + wrappers (errno.c) -------------------------------------------- */

/* The musl error translator: a pure function of its input. A raw result in
 * [-4095, -1] (i.e. (unsigned long)r > -4096UL) is -errno; it sets errno and
 * returns -1. Any other value is a valid result, returned verbatim. */
long __syscall_ret(unsigned long r);

long read(int fd, void *buf, unsigned long count);
long write(int fd, const void *buf, unsigned long count);

/* maize-120: the POSIX-named, errno-translating descriptor wrappers over the
 * hostfs raw stubs. Each calls the matching sys_* stub and passes the result
 * through __syscall_ret exactly as read/write do. `mode` is open()'s O_CREAT
 * permission arg (ignored unless O_CREAT). Directory enumeration lives in the
 * dirent.c wrappers (opendir/readdir/closedir) over sys_getdents64. */
int  open (const char *path, int flags, int mode);
int  close(int fd);
long lseek(int fd, long offset, int whence);
int  fstat(int fd, void *statbuf);

#endif /* MAIZE_SYSCALL_H */
