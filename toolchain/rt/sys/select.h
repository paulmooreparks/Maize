/* toolchain/rt/sys/select.h -- maize-238 Phase 3 select() surface.
 *
 * A full 1024-bit fd_set (128 bytes), bit-for-bit the real Linux/glibc/musl layout, even
 * though quesOS's QUESOS_MAX_FD is only 16: this is what makes an unmodified FD_SET /
 * FD_ISSET-using translation unit (Xlib) compile and run without modification. quesOS only
 * ever inspects bits [0, QUESOS_MAX_FD). exceptfds is accepted but never reports ready
 * (quesOS models no exceptional/OOB condition). timeout: NULL = block forever, {0,0} =
 * non-blocking, else a bounded wait.
 */
#ifndef _SYS_SELECT_H
#define _SYS_SELECT_H

#include "sys/time.h"   /* struct timeval */

#define FD_SETSIZE 1024
#define __NFDBITS  (8 * (int)sizeof(unsigned long))

typedef struct {
    unsigned long fds_bits[FD_SETSIZE / (8 * sizeof(unsigned long))];
} fd_set;

#define FD_ZERO(s) do {                                                     \
        unsigned long __i;                                                  \
        for (__i = 0; __i < sizeof((s)->fds_bits) / sizeof((s)->fds_bits[0]); ++__i) { \
            (s)->fds_bits[__i] = 0;                                         \
        }                                                                   \
    } while (0)
#define FD_SET(fd, s)   ((s)->fds_bits[(fd) / __NFDBITS] |= (1UL << ((fd) % __NFDBITS)))
#define FD_CLR(fd, s)   ((s)->fds_bits[(fd) / __NFDBITS] &= ~(1UL << ((fd) % __NFDBITS)))
#define FD_ISSET(fd, s) (((s)->fds_bits[(fd) / __NFDBITS] >> ((fd) % __NFDBITS)) & 1UL)

long select(int nfds, fd_set *r, fd_set *w, fd_set *e, struct timeval *timeout);

#endif /* _SYS_SELECT_H */
