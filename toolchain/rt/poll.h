/* toolchain/rt/poll.h -- maize-238 Phase 3 poll() surface.
 *
 * Bit-for-bit the real Linux/glibc struct pollfd + POLL* flag values so an unmodified
 * poll()-using translation unit compiles unchanged. quesOS reports POLLIN/POLLOUT/POLLERR;
 * POLLHUP/POLLNVAL are defined for source compatibility. timeout: negative = block
 * forever, 0 = pure non-blocking poll, positive = milliseconds.
 */
#ifndef _POLL_H
#define _POLL_H

typedef unsigned long nfds_t;

struct pollfd {
    int   fd;
    short events;
    short revents;
};

#define POLLIN   0x0001
#define POLLPRI  0x0002
#define POLLOUT  0x0004
#define POLLERR  0x0008
#define POLLHUP  0x0010
#define POLLNVAL 0x0020

long poll(struct pollfd *fds, nfds_t nfds, int timeout);

#endif /* _POLL_H */
