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

/* --- raw stubs (syscall.mazm) ---------------------------------------------- */
long sys_read(int fd, void *buf, unsigned long count);
long sys_write(int fd, const void *buf, unsigned long count);
void _exit(int code);

/* --- errno + wrappers (errno.c) -------------------------------------------- */
extern int errno;

/* The musl error translator: a pure function of its input. A raw result in
 * [-4095, -1] (i.e. (unsigned long)r > -4096UL) is -errno; it sets errno and
 * returns -1. Any other value is a valid result, returned verbatim. */
long __syscall_ret(unsigned long r);

long read(int fd, void *buf, unsigned long count);
long write(int fd, const void *buf, unsigned long count);

/* Minimal errno constants, Linux x86-64 numbering. The VM does not yet produce
 * real -errno codes: today a failing call returns a bare -1, so the wrapper sets
 * errno to 1 (== EPERM here by coincidence, not by diagnosis). maize-75 makes the
 * VM return the correct code; the full E* table is maize-76. */
#define EPERM   1
#define EBADF   9
#define EINVAL 22

#endif /* MAIZE_SYSCALL_H */
