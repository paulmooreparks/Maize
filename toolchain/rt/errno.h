/* toolchain/rt/errno.h -- errno declaration + the E* constant table for the
 * Maize C runtime (maize-76, decision 7348).
 *
 * The E* table previously lived inline in syscall.h with a "full E* table is
 * maize-76" note; that note is now discharged and the table lives here. errno
 * itself is stored (defined) in errno.c; this header only DECLARES it. syscall.h
 * #includes this header so the syscall wrappers keep seeing errno and the codes.
 *
 * Numbering mirrors Linux x86-64 (the same table cproc/glibc programs expect).
 * The VM does not yet emit the full range of -errno codes; the codes are declared
 * so runtime code (sbrk/malloc set ENOMEM; the __syscall_ret translator sets
 * whatever the VM returns) and future syscalls can name them.
 */
#ifndef MAIZE_ERRNO_H
#define MAIZE_ERRNO_H

/* Storage lives in errno.c (single-threaded VM; TLS reserved, maize-74 OQ3). */
extern int errno;

#define EPERM   1
#define ENOENT  2   /* open of a missing path (kilo, maize-172, checks errno != ENOENT) */
#define EBADF   9
#define ENOMEM 12   /* sbrk/malloc: request over HEAP_CEILING or below the floor */
#define EISDIR 21   /* m_misc.c:439: write target is a directory (maize-147) */
#define EINVAL 22
#define ENOTTY 25   /* ioctl on a non-ioctl backend; isatty on a non-tty (maize-172) */
#define ERANGE 34   /* strtol: value out of long range (overflow clamp), maize-142 */

#endif /* MAIZE_ERRNO_H */
