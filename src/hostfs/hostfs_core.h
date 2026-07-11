/* hostfs_core.h: the internal API the native provider (src/sys.cpp) and the two
 * backends call. This is deliberately SEPARATE from hostfs.h: hostfs.h stays the
 * pristine freestanding-C99 boundary (doc section 6, exactly three surfaces),
 * while this header carries the guest-visible dispatch entry points, the shared
 * byte-ABI encode helpers, and the backend-acquisition hooks.
 *
 * Everything here is still portable C (no host or VM headers); the per-host
 * openat2 / GetFinalPathNameByHandle machinery lives only in hostfs_posix.c /
 * hostfs_win32.c behind the ops struct.
 */
#ifndef MAIZE_HOSTFS_CORE_H
#define MAIZE_HOSTFS_CORE_H

#include <stdint.h>
#include "hostfs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bound on a copied-in guest path (maize-79 trust-boundary discipline). A path
   at or past this bound returns -ENAMETOOLONG. */
#define HOSTFS_PATH_MAX 4096

/* The section-2 struct stat image size (bytes). */
#define HOSTFS_STAT_SIZE 144

/* section-2 open-flag bits (x86-64 fixed values). */
#define HOSTFS_O_ACCMODE   0x3
#define HOSTFS_O_RDONLY    0x0
#define HOSTFS_O_WRONLY    0x1
#define HOSTFS_O_RDWR      0x2
#define HOSTFS_O_CREAT     0x40
#define HOSTFS_O_EXCL      0x80
#define HOSTFS_O_TRUNC     0x200
#define HOSTFS_O_APPEND    0x400
#define HOSTFS_O_DIRECTORY 0x10000
#define HOSTFS_O_NOFOLLOW  0x20000
#define HOSTFS_O_CLOEXEC   0x100000

/* section-2 lseek whence values. */
#define HOSTFS_SEEK_SET 0
#define HOSTFS_SEEK_CUR 1
#define HOSTFS_SEEK_END 2

/* section-2 st_mode type bits (octal in the doc; hex here). */
#define HOSTFS_S_IFMT   0xF000
#define HOSTFS_S_IFSOCK 0xC000
#define HOSTFS_S_IFLNK  0xA000
#define HOSTFS_S_IFREG  0x8000
#define HOSTFS_S_IFBLK  0x6000
#define HOSTFS_S_IFDIR  0x4000
#define HOSTFS_S_IFCHR  0x2000
#define HOSTFS_S_IFIFO  0x1000

/* section-2 d_type values. */
#define HOSTFS_DT_UNKNOWN 0
#define HOSTFS_DT_FIFO    1
#define HOSTFS_DT_CHR     2
#define HOSTFS_DT_DIR     4
#define HOSTFS_DT_BLK     6
#define HOSTFS_DT_REG     8
#define HOSTFS_DT_LNK     10
#define HOSTFS_DT_SOCK    12

/* Fixed synthetic inode for the root "/" (doc section 3). */
#define HOSTFS_ROOT_INO 1

/* --- guest-visible dispatch (called by src/sys.cpp) ------------------------- */
/* Each returns a non-negative result or a value in [-4095, -1] encoding -errno,
   ready to hand straight back to RV. `path` is a copied-in host C string; buffers
   are host buffers the provider copies to/from guest memory. */

int64_t hostfs_open(hostfs_table *t, const char *path, int flags, int mode);
int64_t hostfs_close(hostfs_table *t, int fd);
int64_t hostfs_read(hostfs_table *t, int fd, void *buf, uint64_t count);
int64_t hostfs_write(hostfs_table *t, int fd, const void *buf, uint64_t count);
int64_t hostfs_lseek(hostfs_table *t, int fd, int64_t offset, int whence);
/* Composes the 144-byte struct stat image into out_stat (HOSTFS_STAT_SIZE). */
int64_t hostfs_fstat(hostfs_table *t, int fd, uint8_t *out_stat);
/* Packs linux_dirent64 records into buf (capacity `count`). */
int64_t hostfs_getdents(hostfs_table *t, int fd, uint8_t *buf, uint64_t count);

/* Reset the whole guest fd table (called at provider init so a reused process
   image starts clean). */
void hostfs_reset_fds(void);

/* --- shared byte-ABI helpers (core + both backends) ------------------------- */

/* Compose the exact 144-byte little-endian section-2 struct stat image from the
   neutral field struct. Writes at the exact section-2 offsets; never memcpy's a
   host struct. */
void hostfs_encode_stat(const hostfs_stat *in, uint8_t out[HOSTFS_STAT_SIZE]);

/* The 8-byte-rounded on-guest record length for a NUL-terminated name of
   name_len bytes (excluding the NUL). */
uint64_t hostfs_dirent_reclen(uint64_t name_len);

/* Emit one linux_dirent64 record into buf[*poff .. bufcap). On success advances
   *poff by the rounded record length and returns 1; returns 0 (writing nothing)
   when the record does not fit. cookie is the opaque d_off resume value. */
int hostfs_emit_dirent(uint8_t *buf, uint64_t bufcap, uint64_t *poff,
                       uint64_t ino, int64_t cookie, uint8_t d_type,
                       const char *name);

/* True if `flags` carry write intent (O_WRONLY/O_RDWR/O_CREAT/O_TRUNC). O_APPEND
   alone is NOT write intent (doc section 2). */
int hostfs_flags_write_intent(int flags);

/* --- backend acquisition (defined by the active per-host backend) ----------- */

/* The active backend's ops table (POSIX or Win32, selected at build time). */
const hostfs_backend_ops *hostfs_backend_ops_get(void);

/* Open the per-mount anchor (O_PATH/O_DIRECTORY fd on Linux, a directory HANDLE
   on Windows) into mount->anchor. Returns 0 on success or a negative errno.
   Called once per mount at startup; a failure fails startup closed. On Linux this
   is also where the openat2/RESOLVE_BENEATH kernel-floor (>= 5.6) is enforced. */
int64_t hostfs_backend_anchor_open(hostfs_mount *mount);

#ifdef __cplusplus
}
#endif

#endif /* MAIZE_HOSTFS_CORE_H */
