/* hostfs_win32.c: the Windows backend behind hostfs_backend_ops (doc sections 4, 5).
 *
 * Confinement lives ENTIRELY here (comment 2226, binding). The mount root is opened
 * once and canonicalized with GetFinalPathNameByHandle. Every resolution opens the
 * target with FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, canonicalizes
 * the result the same way, and verifies the canonical path is a prefix-child of the
 * mount root's canonical path before handing back a handle. A reparse point (junction
 * or symlink) is refused by default with -EACCES (the reparse policy's default; the
 * opt-in resolve-and-verify-beneath is not required for the POC).
 *
 * Whole file compiles to nothing off Windows (guarded), so both backends can be added
 * to both build targets unconditionally.
 */
#ifdef _WIN32

#define NOMINMAX
#include <windows.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "hostfs_core.h"

/* ReadFile/WriteFile take a 32-bit length; chunk a 64-bit count into <=DWORD slices. */
#define WIN_CHUNK_MAX 0x7FFFFFFF

typedef struct {
    char    *name;   /* UTF-8, guest-facing */
    uint8_t  type;   /* DT_DIR / DT_REG */
} win_dent;

typedef struct {
    HANDLE     h;
    int        is_dir;
    win_dent  *dents;
    uint64_t   ndents;
    uint64_t   cursor;
} win_handle;

typedef struct {
    HANDLE   root;        /* kept open for process lifetime */
    wchar_t *canon_root;  /* canonical DOS path of the mount root */
} win_anchor;

/* --- small wide/utf8 helpers ------------------------------------------------ */

static wchar_t *widen(const char *s) {
    int need = MultiByteToWideChar(CP_ACP, 0, s, -1, NULL, 0);
    if (need <= 0) {
        return NULL;
    }
    wchar_t *w = (wchar_t *)malloc((size_t)need * sizeof(wchar_t));
    if (!w) {
        return NULL;
    }
    MultiByteToWideChar(CP_ACP, 0, s, -1, w, need);
    return w;
}

static char *narrow(const wchar_t *w) {
    int need = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (need <= 0) {
        return NULL;
    }
    char *s = (char *)malloc((size_t)need);
    if (!s) {
        return NULL;
    }
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s, need, NULL, NULL);
    return s;
}

/* Canonical DOS path of an open handle, malloc'd, or NULL. */
static wchar_t *canon_of(HANDLE h) {
    DWORD need = GetFinalPathNameByHandleW(h, NULL, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (need == 0) {
        return NULL;
    }
    wchar_t *buf = (wchar_t *)malloc((size_t)(need + 1) * sizeof(wchar_t));
    if (!buf) {
        return NULL;
    }
    DWORD got = GetFinalPathNameByHandleW(h, buf, need, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (got == 0 || got >= need + 1) {
        free(buf);
        return NULL;
    }
    return buf;
}

/* True if `path` equals `root` or is a strict prefix-child (root + "\\" + rest),
   case-insensitively (Windows filesystem posture). */
static int is_beneath(const wchar_t *root, const wchar_t *path) {
    size_t rl = wcslen(root);
    if (_wcsnicmp(root, path, rl) != 0) {
        return 0;
    }
    if (path[rl] == L'\0') {
        return 1;
    }
    if (path[rl] == L'\\') {
        return 1;
    }
    return 0;
}

/* GetLastError -> ABI errno. */
static int64_t map_last_error(DWORD e) {
    switch (e) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_INVALID_NAME:
            return -(int64_t)HOSTFS_ENOENT;
        case ERROR_ACCESS_DENIED:
        case ERROR_SHARING_VIOLATION:
            return -(int64_t)HOSTFS_EACCES;
        case ERROR_WRITE_PROTECT:
            return -(int64_t)HOSTFS_EROFS;
        case ERROR_FILE_EXISTS:
        case ERROR_ALREADY_EXISTS:
            return -(int64_t)HOSTFS_EEXIST;
        case ERROR_DIRECTORY:
            return -(int64_t)HOSTFS_ENOTDIR;
        default:
            return -(int64_t)HOSTFS_EIO;
    }
}

/* Translate the guest open flags to a Windows CreateFile disposition/access. */
static void translate_flags(int flags, DWORD *access, DWORD *disp) {
    int acc = flags & HOSTFS_O_ACCMODE;
    if (acc == HOSTFS_O_WRONLY) {
        *access = GENERIC_WRITE;
    } else if (acc == HOSTFS_O_RDWR) {
        *access = GENERIC_READ | GENERIC_WRITE;
    } else {
        *access = GENERIC_READ;
    }
    if (flags & HOSTFS_O_APPEND) {
        *access |= FILE_APPEND_DATA;
    }

    if (flags & HOSTFS_O_CREAT) {
        if (flags & HOSTFS_O_EXCL) {
            *disp = CREATE_NEW;
        } else if (flags & HOSTFS_O_TRUNC) {
            *disp = CREATE_ALWAYS;
        } else {
            *disp = OPEN_ALWAYS;
        }
    } else if (flags & HOSTFS_O_TRUNC) {
        *disp = TRUNCATE_EXISTING;
    } else {
        *disp = OPEN_EXISTING;
    }
}

/* Snapshot a directory's entries via FindFirstFile, skipping "." and "..". */
static int64_t snapshot_dir(win_handle *wh, const wchar_t *target) {
    size_t tl = wcslen(target);
    wchar_t *pat = (wchar_t *)malloc((tl + 3) * sizeof(wchar_t));
    if (!pat) {
        return -(int64_t)HOSTFS_ENOMEM;
    }
    memcpy(pat, target, tl * sizeof(wchar_t));
    pat[tl] = L'\\';
    pat[tl + 1] = L'*';
    pat[tl + 2] = L'\0';

    WIN32_FIND_DATAW fd;
    HANDLE fh = FindFirstFileW(pat, &fd);
    free(pat);
    if (fh == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND) {
            return 0;   /* empty directory */
        }
        return map_last_error(e);
    }

    uint64_t cap = 0;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) {
            continue;
        }
        if (wh->ndents >= cap) {
            uint64_t ncap = cap ? cap * 2 : 16;
            win_dent *nd = (win_dent *)realloc(wh->dents, (size_t)ncap * sizeof(win_dent));
            if (!nd) {
                FindClose(fh);
                return -(int64_t)HOSTFS_ENOMEM;
            }
            wh->dents = nd;
            cap = ncap;
        }
        char *nm = narrow(fd.cFileName);
        if (!nm) {
            FindClose(fh);
            return -(int64_t)HOSTFS_ENOMEM;
        }
        wh->dents[wh->ndents].name = nm;
        wh->dents[wh->ndents].type =
            (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? HOSTFS_DT_DIR : HOSTFS_DT_REG;
        ++wh->ndents;
    } while (FindNextFileW(fh, &fd));

    FindClose(fh);
    return 0;
}

/* --- ops -------------------------------------------------------------------- */

static int64_t win_confine(hostfs_mount *mount, const char *guest_path,
                           int flags, void **out_handle) {
    win_anchor *anc = (win_anchor *)mount->anchor;

    /* Build the target wide path: canon_root [ + "\\" + rel-with-backslashes ]. */
    wchar_t *rel = NULL;
    int is_root_itself = (strcmp(guest_path, ".") == 0);
    if (!is_root_itself) {
        rel = widen(guest_path);
        if (!rel) {
            return -(int64_t)HOSTFS_ENOMEM;
        }
        for (wchar_t *p = rel; *p; ++p) {
            if (*p == L'/') {
                *p = L'\\';
            }
        }
    }

    size_t rootl = wcslen(anc->canon_root);
    size_t rell = rel ? wcslen(rel) : 0;
    wchar_t *target = (wchar_t *)malloc((rootl + rell + 2) * sizeof(wchar_t));
    if (!target) {
        free(rel);
        return -(int64_t)HOSTFS_ENOMEM;
    }
    memcpy(target, anc->canon_root, rootl * sizeof(wchar_t));
    size_t tl = rootl;
    if (rel) {
        target[tl++] = L'\\';
        memcpy(target + tl, rel, rell * sizeof(wchar_t));
        tl += rell;
    }
    target[tl] = L'\0';
    free(rel);

    DWORD access, disp;
    translate_flags(flags, &access, &disp);
    DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    DWORD winflags = FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT;

    HANDLE h = CreateFileW(target, access, share, NULL, disp, winflags, NULL);
    free(target);
    if (h == INVALID_HANDLE_VALUE) {
        return map_last_error(GetLastError());
    }

    /* Reparse policy: refuse a junction / symlink by default. */
    BY_HANDLE_FILE_INFORMATION info;
    if (!GetFileInformationByHandle(h, &info)) {
        CloseHandle(h);
        return -(int64_t)HOSTFS_EIO;
    }
    if (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        CloseHandle(h);
        return -(int64_t)HOSTFS_EACCES;
    }

    /* Canonicalize and prefix-verify against the mount root. */
    wchar_t *canon = canon_of(h);
    if (!canon) {
        CloseHandle(h);
        return -(int64_t)HOSTFS_EIO;
    }
    if (!is_beneath(anc->canon_root, canon)) {
        free(canon);
        CloseHandle(h);
        return -(int64_t)HOSTFS_EACCES;
    }
    free(canon);

    win_handle *wh = (win_handle *)calloc(1, sizeof(win_handle));
    if (!wh) {
        CloseHandle(h);
        return -(int64_t)HOSTFS_ENOMEM;
    }
    wh->h = h;
    if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        wh->is_dir = 1;
        wchar_t *canon2 = canon_of(h);
        if (canon2) {
            int64_t rc = snapshot_dir(wh, canon2);
            free(canon2);
            if (rc < 0) {
                CloseHandle(h);
                free(wh->dents);
                free(wh);
                return rc;
            }
        }
    }
    *out_handle = wh;
    return 0;
}

static int64_t win_close(void *handle) {
    win_handle *wh = (win_handle *)handle;
    if (!wh) {
        return -(int64_t)HOSTFS_EBADF;
    }
    if (wh->h != INVALID_HANDLE_VALUE) {
        CloseHandle(wh->h);
    }
    for (uint64_t i = 0; i < wh->ndents; ++i) {
        free(wh->dents[i].name);
    }
    free(wh->dents);
    free(wh);
    return 0;
}

static int64_t win_read(void *handle, void *buf, uint64_t count) {
    win_handle *wh = (win_handle *)handle;
    uint8_t *cursor = (uint8_t *)buf;
    uint64_t total = 0;
    while (count > 0) {
        DWORD chunk = (DWORD)(count < WIN_CHUNK_MAX ? count : WIN_CHUNK_MAX);
        DWORD got = 0;
        if (!ReadFile(wh->h, cursor, chunk, &got, NULL)) {
            return -(int64_t)HOSTFS_EIO;
        }
        total += got;
        if (got < chunk) {
            break;   /* end of file */
        }
        cursor += got;
        count -= got;
    }
    return (int64_t)total;
}

static int64_t win_write(void *handle, const void *buf, uint64_t count) {
    win_handle *wh = (win_handle *)handle;
    const uint8_t *cursor = (const uint8_t *)buf;
    uint64_t total = 0;
    while (count > 0) {
        DWORD chunk = (DWORD)(count < WIN_CHUNK_MAX ? count : WIN_CHUNK_MAX);
        DWORD put = 0;
        if (!WriteFile(wh->h, cursor, chunk, &put, NULL)) {
            return -(int64_t)HOSTFS_EIO;
        }
        total += put;
        if (put < chunk) {
            break;
        }
        cursor += put;
        count -= put;
    }
    return (int64_t)total;
}

static int64_t win_lseek(void *handle, int64_t offset, int whence) {
    win_handle *wh = (win_handle *)handle;
    DWORD method = FILE_BEGIN;
    if (whence == HOSTFS_SEEK_CUR) {
        method = FILE_CURRENT;
    } else if (whence == HOSTFS_SEEK_END) {
        method = FILE_END;
    }
    LARGE_INTEGER dist, out;
    dist.QuadPart = offset;
    if (!SetFilePointerEx(wh->h, dist, &out, method)) {
        return -(int64_t)HOSTFS_EINVAL;
    }
    if (out.QuadPart < 0) {
        return -(int64_t)HOSTFS_EINVAL;
    }
    return (int64_t)out.QuadPart;
}

/* FILETIME (100ns ticks since 1601) -> unix seconds/nanoseconds. */
static void filetime_to_unix(FILETIME ft, int64_t *sec, int64_t *nsec) {
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | (uint64_t)ft.dwLowDateTime;
    const uint64_t epoch = 116444736000000000ULL;   /* 1601 -> 1970 in 100ns ticks */
    if (t < epoch) {
        *sec = 0;
        *nsec = 0;
        return;
    }
    t -= epoch;
    *sec = (int64_t)(t / 10000000ULL);
    *nsec = (int64_t)((t % 10000000ULL) * 100ULL);
}

static int64_t win_fstat(void *handle, hostfs_stat *out) {
    win_handle *wh = (win_handle *)handle;
    BY_HANDLE_FILE_INFORMATION info;
    if (!GetFileInformationByHandle(wh->h, &info)) {
        return -(int64_t)HOSTFS_EIO;
    }
    int is_dir = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
    int readonly = (info.dwFileAttributes & FILE_ATTRIBUTE_READONLY) ? 1 : 0;

    out->st_dev = (uint64_t)info.dwVolumeSerialNumber;
    out->st_ino = ((uint64_t)info.nFileIndexHigh << 32) | (uint64_t)info.nFileIndexLow;
    out->st_nlink = (uint64_t)info.nNumberOfLinks;
    /* Synthesized mode (doc section 5): directories 0755; files 0444 read-only
       else 0644. No real uid/gid/mode on Windows. */
    if (is_dir) {
        out->st_mode = HOSTFS_S_IFDIR | 0755;
    } else {
        out->st_mode = HOSTFS_S_IFREG | (readonly ? 0444 : 0644);
    }
    out->st_uid = 0;
    out->st_gid = 0;
    out->st_rdev = 0;
    out->st_size = (int64_t)(((uint64_t)info.nFileSizeHigh << 32) | (uint64_t)info.nFileSizeLow);
    out->st_blksize = 4096;
    out->st_blocks = (out->st_size + 511) / 512;
    filetime_to_unix(info.ftLastAccessTime, &out->st_atime_sec, &out->st_atime_nsec);
    filetime_to_unix(info.ftLastWriteTime, &out->st_mtime_sec, &out->st_mtime_nsec);
    filetime_to_unix(info.ftCreationTime, &out->st_ctime_sec, &out->st_ctime_nsec);
    return 0;
}

static int64_t win_getdents(void *handle, void *buf, uint64_t count) {
    win_handle *wh = (win_handle *)handle;
    if (!wh->is_dir) {
        return -(int64_t)HOSTFS_ENOTDIR;
    }
    if (wh->cursor >= wh->ndents) {
        return 0;
    }
    uint64_t off = 0;
    uint64_t k = wh->cursor;
    for (; k < wh->ndents; ++k) {
        if (!hostfs_emit_dirent((uint8_t *)buf, count, &off,
                                k + 1, (int64_t)(k + 1),
                                wh->dents[k].type, wh->dents[k].name)) {
            break;
        }
    }
    if (off == 0) {
        return -(int64_t)HOSTFS_EINVAL;
    }
    wh->cursor = k;
    return (int64_t)off;
}

/* maize-151 confinement for the path-mutating ops. A mutating op cannot open+canonicalize
   the target itself (mkdir's target does not exist yet; unlink/rename's may be about to
   vanish), so we confine the PARENT: split the (core-normalized) remainder at the last
   backslash, open+canonicalize the parent directory, reject a reparse point, and verify
   it is a prefix-child of the mount root exactly as win_confine does for open(). The
   returned target is canon_parent + "\\" + final-component; because the core's
   normalize_path already collapsed every "." / ".." / "//", the final component is a lone
   element that cannot traverse out of the verified parent. Caller frees *out. Returns 0
   or a negative errno. */
static int64_t win_confine_target(win_anchor *anc, const char *rel, wchar_t **out) {
    *out = NULL;

    wchar_t *wrel = widen(rel);
    if (!wrel) {
        return -(int64_t)HOSTFS_ENOMEM;
    }
    for (wchar_t *p = wrel; *p; ++p) {
        if (*p == L'/') {
            *p = L'\\';
        }
    }

    /* Split at the last backslash into parent-rel + final component. A lone component
       (or "." for the mount root itself) leaves the parent as the mount root. */
    wchar_t *slash = wcsrchr(wrel, L'\\');
    const wchar_t *final;
    const wchar_t *parent_rel;
    if (slash) {
        *slash = L'\0';
        parent_rel = wrel;
        final = slash + 1;
    } else {
        parent_rel = L"";
        final = wrel;
    }

    /* Build the parent wide path: canon_root [ + "\\" + parent_rel ]. */
    size_t rootl = wcslen(anc->canon_root);
    size_t prl = wcslen(parent_rel);
    wchar_t *ptarget = (wchar_t *)malloc((rootl + prl + 2) * sizeof(wchar_t));
    if (!ptarget) {
        free(wrel);
        return -(int64_t)HOSTFS_ENOMEM;
    }
    memcpy(ptarget, anc->canon_root, rootl * sizeof(wchar_t));
    size_t pl = rootl;
    if (prl) {
        ptarget[pl++] = L'\\';
        memcpy(ptarget + pl, parent_rel, prl * sizeof(wchar_t));
        pl += prl;
    }
    ptarget[pl] = L'\0';

    /* Open + reparse-check + canonicalize + prefix-verify the parent against the root. */
    HANDLE ph = CreateFileW(ptarget, GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            NULL, OPEN_EXISTING,
                            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    free(ptarget);
    if (ph == INVALID_HANDLE_VALUE) {
        free(wrel);
        return map_last_error(GetLastError());
    }
    BY_HANDLE_FILE_INFORMATION info;
    if (!GetFileInformationByHandle(ph, &info)) {
        CloseHandle(ph);
        free(wrel);
        return -(int64_t)HOSTFS_EIO;
    }
    if (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        CloseHandle(ph);
        free(wrel);
        return -(int64_t)HOSTFS_EACCES;
    }
    wchar_t *canon_parent = canon_of(ph);
    CloseHandle(ph);
    if (!canon_parent) {
        free(wrel);
        return -(int64_t)HOSTFS_EIO;
    }
    if (!is_beneath(anc->canon_root, canon_parent)) {
        free(canon_parent);
        free(wrel);
        return -(int64_t)HOSTFS_EACCES;
    }

    /* Compose canon_parent + "\\" + final component. */
    size_t cpl = wcslen(canon_parent);
    size_t fl = wcslen(final);
    wchar_t *target = (wchar_t *)malloc((cpl + fl + 2) * sizeof(wchar_t));
    if (!target) {
        free(canon_parent);
        free(wrel);
        return -(int64_t)HOSTFS_ENOMEM;
    }
    memcpy(target, canon_parent, cpl * sizeof(wchar_t));
    size_t tl = cpl;
    target[tl++] = L'\\';
    memcpy(target + tl, final, fl * sizeof(wchar_t));
    tl += fl;
    target[tl] = L'\0';

    free(canon_parent);
    free(wrel);
    *out = target;
    return 0;
}

/* Out-of-scope ops (doc section 8): not guest-reachable in this POC. */
static int64_t win_open(hostfs_mount *m, const char *p, int fl, int mode) {
    (void)m; (void)p; (void)fl; (void)mode; return -(int64_t)HOSTFS_ENOSYS;
}
static int64_t win_stat(hostfs_mount *m, const char *p, hostfs_stat *o) {
    (void)m; (void)p; (void)o; return -(int64_t)HOSTFS_ENOSYS;
}

/* maize-151 path-mutating ops (confined via win_confine_target above). The core has
   already applied the :ro / synthetic-root write-gate before dispatching here. */
static int64_t win_mkdir(hostfs_mount *m, const char *p, int mode) {
    (void)mode;   /* Windows has no POSIX mode bits on a directory. */
    win_anchor *anc = (win_anchor *)m->anchor;
    wchar_t *target = NULL;
    int64_t rc = win_confine_target(anc, p, &target);
    if (rc < 0) {
        return rc;
    }
    BOOL ok = CreateDirectoryW(target, NULL);
    DWORD e = GetLastError();
    free(target);
    if (!ok) {
        return map_last_error(e);
    }
    return 0;
}
static int64_t win_rmdir(hostfs_mount *m, const char *p) {
    win_anchor *anc = (win_anchor *)m->anchor;
    wchar_t *target = NULL;
    int64_t rc = win_confine_target(anc, p, &target);
    if (rc < 0) {
        return rc;
    }
    BOOL ok = RemoveDirectoryW(target);
    DWORD e = GetLastError();
    free(target);
    if (!ok) {
        return map_last_error(e);
    }
    return 0;
}
static int64_t win_unlink(hostfs_mount *m, const char *p) {
    win_anchor *anc = (win_anchor *)m->anchor;
    wchar_t *target = NULL;
    int64_t rc = win_confine_target(anc, p, &target);
    if (rc < 0) {
        return rc;
    }
    BOOL ok = DeleteFileW(target);
    DWORD e = GetLastError();
    free(target);
    if (!ok) {
        return map_last_error(e);
    }
    return 0;
}
static int64_t win_rename(hostfs_mount *m, const char *o, const char *n) {
    win_anchor *anc = (win_anchor *)m->anchor;
    wchar_t *otarget = NULL;
    wchar_t *ntarget = NULL;
    int64_t rc = win_confine_target(anc, o, &otarget);
    if (rc < 0) {
        return rc;
    }
    rc = win_confine_target(anc, n, &ntarget);
    if (rc < 0) {
        free(otarget);
        return rc;
    }
    BOOL ok = MoveFileExW(otarget, ntarget, MOVEFILE_REPLACE_EXISTING);
    DWORD e = GetLastError();
    free(otarget);
    free(ntarget);
    if (!ok) {
        return map_last_error(e);
    }
    return 0;
}

static const hostfs_backend_ops g_win_ops = {
    win_confine,
    win_open,
    win_close,
    win_read,
    win_write,
    win_lseek,
    win_stat,
    win_fstat,
    win_getdents,
    win_mkdir,
    win_rmdir,
    win_unlink,
    win_rename,
};

const hostfs_backend_ops *hostfs_backend_ops_get(void) {
    return &g_win_ops;
}

int64_t hostfs_backend_anchor_open(hostfs_mount *mount) {
    wchar_t *wroot = widen(mount->host_root);
    if (!wroot) {
        return -(int64_t)HOSTFS_ENOMEM;
    }
    HANDLE root = CreateFileW(wroot, GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              NULL, OPEN_EXISTING,
                              FILE_FLAG_BACKUP_SEMANTICS, NULL);
    free(wroot);
    if (root == INVALID_HANDLE_VALUE) {
        return map_last_error(GetLastError());
    }
    wchar_t *canon = canon_of(root);
    if (!canon) {
        CloseHandle(root);
        return -(int64_t)HOSTFS_EIO;
    }
    win_anchor *anc = (win_anchor *)calloc(1, sizeof(win_anchor));
    if (!anc) {
        free(canon);
        CloseHandle(root);
        return -(int64_t)HOSTFS_ENOMEM;
    }
    anc->root = root;
    anc->canon_root = canon;
    mount->anchor = anc;
    return 0;
}

#endif /* _WIN32 */
