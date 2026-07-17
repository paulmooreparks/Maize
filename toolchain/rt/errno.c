/* toolchain/rt/errno.c -- errno storage + the errno-translating wrapper layer
 * over the raw syscall stubs (maize-74).
 *
 * This is the POSIX-flavored convenience layer above toolchain/rt/syscall.mazm.
 * The raw stubs (sys_read / sys_write) return the VM's RV verbatim; a result in
 * [-4095, -1] encodes -errno. The wrappers pass that raw result through
 * __syscall_ret, the battle-tested musl translator (maize-74 decision 7306):
 * a result in that range becomes errno = -result and a -1 return, everything
 * else is a valid result returned unchanged.
 *
 * errno is a plain global int: the Maize VM is single-threaded, so TLS is not
 * needed (maize-74, OQ3); the wrapper signature does not change if errno later
 * becomes thread-local. Note: until maize-75 makes the VM produce real -errno
 * codes, a failing call returns a bare -1, so errno carries the value 1 on error
 * (the translation MECHANISM is correct; the specific code is approximate).
 *
 * Compiled through the same cproc -> normalize -> qbe -t maize -> mazm -c path as
 * the C fixtures and linked into every C image alongside crt0/syscall and the
 * other C runtime modules (string/ctype/stdio/stdlib; cc-maize.sh RT set).
 */
#include "syscall.h"
#include "stdio.h"      /* remove() prototype (maize-148) */
#include "sys/stat.h"   /* mkdir() prototype + mode_t (maize-147/148) */
#include "fcntl.h"      /* O_CREAT (maize-94: variadic open reads mode only when set) */
#include "stdarg.h"     /* va_list for the variadic open (maize-94) */

/* Single source of truth for errno (single-threaded VM; TLS reserved). */
int errno = 0;

/* MAX_ERRNO = 4095; boundary (unsigned long)r > -4096UL == 0xFFFFFFFFFFFFF000. */
long
__syscall_ret(unsigned long r)
{
    if (r > -4096UL) {
        errno = -(long)r;
        return -1;
    }
    return (long)r;
}

long
read(int fd, void *buf, unsigned long count)
{
    return __syscall_ret(sys_read(fd, buf, count));
}

long
write(int fd, const void *buf, unsigned long count)
{
    return __syscall_ret(sys_write(fd, buf, count));
}

/* maize-120 descriptor wrappers: open/close/lseek/fstat, each routed through
 * __syscall_ret like read/write. These sit beside read/write because errno.c is
 * the errno-translating wrapper TU; the FILE* layer (stdio.c) builds on them. */
int
open(const char *path, int flags, ...)
{
    int mode = 0;
    /* maize-94: the POSIX variadic open. The permission mode is meaningful only on a
       create, so read the optional third argument via va_arg only when O_CREAT is set;
       a two-argument open(path, flags) then passes mode 0 harmlessly. */
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
    }
    return (int)__syscall_ret(sys_open(path, flags, mode));
}

int
close(int fd)
{
    return (int)__syscall_ret(sys_close(fd));
}

long
lseek(int fd, long offset, int whence)
{
    return __syscall_ret(sys_lseek(fd, offset, whence));
}

int
fstat(int fd, void *statbuf)
{
    return (int)__syscall_ret(sys_fstat(fd, statbuf));
}

/* path-mutating wrappers over the raw stubs, mirroring the open/close pattern above.
 * maize-151 wires $52/$53/$57 through the confined hostfs backends, so these now produce
 * real filesystem effects and the familiar errno + -1 contract: a create/remove/rename
 * on a writable mount succeeds, and a :ro mount / synthetic root / cross-mount rename
 * sets errno (EROFS / EXDEV) and returns -1. DOOM's save path (mkdir ./.savegame, write
 * temp.dsg, rename to the slot) runs through mkdir()/rename() here. */
int
remove(const char *path)
{
    return (int)__syscall_ret(sys_unlink(path));
}

int
mkdir(const char *path, mode_t mode)
{
    return (int)__syscall_ret(sys_mkdir(path, (int)mode));
}

int
rename(const char *old, const char *new)
{
    return (int)__syscall_ret(sys_rename(old, new));
}

/* maize-94: path-based stat as a composite over open + fstat + close. Maize's native ABI
 * has no path-stat syscall (SYS $04 is out of scope), and sbase (pwd, cp, ls) reaches for
 * stat(path, &st). Open the path, fstat the descriptor, close. A directory is not always
 * O_RDONLY-openable through the hostfs backend, so fall back to O_DIRECTORY; that pair
 * covers regular files and directories, which is the wave-1 need. A native path-stat is
 * deferred to the filesystem card (maize-22). */
int
stat(const char *path, struct stat *st)
{
    int fd = open(path, O_RDONLY, 0);
    int r;
    if (fd < 0) {
        fd = open(path, O_DIRECTORY, 0);   /* a directory may reject O_RDONLY */
        if (fd < 0) {
            return -1;                     /* errno set by open */
        }
    }
    r = fstat(fd, st);
    close(fd);
    return r;
}

/* lstat aliases stat: hostfs models no symbolic links, so there is no link-vs-target
 * distinction to preserve. Declared for source compatibility with sbase's cp/ls. */
int
lstat(const char *path, struct stat *st)
{
    return stat(path, st);
}
