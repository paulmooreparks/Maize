/* toolchain/rt/sys/types.h -- freestanding <sys/types.h> alias slice for the Maize
 * C runtime (maize-147).
 *
 * cpp -nostdinc -I toolchain/rt resolves #include <sys/types.h> to this file (cpp
 * joins each -I dir with the bracketed subpath), so the sys/ subdir lives under
 * toolchain/rt. DOOM's i_video.c / m_misc.c reference off_t/ssize_t; mode_t backs
 * st_mode and mkdir's perm arg. LP64 widths (long = 64, int = 32), consistent with
 * stddef.h / stdint.h (maize-146). No existing RT header defines these (verified
 * across toolchain/rt). The alias set is kept minimal: struct stat (sys/stat.h) uses
 * concrete widths rather than the dev_t/ino_t soup.
 */
#ifndef MAIZE_SYS_TYPES_H
#define MAIZE_SYS_TYPES_H

/* maize-94: POSIX requires <sys/types.h> to provide size_t; borrowed oksh's unvis.c
 * reaches for it through this header without including <stddef.h>. Identical to
 * stddef.h's typedef (unsigned long), so a TU including both sees a legal, cproc-
 * tolerated duplicate. */
typedef unsigned long size_t;

typedef long          off_t;    /* 64-bit file offset (LP64) */
typedef long          ssize_t;  /* signed size */
typedef unsigned int  mode_t;   /* st_mode / mkdir perm bits */

/* maize-94: dev_t / ino_t match struct stat's st_dev / st_ino widths (sys/stat.h,
 * both unsigned long). Borrowed sbase (fs.h's struct history, the cp/mv/rm
 * same-file guard) references them by name; they are ordinary POSIX aliases, not a
 * new ABI. */
typedef unsigned long dev_t;    /* device id (st_dev) */
typedef unsigned long ino_t;    /* inode number (st_ino) */

/* maize-94: the POSIX id triple borrowed oksh reaches for through <sys/types.h>
 * (struct passwd/group, waitpid, and pid-typed shell bookkeeping). pid_t matches
 * unistd.h / sys/wait.h's existing `typedef int pid_t` exactly, so a TU including
 * both sees an identical (legal, cproc-tolerated) redefinition rather than a clash.
 * uid_t / gid_t are new here (defined nowhere else in RT). */
typedef int           pid_t;    /* process id (matches unistd.h / sys/wait.h) */
typedef unsigned int  uid_t;    /* user id */
typedef unsigned int  gid_t;    /* group id */

/* maize-94: the BSD short-name integer aliases borrowed oksh reaches for (vis.c's
 * u_int, and the sys-queue.h macros). Ordinary width aliases, not a new ABI. */
typedef unsigned char  u_char;
typedef unsigned short u_short;
typedef unsigned int   u_int;
typedef unsigned long  u_long;

#endif /* MAIZE_SYS_TYPES_H */
