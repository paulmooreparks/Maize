/* toolchain/rt/syscall.h -- C binding for Maize syscalls (maize-74).
 *
 * Two layers, per the recorded convention (toolchain/rt/SYSCALL-ABI.md):
 *
 *   raw stubs   sys_read / sys_write / _exit  (toolchain/rt/syscall.mazm)
 *       Each is `SYS <number>; RET`. Args are the C ABI arg registers R0..R2;
 *       the result is whatever the VM leaves in RV, returned verbatim with NO
 *       error interpretation. A result in [-4095, -1] encodes -errno.
 *
 *   wrappers    read / write                  (toolchain/rt/errno.c)
 *       POSIX-named; each calls the matching raw stub and passes the result
 *       through __syscall_ret, which turns a [-4095, -1] result into
 *       errno = -result and a -1 return, and returns any other value verbatim.
 *
 * The hosted syscall numbers are frozen ABI (mirroring Linux x86-64):
 *   read = 0x00, write = 0x01, exit = 0x3C  (reboot = 0xA9 reserved).
 *
 * This header is preprocessed by the system cpp before cproc-qbe (run-ctest.sh),
 * so ordinary include guards and object-like macros are available.
 */
#ifndef MAIZE_SYSCALL_H
#define MAIZE_SYSCALL_H

/* The E* table and `extern int errno` live in errno.h now (maize-76 decision
 * 7348); syscall.h pulls them in so the wrappers below still see errno. */
#include "errno.h"

/* --- raw stubs (syscall.mazm) ---------------------------------------------- */
long sys_read(int fd, void *buf, unsigned long count);
long sys_write(int fd, const void *buf, unsigned long count);
_Noreturn void _exit(int code);

/* maize-114 hostfs raw stubs (SYS $02/$03/$05/$08/$D9). Each returns RV verbatim: a
 * non-negative result, or a value in [-4095, -1] encoding -errno (test the band with
 * `(unsigned long)r > -4096UL`). The POSIX-named, errno-translating wrappers are
 * deferred to the libc-growth line; the acceptance fixtures call these raw stubs. */
long sys_open(const char *path, int flags, int mode);
long sys_close(int fd);
long sys_fstat(int fd, void *statbuf);
long sys_lseek(int fd, long offset, int whence);
long sys_getdents64(int fd, void *dirp, unsigned long count);

/* sys_brk (SYS $0C, maize-75): R0 = requested break (0 queries); returns the
 * new-or-current break in RV, NEVER -errno. sbrk (stdlib.c) wraps this. */
void *sys_brk(void *addr);

/* sys_clock_ms (SYS $F0, maize-141): monotonic milliseconds since VM start.
 * Returns the count in RV; NEVER -errno (exempt from the errno convention,
 * cf. sys_brk). Monotonic non-decreasing; epoch arbitrary. */
unsigned long sys_clock_ms(void);

/* maize-140 termios raw stubs (SYS $F1 tcgetattr / $F2 tcsetattr). Each returns 0 or a
 * [-4095, -1] -errno (-EBADF when no window console is bound). The POSIX-named wrappers
 * tcgetattr()/tcsetattr() live in termios.c. `termios_p` is a struct termios* (termios.h),
 * declared void* here so syscall.h stays free of the termios type. */
long sys_tcgetattr(int fd, void *termios_p);
long sys_tcsetattr(int fd, int optional_actions, void *termios_p);
long sys_ttysize(int fd, void *winsize_p);   /* maize-228: TIOCGWINSZ over the real terminal */

/* path-mutating raw stubs (SYS $57 unlink / $53 mkdir / $52 rename). maize-151 wires
 * these through the confined hostfs backends, so each returns RV verbatim as a real
 * non-negative result or a [-4095, -1] -errno (EROFS on a :ro mount or the synthetic
 * root, EXDEV on a cross-mount rename). remove()/mkdir()/rename() (errno.c) wrap these. */
long sys_unlink(const char *path);
long sys_mkdir(const char *path, int mode);
long sys_rename(const char *old, const char *new);

/* maize-179 ftruncate raw stub (SYS $4D). Routed through the confined hostfs backend:
 * RV = 0 on success, or a [-4095, -1] -errno (EROFS on a :ro mount / synthetic root,
 * EINVAL on a negative length, EBADF on a bad fd). ftruncate() (unistd.c) wraps this. */
long sys_ftruncate(int fd, long length);

/* sys_palette_blit (SYS $F3, maize-213): a pure indexed 32-bit palette blit
 * dst[i] = lut[src[i]] for i in [0, npixels), performed by the VM at host memcpy
 * speed. dst is an XRGB8888 buffer, src an 8bpp index buffer, lut a baked
 * uint32[256] XRGB8888 LUT. Maize-private high-block number with no Linux mirror
 * (like sys_clock_ms). Returns npixels on success, or a [-4095, -1] -errno on a
 * bounds violation (npixels > 2^24, or a dst/src/lut base+len that wraps the
 * 64-bit space); on rejection it performs NO guest write. A graphics primitive,
 * not a file op, so there is no POSIX/errno wrapper: the caller ignores the
 * return (or may assert it equals npixels). */
long sys_palette_blit(void *dst, const void *src, const unsigned int *lut, unsigned long npixels);

/* sys_bulk_copy (SYS $F4, maize-216): copy n bytes src -> dst over guest memory at
 * host memcpy speed. memmove-safe: the VM reads the whole src range into a host
 * buffer before writing dst, so overlapping ranges are correct in both directions
 * (memcpy and memmove both route here). sys_bulk_set (SYS $F5): fill n bytes at dst
 * with c's low byte (memset routes here). Maize-private high-block numbers with no
 * Linux mirror (like sys_clock_ms / sys_palette_blit). Each returns n on success, or
 * a [-4095, -1] -errno on a bounds violation (n > 2^28, or a dst/src base+len that
 * wraps the 64-bit space); on rejection it performs NO guest write. Memory
 * primitives, not file ops, so there is no POSIX/errno wrapper: the caller ignores
 * the return (or may assert it equals n). string.c calls these for large copies/sets
 * only, above BULK_SYSCALL_THRESHOLD. */
long sys_bulk_copy(void *dst, const void *src, unsigned long n);
long sys_bulk_set(void *dst, int c, unsigned long n);

/* maize-174 guest signal subsystem raw stubs (SYS $0D/$0E/$0F/$3E/$6D/$79/$FA/$FB).
 * Guest-only (quesOS's cause-7 dispatcher implements them; a bare-VM caller hits the
 * native table). Each returns RV verbatim. signal.h wraps rt_sigaction/rt_sigprocmask;
 * the unistd wrappers wrap kill/setpgid/getpgid/tcgetpgrp/tcsetpgrp. */
long sys_kill(long pid, long sig);
long sys_rt_sigaction(long sig, const void *act, void *oldact);
long sys_rt_sigprocmask(long how, const void *set, void *oldset);
long sys_rt_sigreturn(void);
long sys_setpgid(long pid, long pgid);
long sys_getpgid(long pid);
long sys_tcgetpgrp(void);
long sys_tcsetpgrp(long pgid);

/* maize-93 process-control raw stubs (SYS $39/$3D/$27/$3B/$16/$21/$20). These have bodies
 * in syscall.mazm but were previously undeclared; maize-94 declares them (decision 8943)
 * so the unistd.c POSIX wrappers (fork/execve/execvp/wait/waitpid/pipe/dup/dup2/getpid)
 * compile against them. Guest-only: dispatched by quesOS, never the native table. Each
 * returns RV verbatim (a [-4095,-1] result is -errno). */
long sys_fork(void);
long sys_wait4(long pid, void *status, long options, void *rusage);
long sys_getpid(void);
long sys_execve(const char *path, char *const argv[], char *const envp[]);
long sys_pipe(int fds[2]);
long sys_dup2(long oldfd, long newfd);
long sys_dup(long oldfd);

/* maize-94 per-process working directory raw stubs (SYS $50 chdir / $4F getcwd). NEW
 * quesOS-guest-only calls (decision 8940): a user process's SYS traps to quesOS, which
 * owns the per-PCB cwd. chdir returns 0 or -errno; getcwd returns the byte length
 * (including the NUL) written to buf, or -ERANGE when size is too small. */
long sys_chdir(const char *path);
long sys_getcwd(char *buf, unsigned long size);

/* --- errno + wrappers (errno.c) -------------------------------------------- */

/* The musl error translator: a pure function of its input. A raw result in
 * [-4095, -1] (i.e. (unsigned long)r > -4096UL) is -errno; it sets errno and
 * returns -1. Any other value is a valid result, returned verbatim. */
long __syscall_ret(unsigned long r);

long read(int fd, void *buf, unsigned long count);
long write(int fd, const void *buf, unsigned long count);

/* maize-120: the POSIX-named, errno-translating descriptor wrappers over the
 * hostfs raw stubs. Each calls the matching sys_* stub and passes the result
 * through __syscall_ret exactly as read/write do. `mode` is open()'s O_CREAT
 * permission arg (ignored unless O_CREAT). Directory enumeration lives in the
 * dirent.c wrappers (opendir/readdir/closedir) over sys_getdents64. */
int  open (const char *path, int flags, ...);   /* maize-94: variadic (POSIX 2-arg form) */
int  close(int fd);
long lseek(int fd, long offset, int whence);
int  fstat(int fd, void *statbuf);

#endif /* MAIZE_SYSCALL_H */
