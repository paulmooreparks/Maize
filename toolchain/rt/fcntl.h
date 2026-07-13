/* toolchain/rt/fcntl.h -- open flags and lseek whence constants for the Maize C
 * runtime (maize-120).
 *
 * These are the frozen x86-64 values pinned in docs/design/hostfs.md section 2 and
 * toolchain/rt/SYSCALL-ABI.md; the hostfs VM layer routes sys_open on exactly these
 * bits, so they are ABI, not implementation choices. fopen and any caller of the
 * open() wrapper include this header rather than hardcoding the magic numbers.
 */
#ifndef MAIZE_FCNTL_H
#define MAIZE_FCNTL_H

/* open() flags (x86-64 fixed; docs/design/hostfs.md section 2). */
#define O_RDONLY    0x0
#define O_WRONLY    0x1
#define O_RDWR      0x2
#define O_CREAT     0x40
#define O_TRUNC     0x200
#define O_APPEND    0x400
#define O_DIRECTORY 0x10000

/* lseek() whence values. */
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#endif /* MAIZE_FCNTL_H */
