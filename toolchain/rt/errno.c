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
open(const char *path, int flags, int mode)
{
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

/* maize-148/152 path-mutating wrappers over the raw stubs, mirroring the open/close
 * pattern above. LINK-COMPLETE only: the VM does not dispatch $52/$53/$57 yet, so the
 * raw stubs return 0 (interim no-op) and these return 0 (apparent success, no
 * filesystem effect), which is benign for DOOM Phase A's link + boot. The real
 * -errno-producing dispatch + confined hostfs backends are the spawned card
 * (maize-151), where the checked filesystem acceptance criteria live. */
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
