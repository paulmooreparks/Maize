/* toolchain/rt/sys/stat.h -- freestanding <sys/stat.h> for the Maize C runtime
 * (maize-147).
 *
 * struct stat is BYTE-ABI, pinned field-for-field to the x86-64 kernel layout in
 * docs/design/hostfs.md section 2 (144 bytes, all fields naturally aligned,
 * little-endian), so fstat(fd, &st) (the wrapper in syscall.h, which takes
 * void *statbuf) fills it correctly. Note the x86-64 quirk that st_nlink (offset 16)
 * precedes st_mode (offset 24, u32); st_size is at offset 48 (s64). ctest/stat_hostfs.c
 * independently reads those same offsets. syscall.h does NOT define struct stat, so
 * defining it here raises no redefinition conflict.
 *
 * mkdir is DECLARED here (maps to SYS $53 in the sibling libc card, maize-148; see
 * hostfs.md section 5); the body is maize-148. mode_t comes from sys/types.h. Only
 * mkdir is declared: no path-based stat(path, &st) wrapper (out of scope, would need
 * a new raw stub not present in maize-148's list).
 */
#ifndef MAIZE_SYS_STAT_H
#define MAIZE_SYS_STAT_H

#include "sys/types.h"

struct stat {
    unsigned long st_dev;         /* 0   */
    unsigned long st_ino;         /* 8   */
    unsigned long st_nlink;       /* 16  */
    unsigned int  st_mode;        /* 24  */
    unsigned int  st_uid;         /* 28  */
    unsigned int  st_gid;         /* 32  */
    unsigned int  __pad0;         /* 36  */
    unsigned long st_rdev;        /* 40  */
    long          st_size;        /* 48  */
    long          st_blksize;     /* 56  */
    long          st_blocks;      /* 64  */
    unsigned long st_atime;       /* 72  */
    unsigned long st_atime_nsec;  /* 80  */
    unsigned long st_mtime;       /* 88  */
    unsigned long st_mtime_nsec;  /* 96  */
    unsigned long st_ctime;       /* 104 */
    unsigned long st_ctime_nsec;  /* 112 */
    long          __unused[3];    /* 120..143 */
};                                /* sizeof == 144 */

/* File-type bits (octal, per hostfs.md section 2). */
#define S_IFMT   0170000
#define S_IFSOCK 0140000
#define S_IFLNK  0120000
#define S_IFREG  0100000
#define S_IFBLK  0060000
#define S_IFDIR  0040000
#define S_IFCHR  0020000
#define S_IFIFO  0010000
#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
/* maize-94: the remaining POSIX file-type predicates, over the S_IF* constants
 * already defined above. Borrowed sbase (ls's type detection, cp's device / symlink
 * guards) tests these by name. On hostfs, which models only regular files and
 * directories, S_ISLNK/S_ISBLK/S_ISCHR/S_ISFIFO/S_ISSOCK are simply always false;
 * the macros are still the honest way to spell those tests. */
#define S_ISLNK(m)  (((m) & S_IFMT) == S_IFLNK)
#define S_ISCHR(m)  (((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m)  (((m) & S_IFMT) == S_IFBLK)
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)
#define S_ISSOCK(m) (((m) & S_IFMT) == S_IFSOCK)

int mkdir(const char *pathname, mode_t mode);

/* maize-94: path-based stat as a libc COMPOSITE over open + fstat + close (the native ABI
 * has no path-stat syscall; SYS $04 is out of scope). Fills *st for a path, returning 0 or
 * -1 with errno. Limitation (recorded in the card decision): the path must be openable
 * (O_RDONLY, with an O_DIRECTORY fallback for directories); a real native path-stat is
 * deferred to the filesystem card (maize-22). lstat aliases stat: hostfs models no
 * symlinks, so there is no link-vs-target distinction to preserve (honest deviation). */
int stat(const char *path, struct stat *st);
int lstat(const char *path, struct stat *st);

#endif /* MAIZE_SYS_STAT_H */
