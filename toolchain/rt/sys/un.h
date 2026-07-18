/* toolchain/rt/sys/un.h -- maize-238 Phase 3 AF_UNIX address shape.
 *
 * The real Linux/glibc struct sockaddr_un: sun_family (sa_family_t) + a 108-byte path.
 * An unmodified Xlib unix-transport connect-by-path fills this and passes it to
 * connect(); the X server fills it for bind(). quesOS uses the NUL-terminated sun_path
 * as an in-kernel namespace key (no hostfs file is created at the bound path).
 */
#ifndef _SYS_UN_H
#define _SYS_UN_H

#include "sys/socket.h"   /* sa_family_t */

struct sockaddr_un {
    sa_family_t sun_family;
    char        sun_path[108];
};

#endif /* _SYS_UN_H */
