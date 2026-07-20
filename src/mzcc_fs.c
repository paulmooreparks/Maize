/* mzcc_fs.c (maize-280): in-process directory-tree primitives for the batch
   subcommands. Replaces the shell's `cp -a`, `find -exec cp`, and list-file read
   loop with native code, so no external cp/find/rsync is ever spawned (decision
   DI 9619). The directory-enumeration primitive (scan_dir) is the one place the
   POSIX opendir/readdir vs Win32 FindFirstFile split lives, mirroring the
   mzcc_proc core/posix/win32 seam (decision D7). */
#include "mzcc_fs.h"
#include "mzcc_internal.h"
#include "mzcc_proc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dirent.h>
#include <sys/types.h>
#endif

/* ---- directory enumeration --------------------------------------------- */

typedef struct {
    char *name;
    int   is_dir;
} FsEnt;

typedef struct {
    FsEnt *v;
    int    n;
    int    cap;
} FsEntList;

static void fsl_init(FsEntList *l) {
    l->v = NULL;
    l->n = 0;
    l->cap = 0;
}

static void fsl_push(FsEntList *l, const char *name, int is_dir) {
    if (l->n + 1 > l->cap) {
        l->cap = l->cap ? l->cap * 2 : 16;
        l->v = (FsEnt *)realloc(l->v, (size_t)l->cap * sizeof(FsEnt));
        if (!l->v) { die("out of memory"); }
    }
    l->v[l->n].name = xstrdup(name);
    l->v[l->n].is_dir = is_dir;
    l->n++;
}

static void fsl_free(FsEntList *l) {
    for (int i = 0; i < l->n; ++i) {
        free(l->v[i].name);
    }
    free(l->v);
    fsl_init(l);
}

/* Enumerate the entries of `dir` (excluding "." and ".."), pushing each name and
   its is-directory flag into `out`. Returns 0 on success, -1 if `dir` cannot be
   opened. */
static int scan_dir(const char *dir, FsEntList *out) {
#if defined(_WIN32)
    char *pat = joinstr(dir, "/*", NULL, NULL);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    free(pat);
    if (h == INVALID_HANDLE_VALUE) {
        return -1;
    }
    do {
        const char *name = fd.cFileName;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
            continue;
        }
        int is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
        fsl_push(out, name, is_dir);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return 0;
#else
    DIR *d = opendir(dir);
    if (!d) {
        return -1;
    }
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        const char *name = de->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
            continue;
        }
        /* Determine directoriness by stat on the full path: d_type is DT_UNKNOWN
           on some filesystems, and stat is the portable answer. */
        char *full = path_join(dir, name);
        struct stat st;
        int is_dir = (stat(full, &st) == 0) && ((st.st_mode & S_IFMT) == S_IFDIR);
        free(full);
        fsl_push(out, name, is_dir);
    }
    closedir(d);
    return 0;
#endif
}

/* ---- public primitives ------------------------------------------------- */

int copy_tree(const char *src, const char *dst) {
    mkdir_p(dst);
    FsEntList ents;
    fsl_init(&ents);
    if (scan_dir(src, &ents) != 0) {
        fsl_free(&ents);
        fprintf(stderr, "mzcc: cannot read directory %s\n", src);
        return -1;
    }
    int rc = 0;
    for (int i = 0; i < ents.n && rc == 0; ++i) {
        char *s = path_join(src, ents.v[i].name);
        char *d = path_join(dst, ents.v[i].name);
        if (ents.v[i].is_dir) {
            rc = copy_tree(s, d);
        } else {
            if (copy_file(s, d) != 0) {
                fprintf(stderr, "mzcc: cannot copy %s -> %s\n", s, d);
                rc = -1;
            }
        }
        free(s);
        free(d);
    }
    fsl_free(&ents);
    return rc;
}

int copy_file_into_every_subdir(const char *header_path, const char *root_dir) {
    /* Basename of the header (paths are slash-normalized by callers). */
    const char *slash = strrchr(header_path, '/');
    const char *leaf = slash ? slash + 1 : header_path;

    /* Copy into this directory first, then recurse into its subdirectories, so
       every directory at every depth receives a copy (the flat effect of
       `find <root> -type d -exec cp <header> {} \;`). */
    char *here = path_join(root_dir, leaf);
    if (copy_file(header_path, here) != 0) {
        fprintf(stderr, "mzcc: cannot copy %s -> %s\n", header_path, here);
        free(here);
        return -1;
    }
    free(here);

    FsEntList ents;
    fsl_init(&ents);
    if (scan_dir(root_dir, &ents) != 0) {
        fsl_free(&ents);
        fprintf(stderr, "mzcc: cannot read directory %s\n", root_dir);
        return -1;
    }
    int rc = 0;
    for (int i = 0; i < ents.n && rc == 0; ++i) {
        if (!ents.v[i].is_dir) {
            continue;
        }
        char *sub = path_join(root_dir, ents.v[i].name);
        rc = copy_file_into_every_subdir(header_path, sub);
        free(sub);
    }
    fsl_free(&ents);
    return rc;
}

int read_list_file(const char *path, StrList *out) {
    ByteBuf b;
    if (read_file(path, &b) != 0) {
        byte_buf_free(&b);
        return -1;
    }
    size_t p = 0;
    while (p < b.len) {
        size_t q = p;
        while (q < b.len && b.data[q] != '\n') { ++q; }
        /* line b[p..q), with every CR byte stripped. */
        ByteBuf line;
        byte_buf_init(&line);
        for (size_t k = p; k < q; ++k) {
            if (b.data[k] != '\r') { byte_buf_append(&line, &b.data[k], 1); }
        }
        byte_buf_append(&line, "\0", 1);
        const char *entry = line.data;
        if (entry[0] != '\0' && entry[0] != '#') {
            sl_push(out, entry);
        }
        byte_buf_free(&line);
        p = (q < b.len) ? q + 1 : q;
    }
    byte_buf_free(&b);
    return 0;
}

int dir_exists(const char *path) {
    struct stat st;
    return (stat(path, &st) == 0) && ((st.st_mode & S_IFMT) == S_IFDIR);
}

int list_dir(const char *dir, StrList *out) {
    FsEntList ents;
    fsl_init(&ents);
    if (scan_dir(dir, &ents) != 0) {
        fsl_free(&ents);
        return -1;
    }
    for (int i = 0; i < ents.n; ++i) {
        sl_push(out, ents.v[i].name);
    }
    fsl_free(&ents);
    return 0;
}
