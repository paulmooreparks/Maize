/* mzcc.c (maize-278): the compiled, cross-platform C guest-build driver. A
   native replacement for the ctest / one-shot compile path of
   scripts/cc-maize.sh: it reproduces that script's full CLI mode matrix, runs
   the preprocess -> compile -> normalize -> assemble chain as an in-memory
   pipeline spawning only compiled binaries, and carries the runtime-object
   enumeration and link order as the single point of truth.

   The parity gate is byte-level: the final linked .mzx image must be identical
   to cc-maize.sh's on the same inputs, preset, and host (see the card's section
   12). Every stage flag, the normalize transform, the RT object set, and the
   mzld link order are reproduced from scripts/cc-maize.sh at HEAD; when in
   doubt this driver matches that script byte-for-byte.

   Language is C (C11), not C++ (decision DI10): cproc compiles C only, so
   mzcc-in-C is the prerequisite for maize-277 self-hosting the driver as a
   guest program, and C links only libc (a smaller dependency surface).

   The whole cygpath / native_path / wslpath marshalling layer cc-maize.sh
   needs is DELETED, not ported (design D6): with no shell splitting argv across
   a shell/native boundary, the boundary does not exist. Native paths pass to
   native exes directly, as discrete argv entries. */
#include "mzcc_proc.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#if defined(_WIN32)
#include <direct.h>
#define MZCC_MKDIR(p) _mkdir(p)
#define MZCC_GETCWD(buf, n) _getcwd((buf), (n))
#define PATH_SEP_LIST ";"
#else
#include <sys/types.h>
#include <unistd.h>
#define MZCC_MKDIR(p) mkdir((p), 0777)
#define MZCC_GETCWD(buf, n) getcwd((buf), (n))
#define PATH_SEP_LIST ":"
#endif

#if defined(_WIN32)
static const char *DEFAULT_PRESET = "windows-llvm-mingw-debug";
static const int IS_WINDOWS = 1;
#elif defined(__APPLE__)
static const char *DEFAULT_PRESET = "macos-debug";
static const int IS_WINDOWS = 0;
#else
static const char *DEFAULT_PRESET = "linux-debug";
static const int IS_WINDOWS = 0;
#endif

/* ---- small utilities ---------------------------------------------------- */

static void die(const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "mzcc: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    exit(2); /* usage / environment failure, matching cc-maize.sh */
}

static void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) {
        die("out of memory");
    }
    return p;
}

static char *xstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = (char *)xmalloc(n);
    memcpy(p, s, n);
    return p;
}

/* Concatenate up to 4 fragments into a fresh allocation. */
static char *joinstr(const char *a, const char *b, const char *c, const char *d) {
    size_t la = a ? strlen(a) : 0;
    size_t lb = b ? strlen(b) : 0;
    size_t lc = c ? strlen(c) : 0;
    size_t ld = d ? strlen(d) : 0;
    char *r = (char *)xmalloc(la + lb + lc + ld + 1);
    char *p = r;
    if (a) { memcpy(p, a, la); p += la; }
    if (b) { memcpy(p, b, lb); p += lb; }
    if (c) { memcpy(p, c, lc); p += lc; }
    if (d) { memcpy(p, d, ld); p += ld; }
    *p = '\0';
    return r;
}

static char *path_join(const char *dir, const char *leaf) {
    return joinstr(dir, "/", leaf, NULL);
}

static int is_regular_file(const char *p) {
    struct stat st;
    if (stat(p, &st) != 0) {
        return 0;
    }
    return (st.st_mode & S_IFMT) == S_IFREG;
}

static int path_exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0;
}

/* Normalize backslashes to forward slashes in place (Windows GetModuleFileNameA
   yields backslashes; every native tool and clang accepts forward slashes, and
   paths never enter the .mzx image, so this is cosmetic uniformity only). */
static void to_slashes(char *s) {
    for (; *s; ++s) {
        if (*s == '\\') {
            *s = '/';
        }
    }
}

/* Remove the trailing "/component" from a path in place. */
static void strip_last_component(char *s) {
    char *slash = strrchr(s, '/');
    if (slash) {
        *slash = '\0';
    }
}

/* dirname as a fresh allocation; "." when there is no slash. */
static char *dir_of(const char *path) {
    char *copy = xstrdup(path);
    to_slashes(copy);
    char *slash = strrchr(copy, '/');
    if (!slash) {
        free(copy);
        return xstrdup(".");
    }
    if (slash == copy) {
        slash[1] = '\0'; /* keep the root "/" */
    } else {
        *slash = '\0';
    }
    return copy;
}

/* Is `path` absolute? POSIX: starts with '/'. Windows additionally accepts a
   drive-letter form ("C:/..." or "C:\\...", already slash-normalized by the
   callers below). */
static int is_abs_path(const char *path) {
    if (path[0] == '/') {
        return 1;
    }
#if defined(_WIN32)
    if (((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) &&
        path[1] == ':' && (path[2] == '/' || path[2] == '\\')) {
        return 1;
    }
#endif
    return 0;
}

/* Absolute path of the process's real working directory, as a fresh
   allocation with forward slashes. The mzcc process itself never chdir's
   (only spawned children do, via run_proc's `cwd`), so this is safe to call
   at any point and always reflects the caller's real location. */
static char *get_real_cwd(void) {
    char buf[4096];
    if (!MZCC_GETCWD(buf, sizeof(buf))) {
        die("cannot determine the current working directory");
    }
    to_slashes(buf);
    return xstrdup(buf);
}

/* dir_of(), absolutized against the process's REAL working directory (review
   #3052 finding 1). compile_tu spawns cpp with cwd = the empty CPP_CWD
   scratch dir (DI6) so a source given by a RELATIVE path resolves its own
   `-I <source-dir>` against that scratch dir instead of the caller's real
   location, breaking a sibling #include ("sibling.h") that cc-maize.sh (which
   runs cpp in the caller's real CWD) resolves fine. Absolutizing here, before
   cpp is spawned, keeps the two drivers byte-identical on relative-path
   sources with sibling headers. Returns a fresh allocation. */
static char *abs_dir_of(const char *src) {
    char *d = dir_of(src);
    if (is_abs_path(d)) {
        return d;
    }
    char *cwd = get_real_cwd();
    char *abs = (strcmp(d, ".") == 0) ? xstrdup(cwd) : joinstr(cwd, "/", d, NULL);
    free(cwd);
    free(d);
    return abs;
}

/* basename with a trailing ".c" stripped, as a fresh allocation. */
static char *base_noext_c(const char *path) {
    char *copy = xstrdup(path);
    to_slashes(copy);
    char *slash = strrchr(copy, '/');
    char *base = slash ? slash + 1 : copy;
    char *r = xstrdup(base);
    free(copy);
    size_t n = strlen(r);
    if (n >= 2 && r[n - 2] == '.' && r[n - 1] == 'c') {
        r[n - 2] = '\0';
    }
    return r;
}

/* Resolve an executable path, tolerating a .exe suffix (reproduces
   cc-maize.sh:285-297). Host-aware: on Windows try <path>.exe then <path>;
   elsewhere try <path> then <path>.exe. Returns a fresh allocation, or NULL. */
static char *resolve_exe(const char *base) {
    char *exe = joinstr(base, ".exe", NULL, NULL);
    if (IS_WINDOWS) {
        if (path_exists(exe)) { return exe; }
        free(exe);
        if (path_exists(base)) { return xstrdup(base); }
    } else {
        if (path_exists(base)) { free(exe); return xstrdup(base); }
        if (path_exists(exe)) { return exe; }
        free(exe);
    }
    return NULL;
}

/* Is `name` resolvable on PATH? (existence probe only; the bare name is then
   handed to run_proc, whose execvp/CreateProcess re-resolves it.) */
static int which_exists(const char *name) {
    const char *path = getenv("PATH");
    if (!path || !path[0]) {
        return 0;
    }
    char *copy = xstrdup(path);
    int found = 0;
    /* Plain strtok: PATH parsing here is single-threaded and non-reentrant use
       is safe (no nested tokenization), and strtok is available on every host. */
    char *tok = strtok(copy, PATH_SEP_LIST);
    for (; tok; tok = strtok(NULL, PATH_SEP_LIST)) {
        char *cand = path_join(tok, name);
        if (path_exists(cand)) { free(cand); found = 1; break; }
        free(cand);
        if (IS_WINDOWS) {
            char *cexe = joinstr(tok, "/", name, ".exe");
            if (path_exists(cexe)) { free(cexe); found = 1; break; }
            free(cexe);
        }
    }
    free(copy);
    return found;
}

static int read_file(const char *path, ByteBuf *out) {
    byte_buf_init(out);
    FILE *f = fopen(path, "rb");
    if (!f) {
        return -1;
    }
    char tmp[65536];
    size_t got;
    while ((got = fread(tmp, 1, sizeof(tmp), f)) > 0) {
        if (byte_buf_append(out, tmp, got) != 0) {
            fclose(f);
            return -1;
        }
    }
    fclose(f);
    return 0;
}

static int write_file(const char *path, const char *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        return -1;
    }
    if (len > 0 && fwrite(data, 1, len, f) != len) {
        fclose(f);
        return -1;
    }
    fclose(f);
    return 0;
}

static int copy_file(const char *src, const char *dst) {
    ByteBuf b;
    if (read_file(src, &b) != 0) {
        byte_buf_free(&b);
        return -1;
    }
    int rc = write_file(dst, b.data, b.len);
    byte_buf_free(&b);
    return rc;
}

/* mkdir -p: create every missing component of `path`. */
static void mkdir_p(const char *path) {
    char *copy = xstrdup(path);
    to_slashes(copy);
    for (char *p = copy + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            MZCC_MKDIR(copy);
            *p = '/';
        }
    }
    MZCC_MKDIR(copy);
    free(copy);
}

/* ---- growable argv builder --------------------------------------------- */

typedef struct {
    char **v;
    int    n;
    int    cap;
} Argv;

static void av_init(Argv *a) {
    a->v = NULL;
    a->n = 0;
    a->cap = 0;
}

static void av_add(Argv *a, const char *s) {
    if (a->n + 2 > a->cap) {
        a->cap = a->cap ? a->cap * 2 : 8;
        a->v = (char **)realloc(a->v, (size_t)a->cap * sizeof(char *));
        if (!a->v) { die("out of memory"); }
    }
    a->v[a->n++] = xstrdup(s);
    a->v[a->n] = NULL;
}

static void av_free(Argv *a) {
    for (int i = 0; i < a->n; ++i) {
        free(a->v[i]);
    }
    free(a->v);
    av_init(a);
}

/* ---- growable string list ---------------------------------------------- */

typedef struct {
    char **v;
    int    n;
    int    cap;
} StrList;

static void sl_init(StrList *s) {
    s->v = NULL;
    s->n = 0;
    s->cap = 0;
}

static void sl_push(StrList *s, const char *item) {
    if (s->n + 1 > s->cap) {
        s->cap = s->cap ? s->cap * 2 : 8;
        s->v = (char **)realloc(s->v, (size_t)s->cap * sizeof(char *));
        if (!s->v) { die("out of memory"); }
    }
    s->v[s->n++] = xstrdup(item);
}

/* ---- in-process transforms (formerly shell / sed) ----------------------- */

/* 3a. CRLF strip (replaces `tr -d '\r'`): copy dropping every 0x0D byte. */
static void crlf_strip(const char *in, size_t n, ByteBuf *out) {
    byte_buf_init(out);
    size_t start = 0;
    for (size_t i = 0; i < n; ++i) {
        if (in[i] == '\r') {
            if (i > start) {
                byte_buf_append(out, in + start, i - start);
            }
            start = i + 1;
        }
    }
    if (n > start) {
        byte_buf_append(out, in + start, n - start);
    }
}

static const char *mem_find(const char *h, size_t hlen, const char *needle, size_t nlen) {
    if (nlen == 0 || hlen < nlen) {
        return NULL;
    }
    for (size_t i = 0; i + nlen <= hlen; ++i) {
        if (h[i] == needle[0] && memcmp(h + i, needle, nlen) == 0) {
            return h + i;
        }
    }
    return NULL;
}

/* Append `s` to `dst`, replacing every occurrence of `from` with `to`. */
static void replace_all_into(ByteBuf *dst, const char *s, size_t len,
                             const char *from, size_t flen,
                             const char *to, size_t tlen) {
    size_t i = 0;
    while (i < len) {
        const char *hit = mem_find(s + i, len - i, from, flen);
        if (!hit) {
            byte_buf_append(dst, s + i, len - i);
            break;
        }
        size_t pre = (size_t)(hit - (s + i));
        byte_buf_append(dst, s + i, pre);
        byte_buf_append(dst, to, tlen);
        i += pre + flen;
    }
}

/* 3b. The ONE normalize (replaces the sed at cc-maize.sh:456-457), reproduced
   BYTE-EXACT line by line (sed is line-oriented) with hand-rolled substring
   replacement rather than a regex engine, to avoid any locale/format
   nondeterminism against the byte-identical parity gate:
     Rule 1  s/extern \$/$/g          : GLOBAL per line, "extern $" -> "$".
     Rule 2  s/\(=[wl]\) neg /\1 sub 0, /  : FIRST match per line only,
              "=w neg " -> "=w sub 0, " and "=l neg " -> "=l sub 0, "
              (only the w and l widths, exactly as the sed captures [wl]). */
static void normalize_buffer(const char *in, size_t n, ByteBuf *out) {
    byte_buf_init(out);
    size_t i = 0;
    while (i < n) {
        size_t j = i;
        while (j < n && in[j] != '\n') {
            ++j;
        }
        int had_nl = (j < n);
        const char *line = in + i;
        size_t llen = j - i;

        /* Rule 1 (global) into r1. */
        ByteBuf r1;
        byte_buf_init(&r1);
        replace_all_into(&r1, line, llen, "extern $", 8, "$", 1);

        /* Rule 2 (first match per line) on r1 into out. */
        const char *w = mem_find(r1.data, r1.len, "=w neg ", 7);
        const char *l = mem_find(r1.data, r1.len, "=l neg ", 7);
        const char *hit = NULL;
        const char *repl = NULL;
        if (w && (!l || w < l)) { hit = w; repl = "=w sub 0, "; }
        else if (l)            { hit = l; repl = "=l sub 0, "; }
        if (hit) {
            size_t pre = (size_t)(hit - r1.data);
            byte_buf_append(out, r1.data, pre);
            byte_buf_append(out, repl, 10);
            byte_buf_append(out, r1.data + pre + 7, r1.len - pre - 7);
        } else if (r1.len > 0) {
            byte_buf_append(out, r1.data, r1.len);
        }
        byte_buf_free(&r1);

        if (had_nl) {
            byte_buf_append(out, "\n", 1);
            i = j + 1;
        } else {
            break;
        }
    }
}

/* ---- driver-wide resolved state ---------------------------------------- */

static char *REPO_ROOT = NULL;
static char *BUILD_DIR = NULL;
static char *RT_DIR = NULL;
static char *OBJ_DIR = NULL;   /* scratch dir the .mzo objects land in */
static char *CPP_CWD = NULL;   /* empty scratch dir = cpp's quote-include base */
static char *SCRATCH_ROOT = NULL;

static char *CPP = NULL;
static char *CPROC_QBE = NULL;
static char *QBE = NULL;
static char *MAZM = NULL;
static char *MAIZE = NULL;
static char *MZLD = NULL;

static Argv EXTRA_CPPDEFS; /* -D tokens in command-line order (empty for every existing caller) */

static void cleanup_scratch(void) {
    if (SCRATCH_ROOT) {
        mzcc_remove_tree(SCRATCH_ROOT);
    }
}

/* ---- per-TU pipeline ---------------------------------------------------- */

/* Assemble a body (or an RT .mazm) read from `bytes` into <OBJ_DIR>/<tag>.mzo
   via the mazm stdin-to-object extension (7a): mazm -c --stdin --base-path
   <OBJ_DIR> --source-name <tag>. Returns a fresh path to the .mzo on success,
   NULL on failure (message already printed). */
static char *assemble_stdin(const char *bytes, size_t len, const char *tag) {
    Argv av;
    av_init(&av);
    av_add(&av, MAZM);
    av_add(&av, "-c");
    av_add(&av, "--stdin");
    av_add(&av, "--base-path");
    av_add(&av, OBJ_DIR);
    av_add(&av, "--source-name");
    av_add(&av, tag);

    ProcResult r;
    int ran = run_proc(MAZM, av.v, av.n, bytes, len, NULL, &r);
    av_free(&av);
    char *mzo = joinstr(OBJ_DIR, "/", tag, ".mzo");
    if (!ran || r.exit_code != 0 || !path_exists(mzo)) {
        fprintf(stderr, "mzcc: mazm -c failed for %s\n", tag);
        if (r.stderr_bytes.len) { fwrite(r.stderr_bytes.data, 1, r.stderr_bytes.len, stderr); }
        if (r.stdout_bytes.len) { fwrite(r.stdout_bytes.data, 1, r.stdout_bytes.len, stderr); }
        byte_buf_free(&r.stdout_bytes);
        byte_buf_free(&r.stderr_bytes);
        free(mzo);
        return NULL;
    }
    byte_buf_free(&r.stdout_bytes);
    byte_buf_free(&r.stderr_bytes);
    return mzo;
}

/* Compile one C translation unit through the full segmented pipeline to a .mzo:
     CRLF strip -> cpp -E ... -> cproc-qbe -> normalize -> qbe -t maize -> mazm -c
   Each stage's stdout is captured to an in-memory buffer and handed to the next
   stage's stdin (buffer-and-hand-off, decision DI3); the ONLY temp file is the
   emitted .mzo (7a). `src` is the source path, `tag` the object base name.
   When `emit_body` is non-NULL the qbe body bytes are copied into it (for the
   --emit .mazm drop). Returns a fresh path to the .mzo, or NULL on failure.

   This is the DI9 pure-function seam: the result is determined solely by the
   preprocessed bytes, the tool identities, and the flags; no shared mutable
   driver state. maize-274 wraps a content-addressed object cache and a work
   queue around this with zero pipeline rearchitecture. */
static char *compile_tu(const char *src, const char *tag, ByteBuf *emit_body) {
    ByteBuf raw;
    if (read_file(src, &raw) != 0) {
        fprintf(stderr, "mzcc: cannot read source %s\n", src);
        byte_buf_free(&raw);
        return NULL;
    }
    ByteBuf lf;
    crlf_strip(raw.data, raw.len, &lf);
    byte_buf_free(&raw);

    char *src_dir = abs_dir_of(src);

    /* Stage: cpp. Reproduces cc-maize.sh:442-449 EXACTLY (the maize-257
       host-canonicalization invariant); flags in this order, as discrete argv
       entries, fed the CRLF-stripped source via stdin (`-`) with cwd = the
       empty scratch dir so the implicit quote-include base matches
       cc-maize.sh's empty-WORK base (decision DI6). */
    Argv cpp;
    av_init(&cpp);
    av_add(&cpp, CPP);
    av_add(&cpp, "-E"); av_add(&cpp, "-P"); av_add(&cpp, "-nostdinc");
    av_add(&cpp, "-D"); av_add(&cpp, "__attribute__(x)=");
    av_add(&cpp, "-U"); av_add(&cpp, "WIN32");
    av_add(&cpp, "-U"); av_add(&cpp, "WIN64");
    av_add(&cpp, "-U"); av_add(&cpp, "_WIN32");
    av_add(&cpp, "-U"); av_add(&cpp, "_WIN64");
    av_add(&cpp, "-U"); av_add(&cpp, "__WIN32");
    av_add(&cpp, "-U"); av_add(&cpp, "__WIN32__");
    av_add(&cpp, "-U"); av_add(&cpp, "__WIN64");
    av_add(&cpp, "-U"); av_add(&cpp, "__WIN64__");
    av_add(&cpp, "-U"); av_add(&cpp, "__MINGW32__");
    av_add(&cpp, "-U"); av_add(&cpp, "__MINGW64__");
    av_add(&cpp, "-D"); av_add(&cpp, "__linux__=1");
    av_add(&cpp, "-D"); av_add(&cpp, "__gnu_linux__=1");
    for (int i = 0; i < EXTRA_CPPDEFS.n; ++i) {
        av_add(&cpp, EXTRA_CPPDEFS.v[i]);
    }
    av_add(&cpp, "-I"); av_add(&cpp, RT_DIR);
    av_add(&cpp, "-I"); av_add(&cpp, src_dir);
    av_add(&cpp, "-");

    ProcResult pp;
    int ran = run_proc(CPP, cpp.v, cpp.n, lf.data, lf.len, CPP_CWD, &pp);
    av_free(&cpp);
    byte_buf_free(&lf);
    free(src_dir);
    if (!ran || pp.exit_code != 0) {
        fprintf(stderr, "mzcc: cpp failed for %s\n", tag);
        if (pp.stderr_bytes.len) { fwrite(pp.stderr_bytes.data, 1, pp.stderr_bytes.len, stderr); }
        byte_buf_free(&pp.stdout_bytes);
        byte_buf_free(&pp.stderr_bytes);
        return NULL;
    }
    byte_buf_free(&pp.stderr_bytes);

    /* Stage: cproc-qbe (C11 -> QBE IL), stdin = cpp stdout. */
    Argv cq;
    av_init(&cq);
    av_add(&cq, CPROC_QBE);
    ProcResult ssa;
    ran = run_proc(CPROC_QBE, cq.v, cq.n, pp.stdout_bytes.data, pp.stdout_bytes.len, NULL, &ssa);
    av_free(&cq);
    byte_buf_free(&pp.stdout_bytes);
    if (!ran || ssa.exit_code != 0) {
        fprintf(stderr, "mzcc: cproc-qbe failed for %s\n", tag);
        if (ssa.stderr_bytes.len) { fwrite(ssa.stderr_bytes.data, 1, ssa.stderr_bytes.len, stderr); }
        byte_buf_free(&ssa.stdout_bytes);
        byte_buf_free(&ssa.stderr_bytes);
        return NULL;
    }
    byte_buf_free(&ssa.stderr_bytes);

    /* Normalize in-process (3b). */
    ByteBuf norm;
    normalize_buffer(ssa.stdout_bytes.data, ssa.stdout_bytes.len, &norm);
    byte_buf_free(&ssa.stdout_bytes);

    /* Stage: qbe -t maize - (IL -> mazm body), stdin = normalized IL. */
    Argv qb;
    av_init(&qb);
    av_add(&qb, QBE);
    av_add(&qb, "-t"); av_add(&qb, "maize");
    av_add(&qb, "-");
    ProcResult body;
    ran = run_proc(QBE, qb.v, qb.n, norm.data, norm.len, NULL, &body);
    av_free(&qb);
    byte_buf_free(&norm);
    if (!ran || body.exit_code != 0) {
        fprintf(stderr, "mzcc: qbe -t maize failed for %s\n", tag);
        if (body.stderr_bytes.len) { fwrite(body.stderr_bytes.data, 1, body.stderr_bytes.len, stderr); }
        byte_buf_free(&body.stdout_bytes);
        byte_buf_free(&body.stderr_bytes);
        return NULL;
    }
    byte_buf_free(&body.stderr_bytes);

    if (emit_body) {
        byte_buf_init(emit_body);
        byte_buf_append(emit_body, body.stdout_bytes.data, body.stdout_bytes.len);
    }

    /* Stage: mazm -c (body -> the ONE temp file, the .mzo). The ".body" infix
       mirrors cc-maize.sh's own "${_tag}.body.mzo" naming (compile_tu, sed
       456-457's neighbor at cc-maize.sh:374) and is load-bearing: without it a
       single-source user object shares OBJ_DIR with the bare-named RT asm
       objects (crt0.mzo/syscall.mzo/setjmp.mzo/mzdev.mzo), so a user source
       literally named crt0.c collides with the RT crt0 object (review #3052
       finding 2). RT_C libc objects (tag "rt_<name>") and multi-source user
       objects (tag "u<i>_<base>") already carry a distinguishing prefix, but
       apply the infix uniformly here so every compile_tu-produced object is
       namespaced away from the bare RT-asm set the same way cc-maize.sh keeps
       them apart. Object names are internal (mzld resolves by symbol, not
       filename), so this does not change the linked .mzx. */
    char *obj_tag = joinstr(tag, ".body", NULL, NULL);
    char *mzo = assemble_stdin(body.stdout_bytes.data, body.stdout_bytes.len, obj_tag);
    free(obj_tag);
    byte_buf_free(&body.stdout_bytes);
    return mzo;
}

/* Assemble a freestanding asm runtime module (crt0/syscall/setjmp/mzdev) to a
   .mzo. Read the .mazm and feed it through the same mazm stdin-to-object path
   (no copy into scratch needed, cc-maize.sh:479-488). Returns a fresh .mzo
   path, or NULL. */
static char *assemble_rt_asm(const char *name) {
    char *src = joinstr(RT_DIR, "/", name, ".mazm");
    ByteBuf b;
    if (read_file(src, &b) != 0) {
        fprintf(stderr, "mzcc: cannot read runtime asm %s\n", src);
        byte_buf_free(&b);
        free(src);
        return NULL;
    }
    free(src);
    char *mzo = assemble_stdin(b.data, b.len, name);
    byte_buf_free(&b);
    return mzo;
}

/* ---- CLI --------------------------------------------------------------- */

typedef struct {
    int      mode_build;
    int      run;
    int      emit;
    int      dev;
    char    *out;
    char    *preset;
    StrList  pos_srcs;
    StrList  srcfiles;
    int      used_sources;
} Options;

static const char *USAGE =
    "usage: mzcc [--preset <name>] [-r|--run] [--emit] [-o <path>] <file.c>\n"
    "       mzcc [--preset <name>] [-r|--run] -o <path> <a.c> <b.c> [<c.c> ...]\n"
    "       mzcc [--preset <name>] [-r|--run] -o <path> --sources <listfile>\n"
    "       mzcc --build";

int main(int argc, char **argv) {
    Options opt;
    memset(&opt, 0, sizeof(opt));
    opt.preset = xstrdup(DEFAULT_PRESET);
    sl_init(&opt.pos_srcs);
    sl_init(&opt.srcfiles);
    av_init(&EXTRA_CPPDEFS);

    /* Hand-rolled argv scan (decision DI1). Flags accepted in any position,
       byte-compatible with cc-maize.sh:173-196 so every existing caller ports
       with zero change. */
    int positional_only = 0;
    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        if (positional_only) {
            sl_push(&opt.pos_srcs, a);
            continue;
        }
        if (strcmp(a, "--build") == 0) {
            opt.mode_build = 1;
        } else if (strcmp(a, "-r") == 0 || strcmp(a, "--run") == 0) {
            opt.run = 1;
        } else if (strcmp(a, "--emit") == 0) {
            opt.emit = 1;
        } else if (strcmp(a, "--dev") == 0) {
            opt.dev = 1;
        } else if (strcmp(a, "-D") == 0) {
            av_add(&EXTRA_CPPDEFS, "-D");
            av_add(&EXTRA_CPPDEFS, (i + 1 < argc) ? argv[++i] : "");
        } else if (strncmp(a, "-D", 2) == 0) {
            av_add(&EXTRA_CPPDEFS, a);
        } else if (strcmp(a, "--compile-only") == 0) {
            opt.run = 0;
        } else if (strcmp(a, "-o") == 0) {
            free(opt.out);
            opt.out = xstrdup((i + 1 < argc) ? argv[++i] : "");
        } else if (strncmp(a, "-o", 2) == 0) {
            free(opt.out);
            opt.out = xstrdup(a + 2);
        } else if (strcmp(a, "--preset") == 0) {
            free(opt.preset);
            opt.preset = xstrdup((i + 1 < argc) ? argv[++i] : "");
        } else if (strncmp(a, "--preset=", 9) == 0) {
            free(opt.preset);
            opt.preset = xstrdup(a + 9);
        } else if (strcmp(a, "--sources") == 0) {
            if (i + 1 < argc) { sl_push(&opt.srcfiles, argv[++i]); }
            opt.used_sources = 1;
        } else if (strncmp(a, "--sources=", 10) == 0) {
            sl_push(&opt.srcfiles, a + 10);
            opt.used_sources = 1;
        } else if (strcmp(a, "--") == 0) {
            positional_only = 1;
        } else if (a[0] == '-') {
            die("unknown option: %s", a);
        } else {
            sl_push(&opt.pos_srcs, a);
        }
    }

    /* REPO_ROOT: MAIZE_ROOT override, else two directory levels up from the
       mzcc exe (built to build/<preset>/mzcc), mirroring cc-maize.sh's
       script-relative self-location (decision DI9). */
    const char *root_env = getenv("MAIZE_ROOT");
    if (root_env && root_env[0]) {
        REPO_ROOT = xstrdup(root_env);
        to_slashes(REPO_ROOT);
    } else {
        char selfpath[4096];
        if (mzcc_self_path(argv[0], selfpath, sizeof(selfpath)) != 0) {
            die("cannot locate the mzcc executable; set MAIZE_ROOT to the repo root");
        }
        to_slashes(selfpath);
        REPO_ROOT = xstrdup(selfpath);
        strip_last_component(REPO_ROOT);   /* drop /mzcc[.exe] -> build/<preset> */
        strip_last_component(REPO_ROOT);   /* drop /<preset>   -> build */
        strip_last_component(REPO_ROOT);   /* drop /build      -> repo root */
    }

    RT_DIR = joinstr(REPO_ROOT, "/toolchain/rt", NULL, NULL);

    /* --build: spawn scripts/build-toolchain.sh and exit with its status
       (decision DI8, the same delegation cc-maize.sh:200 does). On Windows this
       needs a POSIX shell (Git Bash); it is the one residual shell dependency,
       off the compile/ctest/one-shot hot path. Native absorption is maize-279. */
    if (opt.mode_build) {
        char *script = joinstr(REPO_ROOT, "/scripts/build-toolchain.sh", NULL, NULL);
        int code = 2;
        Argv bv;
        av_init(&bv);
        if (IS_WINDOWS) {
            /* Git Bash is the one residual shell dependency on Windows (DI8).
               A bare "bash" resolves to System32\bash.exe (the WSL launcher)
               under the native PATH, which cannot open a Windows drive path, so
               resolve Git Bash deliberately (MZCC_BASH override, then the usual
               install locations) and hand it the native script path as a FILE
               argument, which Git Bash opens directly. */
            const char *bash = getenv("MZCC_BASH");
            char *bash_owned = NULL;
            if (!bash || !bash[0]) {
                const char *cands[] = {
                    "C:/Program Files/Git/bin/bash.exe",
                    "C:/Program Files/Git/usr/bin/bash.exe",
                    "C:/Program Files (x86)/Git/bin/bash.exe",
                    NULL
                };
                for (int i = 0; cands[i]; ++i) {
                    if (path_exists(cands[i])) { bash_owned = xstrdup(cands[i]); break; }
                }
                bash = bash_owned ? bash_owned : "bash";
            }
            av_add(&bv, bash);
            av_add(&bv, script);
            free(bash_owned);
        } else {
            av_add(&bv, script);
        }
        int ran = run_inherit(bv.v[0], bv.v, bv.n, &code);
        av_free(&bv);
        if (!ran) {
            die("could not spawn %s (a POSIX shell is required for --build)", script);
        }
        free(script);
        return code;
    }

    /* Collect sources: positionals first (command-line order), then each
       --sources listfile (one path per line; blank lines and # comments
       skipped; CR stripped). No Windows-path translation (design D6: native
       paths pass straight through). */
    StrList src_list;
    sl_init(&src_list);
    for (int i = 0; i < opt.pos_srcs.n; ++i) {
        if (!is_regular_file(opt.pos_srcs.v[i])) {
            die("no such file: %s", opt.pos_srcs.v[i]);
        }
        sl_push(&src_list, opt.pos_srcs.v[i]);
    }
    for (int i = 0; i < opt.srcfiles.n; ++i) {
        const char *lf = opt.srcfiles.v[i];
        if (!is_regular_file(lf)) {
            die("no such --sources listfile: %s", lf);
        }
        ByteBuf lb;
        if (read_file(lf, &lb) != 0) {
            byte_buf_free(&lb);
            die("cannot read --sources listfile: %s", lf);
        }
        size_t p = 0;
        while (p < lb.len) {
            size_t q = p;
            while (q < lb.len && lb.data[q] != '\n') { ++q; }
            /* line lb[p..q), strip CR */
            ByteBuf line;
            byte_buf_init(&line);
            for (size_t k = p; k < q; ++k) {
                if (lb.data[k] != '\r') { byte_buf_append(&line, &lb.data[k], 1); }
            }
            byte_buf_append(&line, "\0", 1);
            const char *entry = line.data;
            if (entry[0] != '\0' && entry[0] != '#') {
                if (!is_regular_file(entry)) {
                    die("no such file: %s", entry);
                }
                sl_push(&src_list, entry);
            }
            byte_buf_free(&line);
            p = (q < lb.len) ? q + 1 : q;
        }
        byte_buf_free(&lb);
    }

    if (src_list.n == 0) {
        fprintf(stderr, "mzcc: %s\n", USAGE);
        return 2;
    }

    int multi = (src_list.n >= 2) || opt.used_sources;

    /* Multi-source preconditions, checked early (cc-maize.sh:262-273). */
    if (multi) {
        if (opt.emit) {
            die("--emit works only when compiling a single .c file. Drop --emit for a\n"
                "       multi-file build: mzcc [--preset <name>] -o <out.mzx> <a.c> <b.c> ...");
        }
        if ((!opt.out || !opt.out[0]) && !opt.run) {
            die("a multi-file build needs an output path: pass -o <out.mzx> (add -r to\n"
                "       also run it), or -r alone to build and run it.");
        }
    }

    BUILD_DIR = joinstr(REPO_ROOT, "/build/", opt.preset, NULL);

    /* Tool discovery (host-aware .exe resolution, maize-257). */
    CPROC_QBE = resolve_exe(joinstr(REPO_ROOT, "/toolchain/cproc/cproc-qbe", NULL, NULL));
    if (!CPROC_QBE) { die("cproc-qbe not found; run 'mzcc --build' (cproc/qbe)."); }
    QBE = resolve_exe(joinstr(REPO_ROOT, "/toolchain/qbe/obj/qbe", NULL, NULL));
    if (!QBE) { die("qbe not found; run 'mzcc --build' (cproc/qbe)."); }
    MAZM = resolve_exe(path_join(BUILD_DIR, "mazm"));
    if (!MAZM) { die("mazm not found in %s; run scripts/install-mazm.sh (or run-tests.sh) first.", BUILD_DIR); }
    MAIZE = resolve_exe(path_join(BUILD_DIR, "maize"));
    if (!MAIZE) { die("maize not found in %s; run scripts/install-mazm.sh (or run-tests.sh) first.", BUILD_DIR); }
    MZLD = resolve_exe(path_join(BUILD_DIR, "mzld"));
    if (!MZLD) { die("mzld not found in %s; run scripts/install-mazm.sh (or run-tests.sh) first.", BUILD_DIR); }

    /* Preprocessor discovery (cc-maize.sh:314-331, decision D4): $CC, else cc,
       else gcc, else (Windows) the vendored llvm-mingw clang. The section-3c
       macro matrix is applied identically regardless of which candidate wins. */
    const char *cc_env = getenv("CC");
    if (cc_env && cc_env[0]) {
        CPP = xstrdup(cc_env);
    } else if (which_exists("cc")) {
        CPP = xstrdup("cc");
    } else if (which_exists("gcc")) {
        CPP = xstrdup("gcc");
    } else {
        char *vc = joinstr(REPO_ROOT, "/.toolchains/llvm-mingw/bin/x86_64-w64-mingw32-clang.exe", NULL, NULL);
        if (IS_WINDOWS && path_exists(vc)) {
            CPP = vc;
        } else {
            free(vc);
            die("no C preprocessor (cc/gcc) found for #include expansion. On Windows,\n"
                "       run scripts/bootstrap-toolchain.ps1 to fetch the vendored llvm-mingw clang.");
        }
    }

    /* Scratch dirs: OBJ_DIR holds the .mzo objects and the linked image;
       CPP_CWD is an empty dir that becomes cpp's implicit quote-include base
       (DI6). Both under one temp root cleaned on exit. */
    char troot[4096];
    if (mzcc_make_temp_dir(troot, sizeof(troot)) != 0) {
        die("could not create a scratch directory");
    }
    to_slashes(troot);
    SCRATCH_ROOT = xstrdup(troot);
    atexit(cleanup_scratch);
    OBJ_DIR = path_join(SCRATCH_ROOT, "obj");
    CPP_CWD = path_join(SCRATCH_ROOT, "cwd");
    mkdir_p(OBJ_DIR);
    mkdir_p(CPP_CWD);

    /* ---- RT object set and link order (section 8): the single point of truth.
       RT asm objects first (crt0, syscall, setjmp[, mzdev]), then the libc C
       modules, then the user body/bodies last. mzld resolves by name so order
       is layout-only; it is preserved byte-identical to cc-maize.sh:574-582. */
    StrList rt_objs;
    sl_init(&rt_objs);

    static const char *RT_ASM[] = { "crt0", "syscall", "setjmp" };
    for (size_t i = 0; i < sizeof(RT_ASM) / sizeof(RT_ASM[0]); ++i) {
        char *mzo = assemble_rt_asm(RT_ASM[i]);
        if (!mzo) { return 1; }
        sl_push(&rt_objs, mzo);
        free(mzo);
    }
    if (opt.dev) {
        char *mzo = assemble_rt_asm("mzdev");
        if (!mzo) { return 1; }
        sl_push(&rt_objs, mzo);
        free(mzo);
    }

    static const char *RT_C[] = {
        "errno", "string", "strings", "ctype", "math",
        "stdio", "stdlib", "unistd", "dirent", "termios", "time"
    };
    for (size_t i = 0; i < sizeof(RT_C) / sizeof(RT_C[0]); ++i) {
        char *cpath = joinstr(RT_DIR, "/", RT_C[i], ".c");
        char *tag = joinstr("rt_", RT_C[i], NULL, NULL);
        char *mzo = compile_tu(cpath, tag, NULL);
        free(cpath);
        free(tag);
        if (!mzo) {
            fprintf(stderr, "mzcc: failed to compile C runtime object %s.c\n", RT_C[i]);
            return 1;
        }
        sl_push(&rt_objs, mzo);
        free(mzo);
    }

    /* ---- compile user sources ------------------------------------------- */
    StrList user_objs;
    sl_init(&user_objs);
    ByteBuf emit_body;
    int have_emit_body = 0;
    byte_buf_init(&emit_body);

    if (multi) {
        for (int i = 0; i < src_list.n; ++i) {
            char *base = base_noext_c(src_list.v[i]);
            char pfx[32];
            snprintf(pfx, sizeof(pfx), "u%d_", i);
            char *tag = joinstr(pfx, base, NULL, NULL);
            char *mzo = compile_tu(src_list.v[i], tag, NULL);
            free(base);
            free(tag);
            if (!mzo) { return 1; }
            sl_push(&user_objs, mzo);
            free(mzo);
        }
    } else {
        char *base = base_noext_c(src_list.v[0]);
        char *mzo = compile_tu(src_list.v[0], base, opt.emit ? &emit_body : NULL);
        have_emit_body = opt.emit;
        free(base);
        if (!mzo) { return 1; }
        sl_push(&user_objs, mzo);
        free(mzo);
    }

    /* ---- link (default C profile: RT + libc + body, entry _start). The
       link-profile seam is the object set; build-quesos's minimal profile
       slots in at maize-280 without a second pipeline (section 8). ---------- */
    char *mzx = path_join(SCRATCH_ROOT, "prog.mzx");
    Argv lav;
    av_init(&lav);
    av_add(&lav, MZLD);
    av_add(&lav, "-o");
    av_add(&lav, mzx);
    for (int i = 0; i < rt_objs.n; ++i) { av_add(&lav, rt_objs.v[i]); }
    for (int i = 0; i < user_objs.n; ++i) { av_add(&lav, user_objs.v[i]); }
    ProcResult lr;
    int ran = run_proc(MZLD, lav.v, lav.n, "", 0, NULL, &lr);
    av_free(&lav);
    if (!ran || lr.exit_code != 0 || !path_exists(mzx)) {
        fprintf(stderr, "mzcc: mzld failed linking the image\n");
        if (lr.stderr_bytes.len) { fwrite(lr.stderr_bytes.data, 1, lr.stderr_bytes.len, stderr); }
        if (lr.stdout_bytes.len) { fwrite(lr.stdout_bytes.data, 1, lr.stdout_bytes.len, stderr); }
        byte_buf_free(&lr.stdout_bytes);
        byte_buf_free(&lr.stderr_bytes);
        return 1;
    }
    byte_buf_free(&lr.stdout_bytes);
    byte_buf_free(&lr.stderr_bytes);

    /* ---- ordering (load-bearing, D6): emit + produce happen BEFORE run, so
       the artifacts land even on a nonzero guest exit. ---------------------- */
    const char *single_src = multi ? NULL : src_list.v[0];

    if (have_emit_body) {
        char *dst_dir = dir_of(single_src);
        char *base = base_noext_c(single_src);
        char *dst = joinstr(dst_dir, "/", base, ".mazm");
        if (write_file(dst, emit_body.data, emit_body.len) != 0) {
            fprintf(stderr, "mzcc: could not write %s\n", dst);
        } else {
            fprintf(stderr, "mzcc: emitted %s (qbe body)\n", dst);
        }
        free(dst_dir); free(base); free(dst);
    }
    byte_buf_free(&emit_body);

    if (opt.out && opt.out[0]) {
        char *odir = dir_of(opt.out);
        if (!path_exists(odir)) { mkdir_p(odir); }
        free(odir);
        if (copy_file(mzx, opt.out) != 0) {
            fprintf(stderr, "mzcc: could not write %s\n", opt.out);
            return 1;
        }
    } else if (!opt.run) {
        /* beside-source produce (single-source default). */
        char *dst_dir = dir_of(single_src);
        char *base = base_noext_c(single_src);
        char *dst = joinstr(dst_dir, "/", base, ".mzx");
        if (copy_file(mzx, dst) != 0) {
            fprintf(stderr, "mzcc: could not write %s\n", dst);
            free(dst_dir); free(base); free(dst);
            return 1;
        }
        fprintf(stderr, "mzcc: produced %s\n", dst);
        free(dst_dir); free(base); free(dst);
    }

    /* ---- run + propagate the guest exit code only when asked ------------- */
    if (opt.run) {
        Argv rv;
        av_init(&rv);
        av_add(&rv, MAIZE);
        av_add(&rv, mzx);
        int code = 1;
        int rr = run_inherit(MAIZE, rv.v, rv.n, &code);
        av_free(&rv);
        free(mzx);
        if (!rr) {
            fprintf(stderr, "mzcc: could not spawn maize\n");
            return 1;
        }
        return code;
    }

    free(mzx);
    return 0;
}
