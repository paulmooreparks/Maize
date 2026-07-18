/* toolchain/rt/sys/socket.h -- maize-238 Phase 3 AF_UNIX socket surface.
 *
 * Bit-for-bit the real Linux/glibc shapes so unmodified X11 client sources (the
 * borrow-everything doctrine, doc 12) compile against them without modification.
 * quesOS implements AF_UNIX SOCK_STREAM only (the X11-transport shape); other domains,
 * types, and the datagram/option-setting surface (sendto/recvmsg/setsockopt/...) are
 * out of scope (maize-90 scoping: no named consumer). Socket data I/O goes through the
 * ordinary read()/write()/close() already provided by unistd.h.
 */
#ifndef _SYS_SOCKET_H
#define _SYS_SOCKET_H

typedef unsigned short sa_family_t;
typedef unsigned int   socklen_t;

#define AF_UNIX     1
#define AF_LOCAL    1
#define SOCK_STREAM 1

struct sockaddr {
    sa_family_t sa_family;
    char        sa_data[14];
};

long socket(int domain, int type, int protocol);
long bind(int fd, const struct sockaddr *addr, socklen_t addrlen);
long connect(int fd, const struct sockaddr *addr, socklen_t addrlen);
long listen(int fd, int backlog);
long accept(int fd, struct sockaddr *addr, socklen_t *addrlen);
long socketpair(int domain, int type, int protocol, int sv[2]);

#endif /* _SYS_SOCKET_H */
