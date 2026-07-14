/* hostfs_posix.c: the Linux backend behind hostfs_backend_ops (doc sections 4, 5).
 *
 * Confinement lives ENTIRELY here (comment 2226, binding): the mount root is an
 * O_PATH|O_DIRECTORY anchor fd opened once at mount time, and every guest open is
 * an openat2(anchor, rel, &how{.resolve=RESOLVE_BENEATH}) so a `..` or symlink that
 * would escape fails in the kernel, not in a racy userspace check. openat2 is
 * hard-required (kernel >= 5.6, operator ruling OQ 7793): there is no realpath
 * fallback. The kernel's escape errnos EXDEV (18) and ELOOP (40) are translated to
 * -EACCES (13) so the guest sees the single escape code the errno contract promises.
 *
 * Whole file compiles to nothing off Linux (guarded), so both backends can be added
 * to both build targets unconditionally.
 */
#ifdef __linux__

#define _GNU_SOURCE
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>          /* renameat */
#include <sys/stat.h>
#include <sys/syscall.h>

#include "hostfs_core.h"

#ifndef SYS_openat2
#define SYS_openat2 437
#endif
#ifndef SYS_getdents64
#define SYS_getdents64 217
#endif

#define HOSTFS_RESOLVE_BENEATH 0x08

/* struct open_how, defined locally because glibc may lack a wrapper/definition. */
struct hostfs_open_how {
    uint64_t flags;
    uint64_t mode;
    uint64_t resolve;
};

/* A snapshot directory entry (name owned by the handle). */
typedef struct {
    char    *name;
    uint64_t ino;
    uint8_t  type;
} posix_dent;

typedef struct {
    int         fd;        /* the confined open fd */
    int         is_dir;
    posix_dent *dents;     /* directory snapshot (decision 7848) */
    uint64_t    ndents;
    uint64_t    cursor;    /* next snapshot index to page out */
} posix_handle;

/* Host Linux errno is numerically identical to the Maize/ABI errno, so a plain
   negation is correct; only the escape errnos need remapping to EACCES. */
static int64_t map_errno(int e) {
    if (e == EXDEV || e == ELOOP) {
        return -(int64_t)HOSTFS_EACCES;
    }
    return -(int64_t)e;
}

/* Read every directory entry off fd via the raw getdents64 syscall and snapshot
   it into h->dents, skipping "." and "..". Returns 0 or a negative errno. */
static int64_t snapshot_dir(posix_handle *h) {
    char buf[32768];
    uint64_t cap = 0;
    for (;;) {
        long n = syscall(SYS_getdents64, h->fd, buf, sizeof(buf));
        if (n < 0) {
            return map_errno(errno);
        }
        if (n == 0) {
            break;
        }
        long pos = 0;
        while (pos < n) {
            uint8_t *rec = (uint8_t *)buf + pos;
            uint64_t d_ino;
            uint16_t d_reclen;
            uint8_t  d_type;
            memcpy(&d_ino, rec + 0, 8);
            memcpy(&d_reclen, rec + 16, 2);
            d_type = rec[18];
            const char *name = (const char *)(rec + 19);
            if (strcmp(name, ".") != 0 && strcmp(name, "..") != 0) {
                if (h->ndents >= cap) {
                    uint64_t ncap = cap ? cap * 2 : 16;
                    posix_dent *nd = (posix_dent *)realloc(h->dents,
                                        (size_t)ncap * sizeof(posix_dent));
                    if (!nd) {
                        return -(int64_t)HOSTFS_ENOMEM;
                    }
                    h->dents = nd;
                    cap = ncap;
                }
                size_t nlen = strlen(name);
                char *copy = (char *)malloc(nlen + 1);
                if (!copy) {
                    return -(int64_t)HOSTFS_ENOMEM;
                }
                memcpy(copy, name, nlen + 1);
                h->dents[h->ndents].name = copy;
                h->dents[h->ndents].ino = d_ino;
                h->dents[h->ndents].type = d_type;
                ++h->ndents;
            }
            pos += d_reclen;
        }
    }
    return 0;
}

/* --- ops -------------------------------------------------------------------- */

static int64_t posix_confine(hostfs_mount *mount, const char *guest_path,
                             int flags, void **out_handle) {
    int anchor = (int)(intptr_t)mount->anchor;
    struct hostfs_open_how how;
    memset(&how, 0, sizeof(how));
    how.flags = (uint64_t)(unsigned int)flags | (uint64_t)O_CLOEXEC;
    how.mode = 0;
    if (flags & (HOSTFS_O_CREAT)) {
        how.mode = 0644;
    }
    how.resolve = HOSTFS_RESOLVE_BENEATH;

    long fd = syscall(SYS_openat2, anchor, guest_path, &how, sizeof(how));
    if (fd < 0) {
        return map_errno(errno);
    }

    posix_handle *h = (posix_handle *)calloc(1, sizeof(posix_handle));
    if (!h) {
        close((int)fd);
        return -(int64_t)HOSTFS_ENOMEM;
    }
    h->fd = (int)fd;

    struct stat st;
    if (fstat(h->fd, &st) == 0 && S_ISDIR(st.st_mode)) {
        h->is_dir = 1;
        int64_t rc = snapshot_dir(h);
        if (rc < 0) {
            close(h->fd);
            free(h->dents);
            free(h);
            return rc;
        }
    }
    *out_handle = h;
    return 0;
}

static int64_t posix_close(void *handle) {
    posix_handle *h = (posix_handle *)handle;
    if (!h) {
        return -(int64_t)HOSTFS_EBADF;
    }
    if (h->fd >= 0) {
        close(h->fd);
    }
    for (uint64_t i = 0; i < h->ndents; ++i) {
        free(h->dents[i].name);
    }
    free(h->dents);
    free(h);
    return 0;
}

static int64_t posix_read(void *handle, void *buf, uint64_t count) {
    posix_handle *h = (posix_handle *)handle;
    ssize_t r = read(h->fd, buf, (size_t)count);
    if (r < 0) {
        return map_errno(errno);
    }
    return (int64_t)r;
}

static int64_t posix_write(void *handle, const void *buf, uint64_t count) {
    posix_handle *h = (posix_handle *)handle;
    ssize_t r = write(h->fd, buf, (size_t)count);
    if (r < 0) {
        return map_errno(errno);
    }
    return (int64_t)r;
}

static int64_t posix_lseek(void *handle, int64_t offset, int whence) {
    posix_handle *h = (posix_handle *)handle;
    off_t r = lseek(h->fd, (off_t)offset, whence);
    if (r < 0) {
        return map_errno(errno);
    }
    return (int64_t)r;
}

static int64_t posix_fstat(void *handle, hostfs_stat *out) {
    posix_handle *h = (posix_handle *)handle;
    struct stat st;
    if (fstat(h->fd, &st) < 0) {
        return map_errno(errno);
    }
    out->st_dev = (uint64_t)st.st_dev;
    out->st_ino = (uint64_t)st.st_ino;
    out->st_nlink = (uint64_t)st.st_nlink;
    out->st_mode = (uint32_t)st.st_mode;
    out->st_uid = (uint32_t)st.st_uid;
    out->st_gid = (uint32_t)st.st_gid;
    out->st_rdev = (uint64_t)st.st_rdev;
    out->st_size = (int64_t)st.st_size;
    out->st_blksize = (int64_t)st.st_blksize;
    out->st_blocks = (int64_t)st.st_blocks;
    out->st_atime_sec = (int64_t)st.st_atim.tv_sec;
    out->st_atime_nsec = (int64_t)st.st_atim.tv_nsec;
    out->st_mtime_sec = (int64_t)st.st_mtim.tv_sec;
    out->st_mtime_nsec = (int64_t)st.st_mtim.tv_nsec;
    out->st_ctime_sec = (int64_t)st.st_ctim.tv_sec;
    out->st_ctime_nsec = (int64_t)st.st_ctim.tv_nsec;
    return 0;
}

static int64_t posix_getdents(void *handle, void *buf, uint64_t count) {
    posix_handle *h = (posix_handle *)handle;
    if (!h->is_dir) {
        return -(int64_t)HOSTFS_ENOTDIR;
    }
    if (h->cursor >= h->ndents) {
        return 0;   /* end of stream */
    }
    uint64_t off = 0;
    uint64_t k = h->cursor;
    for (; k < h->ndents; ++k) {
        if (!hostfs_emit_dirent((uint8_t *)buf, count, &off,
                                h->dents[k].ino, (int64_t)(k + 1),
                                h->dents[k].type, h->dents[k].name)) {
            break;
        }
    }
    if (off == 0) {
        return -(int64_t)HOSTFS_EINVAL;   /* buffer too small for one record */
    }
    h->cursor = k;
    return (int64_t)off;
}

/* maize-151 confinement for the path-mutating ops. The *at() family cannot resolve
   the whole path under RESOLVE_BENEATH the way openat2 does for open(), so we split the
   (already core-normalized) remainder into a parent directory and a final component,
   openat2(RESOLVE_BENEATH) the PARENT beneath the mount anchor, and then operate on the
   single final component within that confined parent fd. Because the core's
   normalize_path has already collapsed every "." / ".." / "//", the final component is a
   lone path element that cannot itself traverse; and RESOLVE_BENEATH rejects a `..` or
   an escaping symlink anywhere in the parent, exactly as open's confine does. The
   returned parent fd is an O_PATH|O_DIRECTORY handle valid as the dirfd of mkdirat /
   unlinkat / renameat. Returns 0 (fills *pfd + copies the final component into base) or
   a negative errno. */
static int64_t confine_parent(hostfs_mount *mount, const char *rel,
                              int *pfd, char *base, uint64_t basecap) {
    int anchor = (int)(intptr_t)mount->anchor;

    char parent[HOSTFS_PATH_MAX];
    const char *slash = strrchr(rel, '/');
    const char *final;
    if (!slash) {
        /* A lone component (e.g. ".savegame") or "." itself: the parent is the mount
           root, addressed as "." beneath the anchor. */
        parent[0] = '.';
        parent[1] = '\0';
        final = rel;
    } else {
        uint64_t plen = (uint64_t)(slash - rel);
        if (plen + 1 > sizeof(parent)) {
            return -(int64_t)HOSTFS_ENAMETOOLONG;
        }
        memcpy(parent, rel, (size_t)plen);
        parent[plen] = '\0';
        final = slash + 1;
    }

    uint64_t flen = strlen(final);
    if (flen + 1 > basecap) {
        return -(int64_t)HOSTFS_ENAMETOOLONG;
    }
    memcpy(base, final, (size_t)flen + 1);

    struct hostfs_open_how how;
    memset(&how, 0, sizeof(how));
    how.flags = (uint64_t)(O_PATH | O_DIRECTORY | O_CLOEXEC);
    how.resolve = HOSTFS_RESOLVE_BENEATH;
    long fd = syscall(SYS_openat2, anchor, parent, &how, sizeof(how));
    if (fd < 0) {
        return map_errno(errno);
    }
    *pfd = (int)fd;
    return 0;
}

/* Out-of-scope ops (owned by later cards, doc section 8): not guest-reachable because
   the core routes open through confine and dispatches no path-based open/stat number.
   Present so the ops struct is never partially NULL. */
static int64_t posix_open(hostfs_mount *m, const char *p, int fl, int mode) {
    (void)m; (void)p; (void)fl; (void)mode; return -(int64_t)HOSTFS_ENOSYS;
}
static int64_t posix_stat(hostfs_mount *m, const char *p, hostfs_stat *o) {
    (void)m; (void)p; (void)o; return -(int64_t)HOSTFS_ENOSYS;
}

/* maize-151 path-mutating ops (confined via confine_parent above). The core has already
   applied the :ro / synthetic-root write-gate before dispatching here. */
static int64_t posix_mkdir(hostfs_mount *m, const char *p, int mode) {
    int pfd = -1;
    char base[HOSTFS_PATH_MAX];
    int64_t rc = confine_parent(m, p, &pfd, base, sizeof(base));
    if (rc < 0) {
        return rc;
    }
    /* A guest that passes mode 0 would otherwise create an unusable 0000 directory;
       fall back to 0755 so the created dir is enterable (umask still applies). */
    mode_t md = (mode_t)(mode & 0777);
    if (md == 0) {
        md = 0755;
    }
    int r = mkdirat(pfd, base, md);
    int e = errno;
    close(pfd);
    if (r < 0) {
        return map_errno(e);
    }
    return 0;
}
static int64_t posix_rmdir(hostfs_mount *m, const char *p) {
    int pfd = -1;
    char base[HOSTFS_PATH_MAX];
    int64_t rc = confine_parent(m, p, &pfd, base, sizeof(base));
    if (rc < 0) {
        return rc;
    }
    int r = unlinkat(pfd, base, AT_REMOVEDIR);
    int e = errno;
    close(pfd);
    if (r < 0) {
        return map_errno(e);
    }
    return 0;
}
static int64_t posix_unlink(hostfs_mount *m, const char *p) {
    int pfd = -1;
    char base[HOSTFS_PATH_MAX];
    int64_t rc = confine_parent(m, p, &pfd, base, sizeof(base));
    if (rc < 0) {
        return rc;
    }
    int r = unlinkat(pfd, base, 0);
    int e = errno;
    close(pfd);
    if (r < 0) {
        return map_errno(e);
    }
    return 0;
}
static int64_t posix_rename(hostfs_mount *m, const char *o, const char *n) {
    int ofd = -1;
    int nfd = -1;
    char obase[HOSTFS_PATH_MAX];
    char nbase[HOSTFS_PATH_MAX];
    int64_t rc = confine_parent(m, o, &ofd, obase, sizeof(obase));
    if (rc < 0) {
        return rc;
    }
    rc = confine_parent(m, n, &nfd, nbase, sizeof(nbase));
    if (rc < 0) {
        close(ofd);
        return rc;
    }
    int r = renameat(ofd, obase, nfd, nbase);
    int e = errno;
    close(ofd);
    close(nfd);
    if (r < 0) {
        return map_errno(e);
    }
    return 0;
}

static const hostfs_backend_ops g_posix_ops = {
    posix_confine,
    posix_open,
    posix_close,
    posix_read,
    posix_write,
    posix_lseek,
    posix_stat,
    posix_fstat,
    posix_getdents,
    posix_mkdir,
    posix_rmdir,
    posix_unlink,
    posix_rename,
};

const hostfs_backend_ops *hostfs_backend_ops_get(void) {
    return &g_posix_ops;
}

int64_t hostfs_backend_anchor_open(hostfs_mount *mount) {
    int anchor = open(mount->host_root, O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (anchor < 0) {
        return map_errno(errno);
    }

    /* Enforce the openat2 kernel floor (>= 5.6): probe "." beneath the anchor. A
       kernel without openat2 returns ENOSYS; there is no realpath fallback. */
    struct hostfs_open_how how;
    memset(&how, 0, sizeof(how));
    how.flags = (uint64_t)(O_PATH | O_DIRECTORY | O_CLOEXEC);
    how.resolve = HOSTFS_RESOLVE_BENEATH;
    long probe = syscall(SYS_openat2, anchor, ".", &how, sizeof(how));
    if (probe < 0) {
        if (errno == ENOSYS) {
            close(anchor);
            return -(int64_t)HOSTFS_ENOSYS;   /* kernel too old for openat2 */
        }
        close(anchor);
        return map_errno(errno);
    }
    close((int)probe);

    mount->anchor = (void *)(intptr_t)anchor;
    return 0;
}

#endif /* __linux__ */
