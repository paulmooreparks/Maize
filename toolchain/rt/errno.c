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
 * the C fixtures and linked into every C image alongside crt0/syscall/puts
 * (scripts/run-ctest.sh RT_OBJS).
 */
#include "syscall.h"

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
