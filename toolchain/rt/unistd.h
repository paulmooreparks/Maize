/* toolchain/rt/unistd.h -- freestanding <unistd.h> slice for the Maize C runtime
 * (maize-147).
 *
 * DOOM's i_timer.c / i_system.c call usleep; strict cproc needs a visible declaration
 * at each call site. This header DECLARES only usleep; the body lives in the sibling
 * libc card (maize-148). Return int, arg unsigned (avoids needing useconds_t). The
 * read/write/close/lseek descriptor wrappers already live in syscall.h and MUST NOT be
 * re-declared here (a duplicate typedef-free redeclaration is legal but noise; keep the
 * ownership where it is).
 */
#ifndef MAIZE_UNISTD_H
#define MAIZE_UNISTD_H

int usleep(unsigned useconds);

#endif /* MAIZE_UNISTD_H */
