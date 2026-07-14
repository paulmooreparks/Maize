/* toolchain/rt/unistd.h -- freestanding <unistd.h> slice for the Maize C runtime
 * (maize-147).
 *
 * DOOM's i_timer.c / i_system.c call usleep; strict cproc needs a visible declaration
 * at each call site. usleep's body lives in the sibling libc card (maize-148). Return
 * int, arg unsigned (avoids needing useconds_t).
 *
 * maize-172 (kilo) widens this to the standard POSIX <unistd.h> surface an editor
 * reaches for: the read/write/close descriptor wrappers (whose bodies live in errno.c
 * over syscall.h) are RE-DECLARED here with signatures byte-identical to syscall.h so a
 * TU that includes <unistd.h> instead of the Maize-private "syscall.h" still sees a
 * prototype (a duplicate typedef-free redeclaration is legal C). isatty and ftruncate
 * are new (bodies in unistd.c).
 */
#ifndef MAIZE_UNISTD_H
#define MAIZE_UNISTD_H

int usleep(unsigned useconds);

/* POSIX descriptor wrappers (bodies in errno.c). Signatures MUST match syscall.h. */
long read(int fd, void *buf, unsigned long count);
long write(int fd, const void *buf, unsigned long count);
int  close(int fd);

/* isatty (maize-172): 1 if fd is a bound console tty, 0 otherwise (errno = ENOTTY).
 * Implemented over tcgetattr, which succeeds only for a console-backed fd. */
int isatty(int fd);

/* ftruncate (maize-172): PARTIAL. Maize has no truncate syscall yet (SYSCALL-ABI.md
 * reserves it as not-implemented), so this does NOT resize the file; it returns 0.
 * A caller that rewrites the whole file (kilo's save writes the full buffer) is
 * correct when the new content is at least as long as the old file; shrinking a file
 * leaves a stale tail. The real truncate syscall is a follow-up card. */
int ftruncate(int fd, long length);

#endif /* MAIZE_UNISTD_H */
