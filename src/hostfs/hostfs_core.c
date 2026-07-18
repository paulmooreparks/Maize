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

/* Cap per cached top-level component name on a merge_root fd slot. Deliberately
   smaller than build_root_names' local 256-byte stack buffer: build_root_names'
   store lives on the stack of a single getdents_root call and costs nothing once
   that call returns, while merge_names is fd-slot-resident (every one of
   HOSTFS_MAX_FDS=256 slots carries this array whether or not it is a merge_root
   fd), so its per-name budget is deliberately tighter. 64 bytes is ample for a
   real mount's top-level guest component (top_component itself truncates any
   component past this bound rather than overflowing). */
#define HOSTFS_MERGE_NAME_MAX 64

typedef struct {
    int          in_use;
    int          is_root;      /* synthetic-root fd (no backend handle) */
    int          merge_root;   /* "/" fd backed by a real root mount that also has
                                   other granted mounts to merge into its listing */
    uint64_t     root_cursor;  /* next unique-mount index for root getdents; for a
                                   merge_root fd, indexes merge_names during the
                                   mount-name phase */
    int          merge_count;  /* count of deduped extra mount names, cached ONCE
                                   at hostfs_open time (not recomputed per call) */
    char         merge_names[HOSTFS_MAX_MOUNTS][HOSTFS_MERGE_NAME_MAX];
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

/* Longest-prefix match over an already-normalized absolute guest path. Because
   overlapping guest prefixes are rejected at startup, at most one mount matches at
   each prefix length; the longest prefix wins, so a root "/" mount (maize-132) is
   the fallback that yields to any deeper overlay. On a match returns the mount and
   copies the host-relative remainder into rel ("." for the mount root itself).

   A root prefix "/" (plen 1) matches every absolute path: its boundary character
   is the leading '/' itself, so the pfx[plen] boundary test that guards deeper
   prefixes ("/a" must not match "/ab") does not apply to it. */
static hostfs_mount *match_mount(hostfs_table *t, const char *path,
                                 char *rel, uint64_t relcap) {
    hostfs_mount *best = 0;
    size_t bestlen = 0;
    for (unsigned i = 0; i < t->count; ++i) {
        const char *pfx = t->mounts[i].guest_prefix;
        size_t plen = strlen(pfx);
        int is_root_pfx = (plen == 1 && pfx[0] == '/');
        if (strncmp(path, pfx, plen) == 0
            && (is_root_pfx || path[plen] == '\0' || path[plen] == '/')) {
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

/* maize-132: normalize a guest path for mount selection. A path that does not begin
   with '/' is joined onto the table cwd (default "/"); then "." components are
   dropped, "//" runs collapse, and ".." pops the last component, clamping at the
   root so ".." can never rise above "/". The result written to out is a canonical
   absolute path that always begins with '/' (and is exactly "/" for the root).

   This normalizes ONLY the guest path used to pick the mount and its host-relative
   remainder; it does NOT weaken the backend's host-side confinement. The backend
   still resolves that remainder beneath the mount anchor (openat2 RESOLVE_BENEATH /
   the Win32 canonical prefix-child check), so a crafted host path cannot escape a
   mount even though the guest-path ".." was already collapsed here.

   Returns 1 on success, or 0 when the joined/normalized path would overflow a
   buffer (the caller maps that to -ENAMETOOLONG). */
static int normalize_path(hostfs_table *t, const char *path,
                          char *out, uint64_t outcap) {
    /* 1. Join a relative path onto the cwd into a scratch buffer. */
    char joined[HOSTFS_PATH_MAX * 2];
    uint64_t ji = 0;
    if (path[0] != '/') {
        const char *cwd = (t && t->cwd && t->cwd[0]) ? t->cwd : "/";
        while (*cwd) {
            if (ji + 1 >= sizeof(joined)) {
                return 0;
            }
            joined[ji++] = *cwd++;
        }
        if (ji + 1 >= sizeof(joined)) {
            return 0;
        }
        joined[ji++] = '/';
    }
    while (*path) {
        if (ji + 1 >= sizeof(joined)) {
            return 0;
        }
        joined[ji++] = *path++;
    }
    joined[ji] = '\0';

    /* 2. Resolve components onto out. out holds either "/" (the root) or a
          "/seg1/seg2" form with no trailing slash. */
    if (outcap < 2) {
        return 0;
    }
    uint64_t oi = 1;
    out[0] = '/';
    uint64_t i = 0;
    while (joined[i]) {
        if (joined[i] == '/') {
            ++i;
            continue;
        }
        uint64_t j = i;
        while (joined[j] != '\0' && joined[j] != '/') {
            ++j;
        }
        uint64_t clen = j - i;
        if (clen == 1 && joined[i] == '.') {
            /* "." : current directory, drop it. */
        } else if (clen == 2 && joined[i] == '.' && joined[i + 1] == '.') {
            /* ".." : pop the last segment, clamping at the root. */
            if (oi > 1) {
                uint64_t k = oi;
                while (k > 1 && out[k - 1] != '/') {
                    --k;
                }
                oi = (k > 1) ? (k - 1) : 1;
            }
        } else {
            /* A real segment: append "[/]seg". */
            if (oi > 1) {
                if (oi + 1 >= outcap) {
                    return 0;
                }
                out[oi++] = '/';
            }
            if (oi + clen >= outcap) {
                return 0;
            }
            memcpy(out + oi, joined + i, (size_t)clen);
            oi += clen;
        }
        i = j;
    }
    out[oi] = '\0';
    return 1;
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

/* --- merged root (maize-255) ------------------------------------------------- */

/* Called once, at hostfs_open time, for a fd matched to the literal root mount.
   Computes the deduped extra-mount-name set and caches it on the fd slot so
   getdents_merge_root never re-probes the host filesystem per call.

   Existence probe uses ops->confine + ops->close, NOT ops->stat: both backends
   stub stat as -ENOSYS today. confine is what every real open() already resolves
   through, so opening the candidate name under the root mount's own anchor with
   flags=0 (no O_CREAT, no write-intent bits) and immediately closing on success is
   a correctness-equivalent, already-implemented existence check. A nonnegative rc
   means a physical entry of that name already exists (skip it, physical wins); a
   negative rc (ENOENT etc.) means no physical entry, so the candidate becomes a
   merged listing entry. */
static void cache_merge_extra_names(hostfs_table *t, hostfs_mount *root_mount,
                                    hostfs_fd_slot *s) {
    int n = 0;
    for (unsigned i = 0; i < t->count && n < HOSTFS_MAX_MOUNTS; ++i) {
        if (&t->mounts[i] == root_mount) {
            continue;   /* the root mount is the physical listing itself; its own
                           guest_prefix is "/" and top_component("/") would
                           otherwise yield a spurious empty-string name */
        }
        char comp[HOSTFS_MERGE_NAME_MAX];
        top_component(t->mounts[i].guest_prefix, comp, sizeof(comp));
        int dup = 0;
        for (int k = 0; k < n; ++k) {
            if (strcmp(s->merge_names[k], comp) == 0) {
                dup = 1;
                break;
            }
        }
        if (dup) {
            continue;
        }
        void *probe = 0;
        int64_t rc = t->ops->confine(root_mount, comp, 0, &probe);
        if (rc >= 0) {
            t->ops->close(probe);
            continue;   /* a real physical entry of this name already exists: skip */
        }
        memcpy(s->merge_names[n], comp, strlen(comp) + 1);
        ++n;
    }
    s->merge_count = n;
}

static int64_t getdents_merge_root(hostfs_fd_slot *s, hostfs_table *t,
                                   uint8_t *buf, uint64_t count) {
    uint64_t cursor = s->root_cursor;
    if (cursor < (uint64_t)s->merge_count) {
        uint64_t off = 0;
        uint64_t k = cursor;
        for (; k < (uint64_t)s->merge_count; ++k) {
            if (!hostfs_emit_dirent(buf, count, &off,
                                    HOSTFS_ROOT_INO + k + 1, (int64_t)(k + 1),
                                    HOSTFS_DT_DIR, s->merge_names[k])) {
                break;
            }
        }
        if (off == 0) {
            return -HOSTFS_EINVAL;
        }
        s->root_cursor = k;
        return (int64_t)off;
    }
    /* Mount-name phase exhausted (possibly on an earlier call): delegate the rest
       of the stream to the backend's own physical getdents on the already-open
       handle. */
    return t->ops->getdents(s->handle, buf, count);
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

    /* maize-132: normalize (join onto cwd, resolve . / .. / //) BEFORE mount
       selection, so a relative path (DOOM's ./.savegame/temp.dsg) resolves against
       the cwd and lands in the right mount. */
    char norm[HOSTFS_PATH_MAX];
    if (!normalize_path(t, path, norm, sizeof(norm))) {
        return -HOSTFS_ENAMETOOLONG;
    }

    char rel[HOSTFS_PATH_MAX];
    hostfs_mount *m = match_mount(t, norm, rel, sizeof(rel));
    if (m) {
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
        /* maize-255: a fd opened for the literal root mount ("/") also merges in
           the top-level names of every other granted mount, deduped against this
           mount's own physical entries. This is the same is_root_pfx test
           match_mount already applies internally; it is duplicated here rather
           than plumbed out through match_mount's signature to keep the diff to
           the two call sites that need it. A fd opened for a deeper mount (e.g.
           /bin, its own registered mount) never sets this. */
        size_t plen = strlen(m->guest_prefix);
        if (plen == 1 && m->guest_prefix[0] == '/') {
            s->merge_root = 1;
            cache_merge_extra_names(t, m, s);
        }
        return fd;
    }

    /* No mount matched. The synthetic read-only root is the fallback for the
       --no-root case (no "/" mount): "/" enumerates the top-level mount names and
       is never writable. When a "/" root mount IS present it matches above, so this
       path is not reached for it. */
    if (strcmp(norm, "/") == 0) {
        if (hostfs_flags_write_intent(flags)) {
            return -HOSTFS_EROFS;   /* the synthetic root is never writable */
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
    return -HOSTFS_ENOENT;   /* unmounted path (indistinguishable from absent) */
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

int64_t hostfs_ftruncate(hostfs_table *t, int fd, int64_t length) {
    hostfs_fd_slot *s = slot_for_fd(fd);
    if (!s) {
        return -HOSTFS_EBADF;
    }
    if (s->is_root || (s->mount && s->mount->mode == HOSTFS_RO)) {
        return -HOSTFS_EROFS;   /* write-intent op on a :ro mount / synthetic root */
    }
    if (length < 0) {
        return -HOSTFS_EINVAL;
    }
    if (!t->ops->ftruncate) {
        return -HOSTFS_ENOSYS;
    }
    return t->ops->ftruncate(s->handle, length);
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
    if (s->merge_root) {
        if (whence == HOSTFS_SEEK_SET && offset == 0) {
            s->root_cursor = 0;   /* rewind the mount-name phase */
            /* Rewind the physical phase too. Both backends populate their
               getdents cursor from a directory snapshot taken once at confine
               time (posix_handle.cursor / win_handle.cursor); their lseek ops
               forward to the host file-position API, which that snapshot-driven
               getdents never consults, so a bare ops->lseek(handle, 0, SEEK_SET)
               would not actually replay the physical entries on a second read.
               Re-confining the root mount is the same call hostfs_open already
               made to acquire this fd's handle in the first place, and yields a
               fresh handle with a fresh snapshot at cursor 0, genuinely
               reproducing the physical-phase portion of the merged set. */
            void *fresh = 0;
            int64_t rc = t->ops->confine(s->mount, ".", 0, &fresh);
            if (rc < 0) {
                return rc;
            }
            t->ops->close(s->handle);
            s->handle = fresh;
            return 0;
        }
        /* Matches is_root's posture: only rewind is supported on a directory fd,
           no passthrough of arbitrary seeks to the backend's own (opaque,
           cookie-based) physical stream. */
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
    if (s->merge_root) {
        return getdents_merge_root(s, t, buf, count);
    }
    return t->ops->getdents(s->handle, buf, count);
}

/* --- path-mutating dispatch (maize-151) ------------------------------------- */

/* Shared front half of every path-mutating op: normalize the guest path against the
   cwd, longest-prefix match a mount, and apply the write-intent gate. On success fills
   *out_mount and copies the host-relative remainder into rel; on any gate failure
   returns the negative errno the op must return verbatim:
     -ENAMETOOLONG  the joined/normalized path overflowed a buffer
     -EROFS         a :ro mount, or the synthetic root (never writable)
     -ENOENT        no mount matched (indistinguishable from absent)
   This mirrors hostfs_open's normalize -> match -> :ro-gate sequence so a mutating op
   and an open agree on which paths are writable. */
static int64_t resolve_writable(hostfs_table *t, const char *path,
                                char *rel, uint64_t relcap,
                                hostfs_mount **out_mount) {
    char norm[HOSTFS_PATH_MAX];
    if (!normalize_path(t, path, norm, sizeof(norm))) {
        return -HOSTFS_ENAMETOOLONG;
    }
    hostfs_mount *m = match_mount(t, norm, rel, relcap);
    if (!m) {
        /* The synthetic root matches no mount but is never writable, so a
           write-intent op on it is EROFS rather than ENOENT (mirrors hostfs_open). */
        if (strcmp(norm, "/") == 0) {
            return -HOSTFS_EROFS;
        }
        return -HOSTFS_ENOENT;
    }
    if (m->mode == HOSTFS_RO) {
        return -HOSTFS_EROFS;
    }
    *out_mount = m;
    return 0;
}

int64_t hostfs_mkdir(hostfs_table *t, const char *path, int mode) {
    if (!t || !t->ops) {
        return -HOSTFS_ENOENT;   /* nothing mounted */
    }
    char rel[HOSTFS_PATH_MAX];
    hostfs_mount *m = 0;
    int64_t rc = resolve_writable(t, path, rel, sizeof(rel), &m);
    if (rc < 0) {
        return rc;
    }
    if (!t->ops->mkdir) {
        return -HOSTFS_ENOSYS;
    }
    return t->ops->mkdir(m, rel, mode);
}

int64_t hostfs_unlink(hostfs_table *t, const char *path) {
    if (!t || !t->ops) {
        return -HOSTFS_ENOENT;
    }
    char rel[HOSTFS_PATH_MAX];
    hostfs_mount *m = 0;
    int64_t rc = resolve_writable(t, path, rel, sizeof(rel), &m);
    if (rc < 0) {
        return rc;
    }
    if (!t->ops->unlink) {
        return -HOSTFS_ENOSYS;
    }
    return t->ops->unlink(m, rel);
}

int64_t hostfs_rename(hostfs_table *t, const char *oldp, const char *newp) {
    if (!t || !t->ops) {
        return -HOSTFS_ENOENT;
    }
    char relold[HOSTFS_PATH_MAX];
    char relnew[HOSTFS_PATH_MAX];
    hostfs_mount *mo = 0;
    hostfs_mount *mn = 0;
    int64_t rc = resolve_writable(t, oldp, relold, sizeof(relold), &mo);
    if (rc < 0) {
        return rc;
    }
    rc = resolve_writable(t, newp, relnew, sizeof(relnew), &mn);
    if (rc < 0) {
        return rc;
    }
    /* Both paths must land in the SAME mount: the backend renameat / MoveFileEx is a
       single host operation and cannot cross host directories, and a cross-mount move
       could leak content between differently-posture'd grants. -EXDEV is the code the
       C library's rename() maps to a failed cross-device move. */
    if (mo != mn) {
        return -HOSTFS_EXDEV;
    }
    if (!t->ops->rename) {
        return -HOSTFS_ENOSYS;
    }
    return t->ops->rename(mo, relold, relnew);
}
