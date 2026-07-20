/* mzcc_internal.h (maize-280): the reusable build core mzcc.c exposes so the
   batch subcommands (build-userland / build-demos / build-quesos) drive the SAME
   per-TU pipeline as the single-file compile path rather than re-implementing it
   or shelling back into mzcc (decision DI 9617).

   Everything here is defined in mzcc.c; the subcommand TUs (mzcc_userland.c,
   mzcc_demos.c, mzcc_quesos.c) and the filesystem helpers (mzcc_fs.c) call in
   through this header. The single-file compile path in main() is a peer caller of
   the same functions, so extracting them changed no observable output (the
   maize-278 byte-parity gate still holds).

   The link-profile "mechanism" is deliberately NOT a registry (decision DI 9616):
   a profile is just (optional base address, explicit object list) handed to
   mzld_link. build_default_c_image supplies the default C profile's object list
   (RT asm + libc + body); mzcc_quesos.c supplies its own 3-object minimal list.
   A third profile needs a new object-list builder, not a new pipeline. */
#ifndef MZCC_INTERNAL_H
#define MZCC_INTERNAL_H

#include <stddef.h>

#include "mzcc_proc.h" /* ByteBuf */

/* ---- growable argv builder (NULL-terminated at v[n]) -------------------- */
typedef struct {
    char **v;
    int    n;
    int    cap;
} Argv;

void av_init(Argv *a);
void av_add(Argv *a, const char *s);
void av_free(Argv *a);

/* ---- growable string list ---------------------------------------------- */
typedef struct {
    char **v;
    int    n;
    int    cap;
} StrList;

void sl_init(StrList *s);
void sl_push(StrList *s, const char *item);
void sl_free(StrList *s);

/* ---- one batch build's per-invocation shape (a single cc-maize.sh call's
   worth of inputs). extra_defines are extra cpp -D tokens applied to EVERY
   compile in the build (RT libc modules and the user sources alike), exactly as
   cc-maize.sh applies its command-line -D flags globally. */
typedef struct {
    StrList sources;       /* ordered source paths */
    int     used_sources;  /* nonzero when any entry came from a --sources listfile */
    int     dev;           /* link the mzdev RT module */
    Argv    extra_defines; /* per-build cpp -D tokens, e.g. {"-D","EMACS","-D","volatile="} */
} BuildSpec;

/* ---- small utilities (defined in mzcc.c; shared across TUs) ------------- */
#if defined(__GNUC__)
void die(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
#else
void die(const char *fmt, ...);
#endif
void  *xmalloc(size_t n);
char  *xstrdup(const char *s);
char  *joinstr(const char *a, const char *b, const char *c, const char *d);
char  *path_join(const char *dir, const char *leaf);
char  *dir_of(const char *path);
void   to_slashes(char *s);
void   mkdir_p(const char *path);
int    path_exists(const char *p);
int    is_regular_file(const char *p);
int    read_file(const char *path, ByteBuf *out);
int    write_file(const char *path, const char *data, size_t len);
int    copy_file(const char *src, const char *dst);
int    which_exists(const char *name); /* PATH existence probe */

/* ---- driver-wide resolved state (defined in mzcc.c) -------------------- */
extern char *REPO_ROOT;
extern char *RT_DIR;

/* Resolve REPO_ROOT + RT_DIR once (idempotent), so a subcommand can build
   repo-relative source paths and stat submodule directories before resolving
   the toolchain. MAIZE_ROOT override, else self-location from the mzcc exe. */
void ensure_repo_root(void);

/* ---- the reusable build core ------------------------------------------- */

/* Resolve CPP/CPROC_QBE/QBE/MAZM/MAIZE/MZLD/BUILD_DIR (and REPO_ROOT/RT_DIR)
   for `preset`, exactly as main()'s tool-discovery block does. Prints the same
   actionable messages and returns nonzero (usage/setup class, exit-2) on any
   tool-not-found; 0 on success. Idempotent and safe to call repeatedly. */
int resolve_toolchain(const char *preset);

/* Create the compile scratch dirs (OBJ_DIR / CPP_CWD) once, idempotently, and
   register their exit-time cleanup. A subcommand that drives the low-level
   pipeline helpers directly (mzcc_quesos.c) calls this after resolve_toolchain;
   build_default_c_image calls it internally. */
void ensure_scratch(void);

/* MZX-magic + minimum-size validation (the verify_mzx equivalent). Returns 0
   when `path` is a well-formed .mzx image, 1 otherwise (message printed). */
int verify_mzx_image(const char *path);

/* Compile one C translation unit through the full pipeline (cpp -> cproc-qbe ->
   normalize -> qbe -> mazm), applying `extra_defines` (may be NULL) in addition
   to the process-global EXTRA_CPPDEFS. `emit_body` (may be NULL) receives the
   qbe body bytes. Returns a fresh .mzo path, or NULL on failure. */
char *compile_tu_ex(const char *src, const char *tag, const Argv *extra_defines,
                    ByteBuf *emit_body);

/* Assemble a .mazm file at `mazm_path` to a .mzo tagged `tag` via the mazm
   stdin-to-object path. Returns a fresh .mzo path, or NULL. */
char *assemble_mazm_file(const char *mazm_path, const char *tag);

/* Link `objects[0..n_objects)` (already-built .mzo paths, in link order) to
   `out_path`. `base_hex` NULL uses mzld's default base; non-NULL is passed as
   `mzld -b <base_hex>`. This IS the whole link-profile mechanism. Returns 0 on
   success, 1 on failure (message printed). */
int mzld_link(const char *base_hex, const char *out_path, char **objects, int n_objects);

/* Resolve tools for `preset`, compile every source in `spec` through the default
   C link profile (RT asm + libc + body, entry _start, default base), link, and
   copy the result to `out_path`. Never runs the produced image. Returns 0 on
   success, 1 on a compile/link failure, 2 on a toolchain-resolve failure
   (matching mzcc's exit-code convention). */
int build_default_c_image(const char *preset, const BuildSpec *spec, const char *out_path);

/* Per-platform default RELEASE preset the batch subcommands select when --preset
   is omitted (linux-release / macos-debug / windows-llvm-mingw-release), matching
   each .sh script's own `case "$UNAME"` block. NOT mzcc's single-file debug
   default. Returns a static string. */
const char *default_batch_preset(void);

/* ---- subcommand entry points (defined in their own TUs) ---------------- */
int cmd_build_userland(int argc, char **argv);
int cmd_build_demos(int argc, char **argv);
int cmd_build_quesos(int argc, char **argv);

#endif /* MZCC_INTERNAL_H */
