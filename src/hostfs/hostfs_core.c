/* hostfs_core.c: the portable VFS core (doc section 6). Owns longest-prefix
 * guest-path matching, the :ro/:rw write-intent gate (EROFS), synthetic-root
 * enumeration, the guest fd table, and the byte-ABI composition of the section-2
 * struct stat and linux_dirent64 images. It delegates every host touch (confine,
 * open, read, write, lseek, fstat, getdents, close) to the backend ops. It
 * contains NO host or VM headers and NO confinement logic.
 */
#include <stdint.h>
#include <string.h>
#include "hostfs_core.h"

/* Upper bound on simultaneously-open guest fds and on granted mounts. */
#define HOSTFS_MAX_FDS    256
#define HOSTFS_FD_BASE    3
#define HOSTFS_MAX_MOUNTS 64

typedef struct {
    int          in_use;
    int          is_root;      /* synthetic-root fd (no backend handle) */
    uint64_t     root_cursor;  /* next unique-mount index for root getdents */
    hostfs_mount *mount;       /* owning mount (NULL for root) */
    void         *handle;      /* backend handle (NULL for root) */
} hostfs_fd_slot;

static hostfs_fd_slot g_fds[HOSTFS_MAX_FDS];

/* --- little-endian byte writers (host-endianness independent) --------------- */

static void put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static void put_u64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        p[i] = (uint8_t)((v >> (i * 8)) & 0xFF);
    }
}

/* --- fd table --------------------------------------------------------------- */

void hostfs_reset_fds(void) {
    memset(g_fds, 0, sizeof(g_fds));
}

/* Lowest free guest fd >= 3, or -1 when the table is full. */
static int alloc_fd(void) {
    for (int i = 0; i < HOSTFS_MAX_FDS; ++i) {
        if (!g_fds[i].in_use) {
            memset(&g_fds[i], 0, sizeof(g_fds[i]));
            g_fds[i].in_use = 1;
            return i + HOSTFS_FD_BASE;
        }
    }
    return -1;
}

static hostfs_fd_slot *slot_for_fd(int fd) {
    int idx = fd - HOSTFS_FD_BASE;
    if (idx < 0 || idx >= HOSTFS_MAX_FDS) {
        return 0;
    }
    if (!g_fds[idx].in_use) {
        return 0;
    }
    return &g_fds[idx];
}

/* --- helpers ---------------------------------------------------------------- */

int hostfs_flags_write_intent(int flags) {
    int acc = flags & HOSTFS_O_ACCMODE;
    if (acc == HOSTFS_O_WRONLY || acc == HOSTFS_O_RDWR) {
        return 1;
    }
    if (flags & (HOSTFS_O_CREAT | HOSTFS_O_TRUNC)) {
        return 1;
    }
    return 0;
}

/* Longest-prefix match. Because overlapping guest prefixes are rejected at
   startup, at most one mount matches. On a match returns the mount and copies the
   host-relative remainder into rel ("." for the mount root itself). */
static hostfs_mount *match_mount(hostfs_table *t, const char *path,
                                 char *rel, uint64_t relcap) {
    hostfs_mount *best = 0;
    size_t bestlen = 0;
    for (unsigned i = 0; i < t->count; ++i) {
        const char *pfx = t->mounts[i].guest_prefix;
        size_t plen = strlen(pfx);
        if (strncmp(path, pfx, plen) == 0
            && (path[plen] == '\0' || path[plen] == '/')) {
            if (plen > bestlen) {
                bestlen = plen;
                best = &t->mounts[i];
            }
        }
    }
    if (!best) {
        return 0;
    }
    const char *rem = path + bestlen;
    while (*rem == '/') {
        ++rem;
    }
    if (*rem == '\0') {
        if (relcap < 2) {
            return 0;
        }
        rel[0] = '.';
        rel[1] = '\0';
        return best;
    }
    size_t rlen = strlen(rem);
    if (rlen + 1 > relcap) {
        return 0;   /* caller maps a NULL return under a too-long remainder */
    }
    memcpy(rel, rem, rlen + 1);
    return best;
}

/* --- byte-ABI composition --------------------------------------------------- */

void hostfs_encode_stat(const hostfs_stat *in, uint8_t out[HOSTFS_STAT_SIZE]) {
    memset(out, 0, HOSTFS_STAT_SIZE);
    put_u64(out + 0,   in->st_dev);
    put_u64(out + 8,   in->st_ino);
    put_u64(out + 16,  in->st_nlink);
    put_u32(out + 24,  in->st_mode);
    put_u32(out + 28,  in->st_uid);
    put_u32(out + 32,  in->st_gid);
    /* out+36 __pad0 stays zero */
    put_u64(out + 40,  in->st_rdev);
    put_u64(out + 48,  (uint64_t)in->st_size);
    put_u64(out + 56,  (uint64_t)in->st_blksize);
    put_u64(out + 64,  (uint64_t)in->st_blocks);
    put_u64(out + 72,  (uint64_t)in->st_atime_sec);
    put_u64(out + 80,  (uint64_t)in->st_atime_nsec);
    put_u64(out + 88,  (uint64_t)in->st_mtime_sec);
    put_u64(out + 96,  (uint64_t)in->st_mtime_nsec);
    put_u64(out + 104, (uint64_t)in->st_ctime_sec);
    put_u64(out + 112, (uint64_t)in->st_ctime_nsec);
    /* out+120 __unused[3] stays zero */
}

uint64_t hostfs_dirent_reclen(uint64_t name_len) {
    uint64_t base = 19 + name_len + 1;   /* header + name + NUL */
    return (base + 7) & ~(uint64_t)7;    /* round to 8-byte boundary */
}

int hostfs_emit_dirent(uint8_t *buf, uint64_t bufcap, uint64_t *poff,
                       uint64_t ino, int64_t cookie, uint8_t d_type,
                       const char *name) {
    uint64_t nlen = strlen(name);
    uint64_t reclen = hostfs_dirent_reclen(nlen);
    if (*poff + reclen > bufcap) {
        return 0;
    }
    uint8_t *rec = buf + *poff;
    memset(rec, 0, (size_t)reclen);
    put_u64(rec + 0, ino);
    put_u64(rec + 8, (uint64_t)cookie);
    put_u16(rec + 16, (uint16_t)reclen);
    rec[18] = d_type;
    memcpy(rec + 19, name, (size_t)nlen);   /* rec already zeroed -> NUL follows */
    *poff += reclen;
    return 1;
}

/* --- synthetic root --------------------------------------------------------- */

/* Extract the first path component of a guest prefix ("/proj" -> "proj",
   "/home/user" -> "home") into name (bounded). Returns the length, or 0. */
static uint64_t top_component(const char *prefix, char *name, uint64_t cap) {
    const char *start = prefix;
    while (*start == '/') {
        ++start;
    }
    const char *end = start;
    while (*end != '\0' && *end != '/') {
        ++end;
    }
    uint64_t len = (uint64_t)(end - start);
    if (len + 1 > cap) {
        len = cap - 1;
    }
    memcpy(name, start, (size_t)len);
    name[len] = '\0';
    return len;
}

/* Build the ordered, de-duplicated list of top-level root entry names. Returns
   the count; names[] point into the caller's backing store. */
static int build_root_names(hostfs_table *t,
                            char store[HOSTFS_MAX_MOUNTS][256]) {
    int n = 0;
    for (unsigned i = 0; i < t->count && n < HOSTFS_MAX_MOUNTS; ++i) {
        char comp[256];
        top_component(t->mounts[i].guest_prefix, comp, sizeof(comp));
        int dup = 0;
        for (int k = 0; k < n; ++k) {
            if (strcmp(store[k], comp) == 0) {
                dup = 1;
                break;
            }
        }
        if (!dup) {
            memcpy(store[n], comp, strlen(comp) + 1);
            ++n;
        }
    }
    return n;
}

static int64_t getdents_root(hostfs_fd_slot *s, hostfs_table *t,
                             uint8_t *buf, uint64_t count) {
    char store[HOSTFS_MAX_MOUNTS][256];
    int n = build_root_names(t, store);
    uint64_t cursor = s->root_cursor;
    if (cursor >= (uint64_t)n) {
        return 0;   /* end of stream */
    }
    uint64_t off = 0;
    uint64_t k = cursor;
    for (; k < (uint64_t)n; ++k) {
        if (!hostfs_emit_dirent(buf, count, &off,
                                HOSTFS_ROOT_INO + k + 1, (int64_t)(k + 1),
                                HOSTFS_DT_DIR, store[k])) {
            break;
        }
    }
    if (off == 0) {
        return -HOSTFS_EINVAL;   /* buffer too small for even one record */
    }
    s->root_cursor = k;
    return (int64_t)off;
}

static void encode_root_stat(uint8_t *out) {
    hostfs_stat st;
    memset(&st, 0, sizeof(st));
    st.st_ino = HOSTFS_ROOT_INO;
    st.st_nlink = 2;
    st.st_mode = HOSTFS_S_IFDIR | 0555;   /* read-only synthetic directory */
    st.st_blksize = 4096;
    hostfs_encode_stat(&st, out);
}

/* --- guest-visible dispatch ------------------------------------------------- */

int64_t hostfs_open(hostfs_table *t, const char *path, int flags, int mode) {
    if (!t || !t->ops) {
        return -HOSTFS_ENOENT;   /* nothing mounted */
    }
    if (strcmp(path, "/") == 0) {
        if (hostfs_flags_write_intent(flags)) {
            return -HOSTFS_EROFS;   /* the root is never writable */
        }
        int fd = alloc_fd();
        if (fd < 0) {
            return -HOSTFS_ENOMEM;
        }
        hostfs_fd_slot *s = slot_for_fd(fd);
        s->is_root = 1;
        s->root_cursor = 0;
        return fd;
    }

    char rel[HOSTFS_PATH_MAX];
    hostfs_mount *m = match_mount(t, path, rel, sizeof(rel));
    if (!m) {
        return -HOSTFS_ENOENT;   /* unmounted path (indistinguishable from absent) */
    }
    if (m->mode == HOSTFS_RO && hostfs_flags_write_intent(flags)) {
        return -HOSTFS_EROFS;
    }
    void *handle = 0;
    int64_t rc = t->ops->confine(m, rel, flags, &handle);
    if (rc < 0) {
        return rc;
    }
    int fd = alloc_fd();
    if (fd < 0) {
        t->ops->close(handle);
        return -HOSTFS_ENOMEM;
    }
    hostfs_fd_slot *s = slot_for_fd(fd);
    s->mount = m;
    s->handle = handle;
    return fd;
}

int64_t hostfs_close(hostfs_table *t, int fd) {
    hostfs_fd_slot *s = slot_for_fd(fd);
    if (!s) {
        return -HOSTFS_EBADF;
    }
    int64_t rc = 0;
    if (!s->is_root && s->handle) {
        rc = t->ops->close(s->handle);
    }
    s->in_use = 0;
    s->handle = 0;
    s->mount = 0;
    s->is_root = 0;
    return rc;
}

int64_t hostfs_read(hostfs_table *t, int fd, void *buf, uint64_t count) {
    hostfs_fd_slot *s = slot_for_fd(fd);
    if (!s) {
        return -HOSTFS_EBADF;
    }
    if (s->is_root) {
        return -HOSTFS_EISDIR;
    }
    return t->ops->read(s->handle, buf, count);
}

int64_t hostfs_write(hostfs_table *t, int fd, const void *buf, uint64_t count) {
    hostfs_fd_slot *s = slot_for_fd(fd);
    if (!s) {
        return -HOSTFS_EBADF;
    }
    if (s->is_root || (s->mount && s->mount->mode == HOSTFS_RO)) {
        return -HOSTFS_EROFS;   /* write-intent op on a :ro mount / synthetic root */
    }
    return t->ops->write(s->handle, buf, count);
}

int64_t hostfs_lseek(hostfs_table *t, int fd, int64_t offset, int whence) {
    hostfs_fd_slot *s = slot_for_fd(fd);
    if (!s) {
        return -HOSTFS_EBADF;
    }
    if (whence != HOSTFS_SEEK_SET && whence != HOSTFS_SEEK_CUR
        && whence != HOSTFS_SEEK_END) {
        return -HOSTFS_EINVAL;
    }
    if (s->is_root) {
        if (whence == HOSTFS_SEEK_SET && offset == 0) {
            s->root_cursor = 0;   /* rewinddir on the synthetic root */
            return 0;
        }
        return -HOSTFS_EINVAL;
    }
    return t->ops->lseek(s->handle, offset, whence);
}

int64_t hostfs_fstat(hostfs_table *t, int fd, uint8_t *out_stat) {
    hostfs_fd_slot *s = slot_for_fd(fd);
    if (!s) {
        return -HOSTFS_EBADF;
    }
    if (s->is_root) {
        encode_root_stat(out_stat);
        return 0;
    }
    hostfs_stat st;
    memset(&st, 0, sizeof(st));
    int64_t rc = t->ops->fstat(s->handle, &st);
    if (rc < 0) {
        return rc;
    }
    hostfs_encode_stat(&st, out_stat);
    return 0;
}

int64_t hostfs_getdents(hostfs_table *t, int fd, uint8_t *buf, uint64_t count) {
    hostfs_fd_slot *s = slot_for_fd(fd);
    if (!s) {
        return -HOSTFS_EBADF;
    }
    if (s->is_root) {
        return getdents_root(s, t, buf, count);
    }
    return t->ops->getdents(s->handle, buf, count);
}
