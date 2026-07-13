/* toolchain/rt/dirent.h -- POSIX directory-stream wrappers for the Maize C runtime
 * (maize-120), over the sys_getdents64 raw stub.
 *
 * opendir/readdir/closedir decode the frozen linux_dirent64 record layout pinned in
 * docs/design/hostfs.md section 2 (d_ino @0 u64, d_off @8 s64, d_reclen @16 u16,
 * d_type @18 u8, d_name @19 NUL-terminated; records padded to an 8-byte boundary).
 * readdir returns a pointer to a single struct dirent embedded in the DIR, refilled
 * each call, which is POSIX-conformant (the storage is reused between calls).
 */
#ifndef MAIZE_DIRENT_H
#define MAIZE_DIRENT_H

/* Raw getdents64 fill buffer size (bytes). Records are variable-length; this bounds
 * one refill. 1024 mirrors the ls_hostfs.c reference decoder. */
#define DIRENT_BUF 1024

struct dirent {
    unsigned long d_ino;      /* inode number (opaque; decoded from the record) */
    unsigned char d_type;     /* DT_* file-type hint */
    char          d_name[256];/* NUL-terminated entry name */
};

/* The directory stream. The struct dirent slot is overwritten by each readdir; buf
 * holds the current getdents64 page, cursored by bpos within blen valid bytes. */
typedef struct {
    int           fd;
    long          bpos;       /* cursor within buf */
    long          blen;       /* valid bytes in buf */
    struct dirent de;         /* the slot readdir returns a pointer to */
    unsigned char buf[DIRENT_BUF];
} DIR;

DIR           *opendir(const char *path);
struct dirent *readdir(DIR *d);
int            closedir(DIR *d);

#endif /* MAIZE_DIRENT_H */
