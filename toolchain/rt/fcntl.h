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

#include "sys/types.h"   /* mode_t (creat) */

/* open() flags (x86-64 fixed; docs/design/hostfs.md section 2). */
#define O_RDONLY    0x0
#define O_WRONLY    0x1
#define O_RDWR      0x2
#define O_ACCMODE   0x3   /* maize-94: mask for the access-mode bits (oksh io.c/shf.c) */
#define O_CREAT     0x40
#define O_EXCL      0x80   /* maize-94: create-exclusive (oksh exec.c); hostfs has no
                            * exclusive-create, so it is accepted but not enforced. */
#define O_TRUNC     0x200
#define O_APPEND    0x400
#define O_NONBLOCK  0x800  /* maize-94: accepted, no-op (console/hostfs are blocking) */
#define O_CLOEXEC   0x80000 /* maize-94: accepted, no-op (no per-fd cloexec store) */
#define O_DIRECTORY 0x10000

/* lseek() whence values. */
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* open (maize-172): declared here, its POSIX home, so a TU that includes <fcntl.h>
 * for the O_* flags (kilo's editorSave) also sees the prototype. The body is the
 * errno-translating wrapper over sys_open in errno.c; the signature matches the one in
 * the Maize-private "syscall.h" (a duplicate typedef-free redeclaration is legal C, so a
 * TU that includes both headers still compiles).
 *
 * maize-94: open is VARIADIC (the POSIX signature), so borrowed code that calls the
 * two-argument form open(path, flags) compiles (sbase/oksh do this pervasively). The
 * O_CREAT permission mode is the optional third argument, read via va_arg only when
 * O_CREAT is set and defaulting to 0 otherwise; a three-argument call keeps working. */
int open(const char *path, int flags, ...);

/* creat (maize-94): the classic open-for-create shorthand borrowed sbase cp reaches
 * for; a pure libc composite over open (body in errno.c), no new syscall. */
int creat(const char *path, mode_t mode);

/* fcntl (maize-94): borrowed oksh's io.c uses fcntl(fd, F_DUPFD, FDBASE) to lift the
 * shell's own descriptors clear of the user range, and F_SETFD/FD_CLOEXEC to mark
 * them close-on-exec. quesOS has no per-fd flag store, so:
 *   F_DUPFD  -> a real lowest->=arg duplicate (dup loop, closing the intermediates).
 *   F_GETFD/F_SETFD/F_GETFL/F_SETFL -> no-op (returns 0). FD_CLOEXEC is not modeled;
 *     wave-1 pipelines dup the intended ends onto 0/1 before exec, so an un-cleared
 *     shell fd being inherited is benign (honest deviation, decision-noted).
 * F_* values are the frozen Linux numbers. Body in unistd.c (variadic like open). */
#define F_DUPFD 0
#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4

#define FD_CLOEXEC 1

int fcntl(int fd, int cmd, ...);

#endif /* MAIZE_FCNTL_H */
