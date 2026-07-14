/* hostfs.h: the freestanding-C99 VFS core boundary (docs/design/hostfs.md, maize-114).
 *
 * This header is the seam every hostfs provider consumes: the native C++ VM
 * (src/sys.cpp), the reference C VM (maize-87), and future firmware/quesito
 * providers. Per doc section 6 it is freestanding C99 with NO VM header and NO
 * host header: only fixed-width integer types (via <stdint.h>) and the three
 * declared surfaces appear. There is no <windows.h>, no <fcntl.h>, no maize.h.
 *
 * The three declared surfaces (doc section 6):
 *   (a) the hostfs_backend_ops struct of function pointers (with the confine hook),
 *   (b) the hostfs_mount / hostfs_table shapes and the hostfs_mode enum,
 *   (c) the Linux-numbered HOSTFS_E* errno constants.
 *
 * Plus the two byte-image typedefs the ops reference (hostfs_stat, hostfs_dirent),
 * which the section-6 code block itself names.
 *
 * Width discipline (doc section 6, load-bearing): every op returns int64_t (a
 * non-negative result, or a value in [-4095, -1] encoding -errno); sizes and
 * counts are uint64_t. The native Windows consumer is LLP64, where `long` is
 * 32-bit, so a `long` return would truncate the sign-extended -errno band, cap
 * lseek offsets at 2 GiB, and clamp read/write counts. The fixed-width types are
 * deliberate.
 */
#ifndef MAIZE_HOSTFS_H
#define MAIZE_HOSTFS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- forward typedefs (referenced by the ops struct below) ------------------ */

typedef struct hostfs_mount  hostfs_mount;
typedef struct hostfs_stat   hostfs_stat;   /* the section-2 struct stat image */
typedef struct hostfs_dirent hostfs_dirent; /* the section-2 linux_dirent64 image */

/* The neutral field image of the section-2 x86-64 `struct stat`. The backend
   fills these fields from its host stat; the core composes the exact 144-byte
   little-endian on-guest image from them (it is NOT memcpy'd from a host struct,
   whose layout differs on Windows/macOS). */
struct hostfs_stat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint64_t st_nlink;
    uint32_t st_mode;      /* S_IF* type bits OR'd with permission bits */
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_rdev;
    int64_t  st_size;
    int64_t  st_blksize;
    int64_t  st_blocks;
    int64_t  st_atime_sec;
    int64_t  st_atime_nsec;
    int64_t  st_mtime_sec;
    int64_t  st_mtime_nsec;
    int64_t  st_ctime_sec;
    int64_t  st_ctime_nsec;
};

/* The neutral field image of one section-2 `linux_dirent64` entry. The backend
   (or the core's synthetic-root enumerator) produces these; the core packs them
   into the exact on-guest byte layout with 8-byte-rounded records. */
struct hostfs_dirent {
    uint64_t d_ino;
    uint8_t  d_type;       /* DT_* values, doc section 2 */
    const char *d_name;    /* NUL-terminated */
};

/* --- (a) backend operations struct (doc section 6a) ------------------------- */

typedef struct hostfs_backend_ops {
    /* Resolve/confine: prove guest_path stays beneath mount->host_root and hand
       back an opaque backend handle, or a negative errno. This is the one place
       openat2(RESOLVE_BENEATH) / GetFinalPathNameByHandle live (backend only).
       The core calls it but never implements it. */
    int64_t (*confine)(hostfs_mount *mount, const char *guest_path,
                       int flags, void **out_handle);

    int64_t (*open) (hostfs_mount *mount, const char *path, int flags, int mode);
    int64_t (*close)(void *handle);
    int64_t (*read) (void *handle, void *buf, uint64_t count);
    int64_t (*write)(void *handle, const void *buf, uint64_t count);
    int64_t (*lseek)(void *handle, int64_t offset, int whence);
    int64_t (*stat) (hostfs_mount *mount, const char *path, hostfs_stat *out);
    int64_t (*fstat)(void *handle, hostfs_stat *out);
    int64_t (*getdents)(void *handle, void *buf, uint64_t count);
    int64_t (*mkdir) (hostfs_mount *mount, const char *path, int mode);
    int64_t (*rmdir) (hostfs_mount *mount, const char *path);
    int64_t (*unlink)(hostfs_mount *mount, const char *path);
    int64_t (*rename)(hostfs_mount *mount, const char *oldp, const char *newp);
    /* maize-179: fd-based truncate. Sets the open file's size to exactly `length`
       (POSIX ftruncate: a shrink drops the tail, an extend zero-fills; the file offset
       is unchanged). The core applies the :ro / synthetic-root write-gate and the
       negative-length check before dispatch, so the backend only performs the host
       resize on its stored handle. */
    int64_t (*ftruncate)(void *handle, int64_t length);
} hostfs_backend_ops;

/* --- (b) mount table shape (doc section 6b) --------------------------------- */

typedef enum { HOSTFS_RO = 0, HOSTFS_RW = 1 } hostfs_mode;

struct hostfs_mount {
    const char  *guest_prefix;   /* nix absolute, e.g. "/proj" */
    const char  *host_root;      /* native host path, opaque to the core */
    hostfs_mode  mode;           /* ro / rw; write-intent on RO -> EROFS */
    void        *anchor;         /* backend-owned root handle (anchor fd / HANDLE) */
};

typedef struct hostfs_table {
    hostfs_mount             *mounts;   /* longest-prefix match */
    unsigned                  count;
    const hostfs_backend_ops *ops;      /* the active backend */
    const char               *cwd;      /* maize-132: base for relative-path
                                           resolution (a *nix absolute path). NULL
                                           or empty is treated as "/". The core
                                           normalizes each guest path against this
                                           before selecting a mount; the backend's
                                           host-side confinement is unchanged. */
} hostfs_table;

/* --- (c) Linux-numbered errno constants (doc section 6c) -------------------- */

#define HOSTFS_EPERM      1
#define HOSTFS_ENOENT     2
#define HOSTFS_EIO        5
#define HOSTFS_EBADF      9
#define HOSTFS_ENOMEM    12
#define HOSTFS_EACCES    13
#define HOSTFS_EEXIST    17
#define HOSTFS_EXDEV     18
#define HOSTFS_ENOTDIR   20
#define HOSTFS_EISDIR    21
#define HOSTFS_EINVAL    22
#define HOSTFS_EROFS     30
#define HOSTFS_ENAMETOOLONG 36
#define HOSTFS_ENOSYS    38
#define HOSTFS_ENOTEMPTY 39
#define HOSTFS_ELOOP     40

#ifdef __cplusplus
}
#endif

#endif /* MAIZE_HOSTFS_H */
