/* mzcc_fs.h (maize-280): in-process filesystem-tree primitives the batch
   subcommands use in place of the shell's `cp -a`, `find -exec cp`, and the
   list-file `read` loop. No external cp/find/rsync is spawned anywhere (decision
   DI 9619); the POSIX opendir/readdir vs Win32 FindFirstFile split lives in
   mzcc_fs.c, mirroring the mzcc_proc core/posix/win32 seam. */
#ifndef MZCC_FS_H
#define MZCC_FS_H

#include "mzcc_internal.h" /* StrList */

/* Recursive directory copy: copy the CONTENTS of `src` into `dst`, creating
   `dst` (and intermediate components) as needed and MERGING into any existing
   destination subtree (later entries overwrite). This covers both the shell's
   `cp -a <sub> <stage>` (fresh dst) and `cp -a <overlay>/. <stage>/` (merge onto
   an existing tree). Returns 0 on success, -1 on any copy failure. */
int copy_tree(const char *src, const char *dst);

/* Copy the single file `header_path` into `root_dir` and into every directory
   beneath it (at every depth), replacing the shell's
   `find <root> -type d -exec cp <header> {} \;`. Returns 0/-1. */
int copy_file_into_every_subdir(const char *header_path, const char *root_dir);

/* Parse a list file (one path per line; blank lines and lines beginning with '#'
   skipped; every CR byte stripped) into `out` (already sl_init'd). The same
   parser the --sources listfiles and the userland/oksh .list files use. Returns
   0 on success, -1 if the file cannot be read. */
int read_list_file(const char *path, StrList *out);

/* Nonzero when `path` exists and is a directory. */
int dir_exists(const char *path);

/* List the entry names of `dir` (files and subdirectories, excluding "." and
   "..") into `out` (already sl_init'd), in the platform's readdir order (the
   caller sorts if it needs a stable order). Returns 0 on success, -1 if `dir`
   cannot be opened. */
int list_dir(const char *dir, StrList *out);

#endif /* MZCC_FS_H */
