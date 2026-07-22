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
#include "mzcc_internal.h"
#include "mzcc_fs.h"
#include "mzcc_cache.h"
#include "mzcc_sched.h"
#include "mzcc_sha256.h"

#include <assert.h>
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

void die(const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "mzcc: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    exit(2); /* usage / environment failure, matching cc-maize.sh */
}

void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) {
        die("out of memory");
    }
    return p;
}

char *xstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = (char *)xmalloc(n);
    memcpy(p, s, n);
    return p;
}

/* Concatenate up to 4 fragments into a fresh allocation. */
char *joinstr(const char *a, const char *b, const char *c, const char *d) {
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

char *path_join(const char *dir, const char *leaf) {
    return joinstr(dir, "/", leaf, NULL);
}

int is_regular_file(const char *p) {
    struct stat st;
    if (stat(p, &st) != 0) {
        return 0;
    }
    return (st.st_mode & S_IFMT) == S_IFREG;
}

int path_exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0;
}

/* Normalize backslashes to forward slashes in place (Windows GetModuleFileNameA
   yields backslashes; every native tool and clang accepts forward slashes, and
   paths never enter the .mzx image, so this is cosmetic uniformity only). */
void to_slashes(char *s) {
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
char *dir_of(const char *path) {
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

/* A stable, host-independent identity for `src`, used to tell cpp what
   __FILE__ should expand to (maize-290). cpp is fed the source via stdin
   (decision DI6, no named input file), so without this, __FILE__ would
   otherwise take whatever placeholder value a bare "-" stdin feed implies;
   TUs that embed __FILE__ in a string literal (doom's w_wad.c, via
   z_zone.h's Z_ChangeTag macro) need that value to be the SAME string on
   every host and every run, not a per-invocation scratch path.

   Absolutizes `src` against the real CWD (same construction as
   abs_dir_of), then strips the REPO_ROOT prefix so the result is the
   source's repo-relative path (forward slashes): deterministic regardless
   of host, temp-dir placement, or invocation cwd, and still a meaningful
   diagnostic value. Falls back to the bare basename for a source living
   outside the repo tree. Returns a fresh allocation. */
static char *stable_file_identity(const char *src) {
    char *abs = xstrdup(src);
    to_slashes(abs);
    if (!is_abs_path(abs)) {
        char *cwd = get_real_cwd();
        char *joined = joinstr(cwd, "/", abs, NULL);
        free(cwd);
        free(abs);
        abs = joined;
    }
    size_t root_len = REPO_ROOT ? strlen(REPO_ROOT) : 0;
    if (REPO_ROOT && strncmp(abs, REPO_ROOT, root_len) == 0 && abs[root_len] == '/') {
        char *rel = xstrdup(abs + root_len + 1);
        free(abs);
        return rel;
    }
    char *slash = strrchr(abs, '/');
    char *base = xstrdup(slash ? slash + 1 : abs);
    free(abs);
    return base;
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

/* Resolve `name` to the absolute path of the first PATH entry that contains it,
   or NULL if it is not on PATH (maize-303). The path-recovering variant of
   which_exists: the cpp-identity guard needs the ACTUAL file to hash, not a
   yes/no, because CPP is a bare name on Linux (cc/gcc/$CC) that which_exists
   confirms but discards. Returns a fresh allocation. */
static char *which_resolve(const char *name) {
    const char *path = getenv("PATH");
    if (!path || !path[0]) {
        return NULL;
    }
    char *copy = xstrdup(path);
    char *result = NULL;
    /* Plain strtok: PATH parsing here is single-threaded and non-reentrant use
       is safe (no nested tokenization), and strtok is available on every host. */
    char *tok = strtok(copy, PATH_SEP_LIST);
    for (; tok; tok = strtok(NULL, PATH_SEP_LIST)) {
        char *cand = path_join(tok, name);
        if (path_exists(cand)) { result = cand; break; }
        free(cand);
        if (IS_WINDOWS) {
            char *cexe = joinstr(tok, "/", name, ".exe");
            if (path_exists(cexe)) { result = cexe; break; }
            free(cexe);
        }
    }
    free(copy);
    return result;
}

/* Is `name` resolvable on PATH? (existence probe only; the bare name is then
   handed to run_proc, whose execvp/CreateProcess re-resolves it.) */
int which_exists(const char *name) {
    char *p = which_resolve(name);
    if (p) { free(p); return 1; }
    return 0;
}

int read_file(const char *path, ByteBuf *out) {
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

int write_file(const char *path, const char *data, size_t len) {
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

int copy_file(const char *src, const char *dst) {
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
void mkdir_p(const char *path) {
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

/* ---- growable argv builder (type in mzcc_internal.h) ------------------- */

void av_init(Argv *a) {
    a->v = NULL;
    a->n = 0;
    a->cap = 0;
}

void av_add(Argv *a, const char *s) {
    if (a->n + 2 > a->cap) {
        a->cap = a->cap ? a->cap * 2 : 8;
        a->v = (char **)realloc(a->v, (size_t)a->cap * sizeof(char *));
        if (!a->v) { die("out of memory"); }
    }
    a->v[a->n++] = xstrdup(s);
    a->v[a->n] = NULL;
}

void av_free(Argv *a) {
    for (int i = 0; i < a->n; ++i) {
        free(a->v[i]);
    }
    free(a->v);
    av_init(a);
}

/* ---- growable string list (type in mzcc_internal.h) -------------------- */

void sl_init(StrList *s) {
    s->v = NULL;
    s->n = 0;
    s->cap = 0;
}

void sl_push(StrList *s, const char *item) {
    if (s->n + 1 > s->cap) {
        s->cap = s->cap ? s->cap * 2 : 8;
        s->v = (char **)realloc(s->v, (size_t)s->cap * sizeof(char *));
        if (!s->v) { die("out of memory"); }
    }
    s->v[s->n++] = xstrdup(item);
}

void sl_free(StrList *s) {
    for (int i = 0; i < s->n; ++i) {
        free(s->v[i]);
    }
    free(s->v);
    sl_init(s);
}

/* ---- failure-diagnostic sink (maize-274) -------------------------------
   Every per-TU failure message and captured child stderr goes through these two
   helpers. With `diag == NULL` (the serial single-file path, quesos, --build)
   the text reaches the process stderr inline exactly as before. Under the
   parallel scheduler each job passes its own `diag` ByteBuf, so concurrent
   failures never interleave on stderr; the driver flushes each failed job's
   captured diagnostics in canonical order after join (spec 3d). */
static void diag_printf(ByteBuf *diag, const char *fmt, ...) {
    va_list ap;
    if (!diag) {
        va_start(ap, fmt);
        vfprintf(stderr, fmt, ap);
        va_end(ap);
        return;
    }
    char buf[2048];
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        byte_buf_append(diag, buf, (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1);
    }
}

static void diag_bytes(ByteBuf *diag, const char *data, size_t len) {
    if (len == 0) {
        return;
    }
    if (!diag) {
        fwrite(data, 1, len, stderr);
    } else {
        byte_buf_append(diag, data, len);
    }
}

/* MAIZE_CACHE_STATS=1 turns on a per-TU HIT/MISS line to stderr (spec 2d / AC
   9633: a cache-hit log is the observable that proves cproc-qbe/qbe/mazm were
   skipped on a hit). Resolved ONCE, on the main thread, in resolve_toolchain
   (cycle-2 review finding 2): the previous shape lazily first-wrote a static
   local (`v = -1` sentinel) from whichever thread called cache_log() first,
   which is a scheduler worker the moment the parallel object build starts,
   a TSan-flaggable racy first-write on a global. resolve_toolchain runs
   serially before build_objects_parallel spawns any worker (the same
   guarantee mzcc_cache_configure's warm-init relies on), so resolving the
   flag there and having cache_stats_on() do a plain read closes the race
   with no lazy first-write left on any cache/stats global. */
static int g_cache_stats_on = 0;

static void mzcc_cache_stats_resolve(void) {
    const char *e = getenv("MAIZE_CACHE_STATS");
    g_cache_stats_on = (e && e[0] && !(e[0] == '0' && e[1] == '\0')) ? 1 : 0;
}

static int cache_stats_on(void) {
    return g_cache_stats_on;
}

static void cache_log(const char *verb, const char *tag) {
    if (cache_stats_on()) {
        fprintf(stderr, "mzcc: cache %s %s\n", verb, tag);
    }
}

/* Process-wide -j override (maize-274), 0 = unset. Set by main()'s scan or a
   batch subcommand via mzcc_set_jobs_override. */
static int g_jobs_override = 0;

void mzcc_set_jobs_override(int n) {
    g_jobs_override = (n > 0) ? n : 0;
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

char *REPO_ROOT = NULL;         /* extern (mzcc_internal.h): subcommands read it */
static char *BUILD_DIR = NULL;
char *RT_DIR = NULL;            /* extern (mzcc_internal.h): quesos reads syscall.mazm from here */
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

/* ---- direct-mode cpp-output cache identity (maize-303) ------------------ */
/* g_ppcache_disabled: set at toolchain-resolve time when NO real cpp identity
   can be obtained (CPP unresolvable/unreadable AND the --version probe fails).
   When set, the cpp-output cache is bypassed for the whole build (always spawn
   cpp), the fail-CLOSED degrade: a missing identity never folds a constant and
   serves a stale hit. g_cpp_identity is the 32-byte SHA-256 over the resolved
   CPP file bytes plus its `--version` output; valid only when NOT disabled.
   g_rt_listing memoizes RT_DIR's sorted entry-basename listing (the shadow
   guard), warmed once on the main thread in ppcache_configure so no worker is
   ever its first writer. */
static int     g_ppcache_disabled = 0;
static uint8_t g_cpp_identity[MZCC_SHA256_DIGEST_LEN];
static char   *g_rt_listing = NULL;

/* argv[0] captured at main() entry, so ensure_repo_root() can self-locate even
   when reached from a subcommand entry point (which is handed argv+2). */
static const char *g_argv0 = "mzcc";

static void cleanup_scratch(void) {
    if (SCRATCH_ROOT) {
        mzcc_remove_tree(SCRATCH_ROOT);
    }
}

/* ---- RT object set (section 8): the single point of truth. RT asm objects
   first (crt0, syscall, setjmp), then the libc C modules. Since maize-302 these
   are compiled ONCE into the prebuilt runtime archive (.mza) by
   ensure_runtime_archive rather than per program; the member order here is the
   archive's declared layout order, so crt0/_start still lands first and the user
   body last, preserving the from-source layout. mzdev stays a per-build asm
   object (decision D3) and is NOT in this set. mzld resolves by symbol, so order
   is layout-only. Shared by ensure_runtime_archive and the archive-key hash. */
static const char *RT_ASM[] = { "crt0", "syscall", "setjmp" };
static const char *RT_C[] = {
    "errno", "string", "strings", "ctype", "math",
    "stdio", "stdlib", "unistd", "dirent", "termios", "time"
};

/* ---- per-TU pipeline ---------------------------------------------------- */

/* Assemble a body (or an RT .mazm) read from `bytes` into <OBJ_DIR>/<tag>.mzo
   via the mazm stdin-to-object extension (7a): mazm -c --stdin --base-path
   <OBJ_DIR> --source-name <tag>. Returns a fresh path to the .mzo on success,
   NULL on failure (message already printed). */
static char *assemble_stdin(const char *bytes, size_t len, const char *tag, ByteBuf *diag) {
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
        diag_printf(diag, "mzcc: mazm -c failed for %s\n", tag);
        diag_bytes(diag, r.stderr_bytes.data, r.stderr_bytes.len);
        diag_bytes(diag, r.stdout_bytes.data, r.stdout_bytes.len);
        byte_buf_free(&r.stdout_bytes);
        byte_buf_free(&r.stderr_bytes);
        free(mzo);
        return NULL;
    }
    byte_buf_free(&r.stdout_bytes);
    byte_buf_free(&r.stderr_bytes);
    return mzo;
}

/* ======================================================================== */
/* Direct-mode cpp-output cache (maize-303).                                 */
/*                                                                           */
/* A ccache-style two-level cache in front of the cpp spawn: a MANIFEST      */
/* (keyed by manifest_key, computable without running cpp) records the       */
/* transitive include set a TU opened; a PREPROCESSED entry (keyed by        */
/* preprocessed_key = f(manifest_key, current header bytes)) holds the cpp   */
/* stdout. On a warm rebuild an unchanged TU skips cpp entirely. Correctness */
/* is paramount: a stale hit is a silent miscompile, so preprocessed_key is  */
/* recomputed from LIVE header bytes at lookup (a changed/removed header      */
/* misses by construction), cpp identity is folded real-and-fail-closed, and */
/* the -I entry listings guard the header-shadowing hazard.                  */
/* ======================================================================== */

/* Absorb `len` bytes into a running SHA-256, length-prefixed (big-endian u64)
   so two adjacent contributions cannot alias by a boundary shift. Mirrors
   mzcc_cache.c's fp_absorb_file framing for in-memory buffers. */
static void sha_absorb_lp(mzcc_sha256_ctx *c, const void *data, size_t len) {
    uint8_t lp[8];
    for (int i = 0; i < 8; ++i) { lp[i] = (uint8_t)((uint64_t)len >> (56 - i * 8)); }
    mzcc_sha256_update(c, lp, 8);
    mzcc_sha256_update(c, len ? data : "", len);
}

static int cmp_cstr(const void *a, const void *b) {
    const char *sa = *(const char *const *)a;
    const char *sb = *(const char *const *)b;
    return strcmp(sa, sb);
}

/* The sorted, newline-joined basename listing of `dir` (the shadow-guard
   component): a fresh string, empty when `dir` is unreadable. Filenames only,
   NOT contents; this component only has to notice the candidate-file SET
   changed (an added/removed same-named header at a higher -I priority), since
   an actually-included header's content change is already caught by
   preprocessed_key. */
static char *dir_listing_string(const char *dir) {
    StrList names;
    sl_init(&names);
    if (list_dir(dir, &names) != 0) { sl_free(&names); return xstrdup(""); }
    if (names.n > 1) { qsort(names.v, (size_t)names.n, sizeof(char *), cmp_cstr); }
    ByteBuf b;
    byte_buf_init(&b);
    for (int i = 0; i < names.n; ++i) {
        byte_buf_append(&b, names.v[i], strlen(names.v[i]));
        byte_buf_append(&b, "\n", 1);
    }
    sl_free(&names);
    char *r = xmalloc(b.len + 1);
    if (b.len) { memcpy(r, b.data, b.len); }
    r[b.len] = '\0';
    byte_buf_free(&b);
    return r;
}

/* RT_DIR's listing, memoized. RT_DIR is constant across a build and shared by
   every TU, so this is computed once; warmed on the main thread by
   ppcache_configure before any worker exists, so the first-write is never a
   race (mirroring the object cache's warm-init discipline). The per-program
   src_dir listing is NOT memoized (it is cheap and per-src-dir memoization
   would need cross-worker locking); recomputing it per TU is correct and
   over-invalidation-safe. */
static const char *rt_listing(void) {
    if (!g_rt_listing) { g_rt_listing = dir_listing_string(RT_DIR ? RT_DIR : ""); }
    return g_rt_listing;
}

/* If slash-normalized `path` lies strictly under slash-normalized `root`
   (root + '/' + tail, tail non-empty), return a fresh copy of the tail; else
   NULL. A trailing '/' on root is ignored so RT_DIR / src_dir match whether or
   not they carry one. Used to re-express an absolute staged include path as a
   stable, root-relative reference (maize-303 cycle-2). */
static char *rel_under(const char *path, const char *root) {
    if (!path || !root || !root[0]) { return NULL; }
    size_t rl = strlen(root);
    while (rl > 0 && root[rl - 1] == '/') { --rl; }
    if (rl == 0) { return NULL; }
    if (strncmp(path, root, rl) != 0 || path[rl] != '/' || !path[rl + 1]) {
        return NULL;
    }
    return xstrdup(path + rl + 1);
}

/* Encode a discovered (absolute, slash-normalized) include path as a STABLE,
   re-anchorable manifest reference: "S\t<rel>" when it lies under src_dir,
   "R\t<rel>" when under RT_DIR. Returns a fresh encoded string, or NULL when
   the path lies under NEITHER root. The two roots are physically disjoint
   (RT_DIR under REPO_ROOT, src_dir under the staging temp), so classification
   is unambiguous. An unmappable include (a header outside both search roots)
   returns NULL, and the caller then DECLINES to cache the TU (a safe re-
   preprocess every build, never a wrong hit): this is the fail-safe the spec's
   "stable identity or bypass" clause requires. src_dir is deliberately keyed as
   a ROOT here (not the absolute value) so the reference survives the randomized
   per-invocation staging dir. */
static char *encode_include(const char *abspath, const char *src_dir) {
    char *rel = rel_under(abspath, src_dir);
    if (rel) { char *e = joinstr("S\t", rel, NULL, NULL); free(rel); return e; }
    rel = rel_under(abspath, RT_DIR);
    if (rel) { char *e = joinstr("R\t", rel, NULL, NULL); free(rel); return e; }
    return NULL;
}

/* Re-anchor an encoded reference to the CURRENT invocation's roots so its live
   bytes can be read: "R\t<rel>" -> <RT_DIR>/<rel>, "S\t<rel>" -> <src_dir>/<rel>.
   Returns a fresh absolute path, or NULL on a malformed reference (treated as a
   miss). The manifest was written by a prior invocation whose absolute staging
   dir is gone; re-anchoring reads THIS invocation's freshly staged copy, so a
   header edited in the repo re-stages with new bytes and misses by content. */
static char *reanchor_include(const char *enc, const char *src_dir) {
    if (!enc || !enc[0] || enc[1] != '\t') { return NULL; }
    const char *rel = enc + 2;
    if (enc[0] == 'R') { return joinstr(RT_DIR, "/", rel, NULL); }
    if (enc[0] == 'S') { return joinstr(src_dir ? src_dir : "", "/", rel, NULL); }
    return NULL;
}

/* manifest_key = SHA-256(domain, cpp-identity, RT-listing, src_dir-listing,
   fed bytes, each output-affecting cpp flag argv[flag_lo..flag_hi) in order,
   then the two -I search paths as STABLE identities). flag_hi is the index just
   BEFORE the two `-I <dir>` argv pairs, so those absolute VALUES are excluded
   from the verbatim flag fold and re-added below as path-independent tokens:
   RT_DIR by its repo-relative path, src_dir by the source's maize-290 stable
   identity `src_id`. This is the cycle-2 fix: the old key folded the absolute
   `-I src_dir` value, which is a fresh randomized staging path every invocation
   (mzcc_userland.c stage_root), so a warm rebuild never hit across invocations.
   The -I directory LISTINGS (the shadow guard, folded above) plus src_id carry
   the search-path semantics without the volatile absolute string. */
static void compute_manifest_key(const Argv *cpp, int flag_lo, int flag_hi,
                                 const char *fed_data, size_t fed_len,
                                 const char *src_dir, const char *src_id,
                                 char out[MZCC_SHA256_HEX_LEN + 1]) {
    mzcc_sha256_ctx c;
    mzcc_sha256_init(&c);
    static const char dom[] = "ppcache-manifest-v2";
    sha_absorb_lp(&c, dom, sizeof(dom) - 1);
    /* Reaching here implies !g_ppcache_disabled, i.e. the identity is real. */
    sha_absorb_lp(&c, g_cpp_identity, sizeof(g_cpp_identity));
    const char *rt = rt_listing();
    sha_absorb_lp(&c, rt, strlen(rt));
    char *sd = dir_listing_string(src_dir);
    sha_absorb_lp(&c, sd, strlen(sd));
    free(sd);
    sha_absorb_lp(&c, fed_data, fed_len);
    for (int i = flag_lo; i < flag_hi; ++i) {
        sha_absorb_lp(&c, cpp->v[i], strlen(cpp->v[i]));
    }
    static const char itok[] = "-I";
    char *rtrel = rel_under(RT_DIR, REPO_ROOT);
    const char *rtkey = rtrel ? rtrel : "toolchain/rt";
    sha_absorb_lp(&c, itok, sizeof(itok) - 1);
    sha_absorb_lp(&c, rtkey, strlen(rtkey));
    free(rtrel);
    sha_absorb_lp(&c, itok, sizeof(itok) - 1);
    sha_absorb_lp(&c, src_id, strlen(src_id));
    uint8_t dig[MZCC_SHA256_DIGEST_LEN];
    mzcc_sha256_final(&c, dig);
    mzcc_sha256_hex(dig, out);
}

/* preprocessed_key = SHA-256(domain, manifest_key hex, and for each recorded
   include in order: its STABLE encoded reference + its CURRENT content bytes,
   read by re-anchoring the reference to this invocation's roots). Returns 1
   with the key filled when every include re-anchored and read; 0 (a MISS, never
   a stale serve) the moment any recorded include is malformed, unmappable, or
   unreadable/removed. Folding the encoded reference (not the absolute path)
   keeps the key stable across the randomized staging dir. */
static int compute_preprocessed_key(const char *manifest_key_hex,
                                    const StrList *includes,
                                    const char *src_dir,
                                    char out[MZCC_SHA256_HEX_LEN + 1]) {
    mzcc_sha256_ctx c;
    mzcc_sha256_init(&c);
    static const char dom[] = "ppcache-output-v2";
    sha_absorb_lp(&c, dom, sizeof(dom) - 1);
    sha_absorb_lp(&c, manifest_key_hex, strlen(manifest_key_hex));
    for (int i = 0; i < includes->n; ++i) {
        const char *enc = includes->v[i];
        sha_absorb_lp(&c, enc, strlen(enc));
        char *path = reanchor_include(enc, src_dir);
        if (!path) { return 0; } /* malformed/unmappable reference -> miss */
        ByteBuf hb;
        if (read_file(path, &hb) != 0) {
            byte_buf_free(&hb);
            free(path);
            return 0; /* a removed/unreadable recorded header forces a miss */
        }
        sha_absorb_lp(&c, hb.data, hb.len);
        byte_buf_free(&hb);
        free(path);
    }
    uint8_t dig[MZCC_SHA256_DIGEST_LEN];
    mzcc_sha256_final(&c, dig);
    mzcc_sha256_hex(dig, out);
    return 1;
}

/* Flush an accumulated depfile token into `out` (canonicalized to forward
   slashes), dropping the empty token and the stdin "-" prerequisite. */
static void dep_flush(ByteBuf *tok, StrList *out) {
    if (tok->len == 0) { return; }
    char *s = xmalloc(tok->len + 1);
    memcpy(s, tok->data, tok->len);
    s[tok->len] = '\0';
    to_slashes(s);
    if (!(s[0] == '-' && s[1] == '\0')) { sl_push(out, s); }
    free(s);
    tok->len = 0;
}

/* Parse a `-MD` depfile ("tu: dep1 dep2 \<newline> dep3 ...") into the ordered
   include set. Strips the target up to the first ':', joins `\`-newline
   continuations, splits on unescaped whitespace, and unescapes `\ ` in a path.
   The stdin "-" prerequisite is dropped by dep_flush. */
static void parse_depfile(const char *data, size_t len, StrList *out) {
    size_t i = 0;
    /* Skip the target: advance past the first unescaped ':'. */
    for (; i < len; ++i) {
        if (data[i] == '\\' && i + 1 < len) { ++i; continue; }
        if (data[i] == ':') { ++i; break; }
    }
    ByteBuf tok;
    byte_buf_init(&tok);
    for (; i < len; ++i) {
        char ch = data[i];
        if (ch == '\\' && i + 1 < len) {
            char nx = data[i + 1];
            if (nx == '\n' || nx == '\r') { dep_flush(&tok, out); ++i; continue; }
            /* An escaped space/tab (or any other escaped char) is literal. */
            byte_buf_append(&tok, &nx, 1);
            ++i;
            continue;
        }
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
            dep_flush(&tok, out);
            continue;
        }
        byte_buf_append(&tok, &ch, 1);
    }
    dep_flush(&tok, out);
    byte_buf_free(&tok);
}

/* Serialize the include set to the v2 manifest format (one STABLE encoded
   reference "S\t<rel>" / "R\t<rel>" per line, preceded by a magic/version
   header and the count) into `out`. v2 stores root-relative references, not the
   v1 absolute paths, so a manifest survives the per-invocation staging dir. */
static void serialize_manifest_into(const StrList *includes, ByteBuf *out) {
    byte_buf_init(out);
    char hdr[64];
    int hn = snprintf(hdr, sizeof(hdr), "MZCC-PPMANIFEST v2\n%d\n", includes->n);
    if (hn > 0) { byte_buf_append(out, hdr, (size_t)hn); }
    for (int i = 0; i < includes->n; ++i) {
        byte_buf_append(out, includes->v[i], strlen(includes->v[i]));
        byte_buf_append(out, "\n", 1);
    }
}

/* Parse a stored v1 manifest back into the include list. Returns 0 on a
   well-formed manifest, -1 on any malformation (the caller treats -1 as a
   miss, never a stale serve). */
static int parse_manifest(const char *data, size_t len, StrList *out) {
    /* Line 0: magic. Line 1: count. Lines 2..2+count: encoded references. */
    static const char magic[] = "MZCC-PPMANIFEST v2";
    size_t mlen = sizeof(magic) - 1;
    if (len < mlen || memcmp(data, magic, mlen) != 0) { return -1; }
    size_t i = mlen;
    if (i >= len || data[i] != '\n') { return -1; }
    ++i;
    /* Read the decimal count. */
    long count = 0;
    int saw_digit = 0;
    for (; i < len && data[i] != '\n'; ++i) {
        if (data[i] < '0' || data[i] > '9') { return -1; }
        count = count * 10 + (data[i] - '0');
        saw_digit = 1;
        if (count > 100000) { return -1; } /* sanity bound */
    }
    if (!saw_digit || i >= len) { return -1; }
    ++i; /* consume the newline after the count */
    long read = 0;
    while (i < len && read < count) {
        size_t start = i;
        while (i < len && data[i] != '\n') { ++i; }
        size_t plen = i - start;
        char *s = xmalloc(plen + 1);
        if (plen) { memcpy(s, data + start, plen); }
        s[plen] = '\0';
        sl_push(out, s);
        free(s);
        ++read;
        if (i < len) { ++i; } /* consume newline */
    }
    return (read == count) ? 0 : -1;
}

/* Resolve the cpp identity once, on the main thread, at toolchain-resolve time
   (called from resolve_toolchain right after CPP is chosen and
   mzcc_cache_configure has run, before any worker spawns). See the
   g_ppcache_disabled comment for the fail-closed contract. */
static void ppcache_configure(void) {
    g_ppcache_disabled = 0;
    free(g_rt_listing);
    g_rt_listing = NULL;

    mzcc_sha256_ctx c;
    mzcc_sha256_init(&c);
    int have_signal = 0;

    /* (1) resolved-file bytes (primary): resolve a bare CPP against PATH, hash
       the actual file. Catches even a same-version rebuild of the binary. */
    char *resolved = NULL;
    if (CPP && CPP[0]) {
        resolved = is_abs_path(CPP) ? xstrdup(CPP) : which_resolve(CPP);
    }
    if (resolved) {
        ByteBuf fb;
        if (read_file(resolved, &fb) == 0) {
            sha_absorb_lp(&c, fb.data, fb.len);
            have_signal = 1;
        }
        byte_buf_free(&fb);
    }
    free(resolved);

    /* (2) `<CPP> --version` bytes (belt-and-suspenders + wrapper/shim catch). */
    if (CPP && CPP[0]) {
        Argv v;
        av_init(&v);
        av_add(&v, CPP);
        av_add(&v, "--version");
        ProcResult pr;
        int ran = run_proc(CPP, v.v, v.n, NULL, 0, NULL, &pr);
        av_free(&v);
        if (ran && (pr.stdout_bytes.len > 0 || pr.stderr_bytes.len > 0)) {
            sha_absorb_lp(&c, pr.stdout_bytes.data, pr.stdout_bytes.len);
            sha_absorb_lp(&c, pr.stderr_bytes.data, pr.stderr_bytes.len);
            have_signal = 1;
        }
        if (ran) {
            byte_buf_free(&pr.stdout_bytes);
            byte_buf_free(&pr.stderr_bytes);
        }
    }

    if (!have_signal) {
        /* Fail closed: no real identity, disable the cpp cache for the build. */
        g_ppcache_disabled = 1;
        return;
    }
    mzcc_sha256_final(&c, g_cpp_identity);

    /* Warm the RT_DIR shadow-guard listing on this (main) thread. */
    (void)rt_listing();
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
   queue around this with zero pipeline rearchitecture.

   `extra_defines` (maize-280) are per-build cpp -D tokens applied in ADDITION to
   the process-global EXTRA_CPPDEFS, at the same argv position, so a batch build
   (oksh's -D EMACS -D volatile=, doom's -D DOOMGENERIC_RES*) reproduces the exact
   cpp line cc-maize.sh emits for the same command-line -D flags. NULL means none,
   which makes compile_tu_ex behavior-identical to the old compile_tu. */
char *compile_tu_ex(const char *src, const char *tag, const Argv *extra_defines,
                    ByteBuf *emit_body, ByteBuf *diag) {
    ByteBuf raw;
    if (read_file(src, &raw) != 0) {
        diag_printf(diag, "mzcc: cannot read source %s\n", src);
        byte_buf_free(&raw);
        return NULL;
    }
    ByteBuf lf;
    crlf_strip(raw.data, raw.len, &lf);
    byte_buf_free(&raw);

    /* Prepend a `# 1 "<identity>"` line-marker (maize-290): the standard
       mechanism every cpp implementation (gcc, clang) uses to name its
       current input when it isn't a real file. It tells cpp "the next
       physical line is logical line 1 of <identity>", so __FILE__/__LINE__
       inside the TU expand exactly as if <identity> had been passed as a
       real path, with no line-number shift from the inserted marker line
       itself. `-P` (below) only suppresses line markers in cpp's OUTPUT;
       this input-side marker is a separate, well-supported mechanism. */
    char *file_id = stable_file_identity(src);
    ByteBuf fed;
    byte_buf_init(&fed);
    byte_buf_append(&fed, "# 1 \"", 5);
    byte_buf_append(&fed, file_id, strlen(file_id));
    byte_buf_append(&fed, "\"\n", 2);
    byte_buf_append(&fed, lf.data, lf.len);
    free(file_id);
    byte_buf_free(&lf);

    char *src_dir = abs_dir_of(src);
    /* The source's maize-290 stable identity (repo-relative, or basename for a
       source outside the repo tree): folded into manifest_key in place of the
       absolute `-I src_dir` value, so the key is stable across the randomized
       per-invocation staging dir (maize-303 cycle-2). */
    char *src_id = stable_file_identity(src);

    /* Stage: cpp. Reproduces cc-maize.sh:442-449 EXACTLY (the maize-257
       host-canonicalization invariant); flags in this order, as discrete argv
       entries, fed the CRLF-stripped source (with the stable __FILE__
       identity marker line prepended above) via stdin (`-`) with cwd = the
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
    if (extra_defines) {
        for (int i = 0; i < extra_defines->n; ++i) {
            av_add(&cpp, extra_defines->v[i]);
        }
    }
    /* `flags_end` marks the argv index just BEFORE the two `-I <dir>` pairs, so
       manifest_key folds the pure output-affecting flags verbatim over
       argv[1..flags_end) and re-adds the two -I search paths as STABLE
       identities (RT_DIR repo-relative, src_dir as src_id) rather than their
       per-invocation absolute values (maize-303 cycle-2). */
    int flags_end = cpp.n;
    av_add(&cpp, "-I"); av_add(&cpp, RT_DIR);
    av_add(&cpp, "-I"); av_add(&cpp, src_dir);
    /* NOTE: the stdin input marker "-" is appended in the MISS branch below,
       AFTER the miss-only -MD/-MF/-MT depfile flags, so the canonical option-
       before-input order holds; manifest_key is computed over the flags here
       (argv[1..flags_end) verbatim plus the two -I paths as stable identities)
       while "-" is still absent. */

    /* ---- direct-mode cpp-output cache lookup (maize-303) ------------------
       Runs BEFORE the cpp spawn. On a preprocessed-cache hit we serve the
       cached bytes AS cpp's stdout and skip the spawn; on any miss (or when the
       cache is off / cpp-identity unavailable) we fall through to the spawn,
       with the depfile side-channel added so the miss populates the manifest +
       preprocessed entries. emit_body bypasses the cpp cache exactly as it
       bypasses the object cache (the qbe body is a side output). */
    int pp_on = mzcc_cache_enabled() && !g_ppcache_disabled && emit_body == NULL;
    char mkey[MZCC_SHA256_HEX_LEN + 1];
    ProcResult pp;
    int pp_served = 0; /* 1 once pp.stdout_bytes holds served cpp-cache bytes */
    if (pp_on) {
        compute_manifest_key(&cpp, 1, flags_end, fed.data, fed.len, src_dir, src_id, mkey);
        ByteBuf mbytes;
        if (mzcc_cache_manifest_lookup(mkey, &mbytes)) {
            StrList inc;
            sl_init(&inc);
            if (parse_manifest(mbytes.data, mbytes.len, &inc) == 0) {
                char pkey[MZCC_SHA256_HEX_LEN + 1];
                /* Recompute preprocessed_key from LIVE header bytes (re-anchored
                   to this invocation's roots): a changed or removed recorded
                   header yields a key that misses every stored entry, so a stale
                   hit is unreachable by construction. */
                if (compute_preprocessed_key(mkey, &inc, src_dir, pkey)) {
                    ByteBuf ppbytes;
                    if (mzcc_cache_pp_lookup(pkey, &ppbytes)) {
                        pp.exit_code = 0;
                        pp.stdout_bytes = ppbytes; /* transfer ownership */
                        byte_buf_init(&pp.stderr_bytes);
                        pp_served = 1;
                        cache_log("cpp-hit", tag);
                    }
                }
            }
            sl_free(&inc);
            byte_buf_free(&mbytes);
        }
    }

    int ran = 1;
    if (!pp_served) {
        char *depfile = NULL;
        if (pp_on) {
            depfile = joinstr(OBJ_DIR, "/", tag, ".ppd");
            av_add(&cpp, "-MD");
            av_add(&cpp, "-MF"); av_add(&cpp, depfile);
            av_add(&cpp, "-MT"); av_add(&cpp, "tu");
            cache_log("cpp-miss", tag);
        }
        av_add(&cpp, "-");
        ran = run_proc(CPP, cpp.v, cpp.n, fed.data, fed.len, CPP_CWD, &pp);
        if (ran && pp.exit_code == 0 && pp_on && depfile) {
            /* Discover the include set from the depfile and populate both
               entries. Both stores are best-effort: a failure just means the
               next build re-preprocesses. */
            ByteBuf db;
            if (read_file(depfile, &db) == 0) {
                StrList raw;
                sl_init(&raw);
                parse_depfile(db.data, db.len, &raw);
                /* Re-express each discovered absolute include as a STABLE,
                   re-anchorable reference. If ANY include lies outside both -I
                   search roots it is unmappable: decline to cache this TU (leave
                   no manifest), so it safely re-preprocesses every build rather
                   than risk a wrong hit on a reference we cannot re-anchor. */
                StrList inc;
                sl_init(&inc);
                int mappable = 1;
                for (int k = 0; k < raw.n; ++k) {
                    char *enc = encode_include(raw.v[k], src_dir);
                    if (!enc) { mappable = 0; break; }
                    sl_push(&inc, enc);
                    free(enc);
                }
                sl_free(&raw);
                char pkey[MZCC_SHA256_HEX_LEN + 1];
                if (mappable && compute_preprocessed_key(mkey, &inc, src_dir, pkey)) {
                    mzcc_cache_pp_store(pkey, pp.stdout_bytes.data, pp.stdout_bytes.len);
                    ByteBuf mser;
                    serialize_manifest_into(&inc, &mser);
                    mzcc_cache_manifest_store(mkey, mser.data, mser.len);
                    byte_buf_free(&mser);
                }
                sl_free(&inc);
            }
            byte_buf_free(&db);
            remove(depfile);
        }
        free(depfile);
    }
    av_free(&cpp);
    byte_buf_free(&fed);
    free(src_dir);
    free(src_id);
    if (!ran || pp.exit_code != 0) {
        diag_printf(diag, "mzcc: cpp failed for %s\n", tag);
        diag_bytes(diag, pp.stderr_bytes.data, pp.stderr_bytes.len);
        byte_buf_free(&pp.stdout_bytes);
        byte_buf_free(&pp.stderr_bytes);
        return NULL;
    }
    byte_buf_free(&pp.stderr_bytes);

    /* ---- content-addressed object cache (maize-274, spec 2d) -------------
       The cpp boundary is the key boundary: the preprocessed bytes are the
       flattened #include closure, so a header/-D/-I/source change all roll the
       key. The object tag carries the ".body" infix (the same obj_tag the mazm
       stage names below), and is folded into the key so a served object is
       bit-identical to a fresh compile for that exact tag. cpp already ran (its
       output IS the key), so only cproc-qbe/normalize/qbe/mazm are skipped on a
       hit. --emit bypasses the cache (the qbe body is a side output the cache
       does not store). */
    char *obj_tag = joinstr(tag, ".body", NULL, NULL);
    char *mzo_dst = joinstr(OBJ_DIR, "/", obj_tag, ".mzo");
    int cache_on = mzcc_cache_enabled() && emit_body == NULL;
    char key[MZCC_SHA256_HEX_LEN + 1];
    if (cache_on) {
        mzcc_cache_key(pp.stdout_bytes.data, pp.stdout_bytes.len, obj_tag, key);
        if (mzcc_cache_lookup(key, mzo_dst)) {
            cache_log("hit", obj_tag);
            byte_buf_free(&pp.stdout_bytes);
            free(obj_tag);
            return mzo_dst; /* cproc-qbe / qbe / mazm all skipped */
        }
        cache_log("miss", obj_tag);
    }

    /* Stage: cproc-qbe (C11 -> QBE IL), stdin = cpp stdout. */
    Argv cq;
    av_init(&cq);
    av_add(&cq, CPROC_QBE);
    ProcResult ssa;
    ran = run_proc(CPROC_QBE, cq.v, cq.n, pp.stdout_bytes.data, pp.stdout_bytes.len, NULL, &ssa);
    av_free(&cq);
    byte_buf_free(&pp.stdout_bytes);
    if (!ran || ssa.exit_code != 0) {
        diag_printf(diag, "mzcc: cproc-qbe failed for %s\n", tag);
        diag_bytes(diag, ssa.stderr_bytes.data, ssa.stderr_bytes.len);
        byte_buf_free(&ssa.stdout_bytes);
        byte_buf_free(&ssa.stderr_bytes);
        free(obj_tag); free(mzo_dst);
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
        diag_printf(diag, "mzcc: qbe -t maize failed for %s\n", tag);
        diag_bytes(diag, body.stderr_bytes.data, body.stderr_bytes.len);
        byte_buf_free(&body.stdout_bytes);
        byte_buf_free(&body.stderr_bytes);
        free(obj_tag); free(mzo_dst);
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
    char *mzo = assemble_stdin(body.stdout_bytes.data, body.stdout_bytes.len, obj_tag, diag);
    byte_buf_free(&body.stdout_bytes);

    /* Store on a fully-successful .mzo only (spec 2c): a failed pipeline above
       returned early and never reaches here, so the cache never holds a torn or
       partial object. Store is best-effort; a failure just means the next build
       recompiles. */
    if (mzo && cache_on) {
        mzcc_cache_store(key, mzo);
    }
    free(obj_tag);
    free(mzo_dst);
    return mzo;
}

/* Case-fold one ASCII byte to upper-case (matching the ::toupper mazm's own
   tokenizer applies at src/mazm.cpp:922; see mazm_has_include below). Plain
   ASCII is enough: mazm's keyword scan is ASCII-only. */
static char ascii_toupper(char c) {
    return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

/* Does a to-be-cached .mazm carry an INCLUDE directive (maize-37)? The asm
   cache keys on the RAW .mazm bytes, which do NOT capture an included file's
   contents, so a cached object built from an INCLUDE-using .mazm could be served
   stale when the included file changes. Resolved OQ 9662: DETECT INCLUDE and
   bypass the cache for that TU (never store or serve), rather than fold the
   include closure into the key (deferred). Detection is a case-INSENSITIVE
   whole-word scan (cycle-1 review fix, Convention counterexamples Entry 20):
   mazm's own directive dispatch upper-cases the candidate keyword before the
   table lookup (src/mazm.cpp:922, `std::transform(..., ::toupper)`), so
   `include`, `Include`, and `INCLUDE` are all honored as the same directive.
   A scan for only the upper-case literal would miss every other spelling,
   letting a lower/mixed-case include splice bypass detection and serve a
   stale object. A false positive (the word inside a comment, in any case)
   only forgoes caching for that TU, which is always safe: over-matching here
   has no downside. The include-free RT set (crt0/syscall/setjmp/mzdev)
   caches normally regardless of case. */
static int mazm_has_include(const char *bytes, size_t len) {
    const char *kw = "INCLUDE";
    size_t klen = 7;
    for (size_t i = 0; i + klen <= len; ++i) {
        int match = 1;
        for (size_t k = 0; k < klen; ++k) {
            if (ascii_toupper(bytes[i + k]) != kw[k]) {
                match = 0;
                break;
            }
        }
        if (!match) {
            continue;
        }
        char before = (i == 0) ? ' ' : bytes[i - 1];
        char after = (i + klen < len) ? bytes[i + klen] : ' ';
        int lhs_delim = !((before >= 'A' && before <= 'Z') ||
                          (before >= 'a' && before <= 'z') ||
                          (before >= '0' && before <= '9') || before == '_');
        int rhs_delim = !((after >= 'A' && after <= 'Z') ||
                          (after >= 'a' && after <= 'z') ||
                          (after >= '0' && after <= '9') || after == '_');
        if (lhs_delim && rhs_delim) {
            return 1;
        }
    }
    return 0;
}

/* Assemble a .mazm file at `mazm_path` to a .mzo tagged `tag` (maize-280
   generalization of assemble_rt_asm, which hardcoded RT_DIR/<name>.mazm). Read
   the .mazm and feed it through the same mazm stdin-to-object path (no copy into
   scratch needed, cc-maize.sh:479-488). Returns a fresh .mzo path, or NULL. */
char *assemble_mazm_file(const char *mazm_path, const char *tag, ByteBuf *diag) {
    ByteBuf b;
    if (read_file(mazm_path, &b) != 0) {
        diag_printf(diag, "mzcc: cannot read asm module %s\n", mazm_path);
        byte_buf_free(&b);
        return NULL;
    }

    /* Asm-path object cache (maize-274, spec 2d): key = SHA-256(fingerprint ||
       raw .mazm bytes || tag). Bypass entirely on an INCLUDE-using .mazm (OQ
       9662) so an object whose include closure is not in the key is never
       stored or served. */
    int cache_on = mzcc_cache_enabled() && !mazm_has_include(b.data, b.len);
    char *mzo_dst = joinstr(OBJ_DIR, "/", tag, ".mzo");
    char key[MZCC_SHA256_HEX_LEN + 1];
    if (cache_on) {
        mzcc_cache_key(b.data, b.len, tag, key);
        if (mzcc_cache_lookup(key, mzo_dst)) {
            cache_log("hit", tag);
            byte_buf_free(&b);
            return mzo_dst; /* mazm spawn skipped */
        }
        cache_log("miss", tag);
    }

    char *mzo = assemble_stdin(b.data, b.len, tag, diag);
    byte_buf_free(&b);
    if (mzo && cache_on) {
        mzcc_cache_store(key, mzo);
    }
    free(mzo_dst);
    return mzo;
}

/* ---- reusable build core: the parallel object build (maize-274) ---------
   The RT asm set + optional mzdev + the libc C modules + the user TUs form ONE
   independent job set feeding a single link. compile_tu_ex / assemble_mazm_file
   are the DI9 pure-function seam (no shared mutable driver state), so workers
   are thread-safe as-is; the only shared writable resource, OBJ_DIR, receives a
   distinct filename per job (tags are unique). Both former serial loops
   (build_rt_objects / compile_user_sources) collapse into ONE job builder here
   so the single-file path and every batch subcommand share it (spec section 4).

   Determinism (spec 3c): only compilation is parallelized. Each job writes its
   own pre-assigned slot; the object vector is read back in canonical job order
   (mzdev when --dev, then user in source order; the runtime members now come
   from the prebuilt archive, maize-302), INDEPENDENT of completion order, so the
   linked .mzx is byte-identical to the serial (MAIZE_JOBS=1) build. */

typedef enum { JOB_ASM, JOB_C } JobKind;

typedef struct {
    JobKind      kind;
    char        *src;    /* owned: .mazm path (ASM) or .c path (C) */
    char        *tag;    /* owned object tag */
    const Argv  *extra;  /* borrowed cpp -D tokens (C jobs; may be NULL) */
    int          want_emit;
    /* outputs, each written only by this job's own worker (no sharing): */
    char        *mzo;    /* owned result path; NULL on failure */
    ByteBuf      emit;    /* qbe body, when want_emit */
    int          have_emit;
    ByteBuf      diag;    /* captured failure diagnostics (spec 3d) */
    int          ok;
} Job;

/* The per-index worker (MzJobFn): run one job into its own slots. */
static int run_one_job(void *ctx, int i) {
    Job *j = &((Job *)ctx)[i];
    byte_buf_init(&j->diag);
    j->have_emit = 0;
    if (j->kind == JOB_ASM) {
        j->mzo = assemble_mazm_file(j->src, j->tag, &j->diag);
    } else {
        ByteBuf *emitp = j->want_emit ? &j->emit : NULL;
        j->mzo = compile_tu_ex(j->src, j->tag, j->extra, emitp, &j->diag);
        if (j->mzo && emitp) { j->have_emit = 1; }
    }
    j->ok = (j->mzo != NULL);
    return j->ok ? 0 : 1;
}

/* Concurrency cap (spec 3b): explicit -j / MAIZE_JOBS wins; else max(2,
   nproc-2), fallback 4 when nproc is unknown; then clamped to 1..job_count.
   MAIZE_JOBS=1 forces the serial parity reference. */
static int resolve_job_cap(int job_count) {
    int explicit_cap = 0;
    if (g_jobs_override > 0) {
        explicit_cap = g_jobs_override;
    } else {
        const char *e = getenv("MAIZE_JOBS");
        if (e && e[0]) {
            int v = atoi(e);
            if (v > 0) { explicit_cap = v; }
        }
    }
    int cap;
    if (explicit_cap > 0) {
        cap = explicit_cap;
    } else {
        int np = mzcc_nproc();
        if (np > 0) {
            cap = np - 2;
            if (cap < 2) { cap = 2; }
        } else {
            cap = 4;
        }
    }
    if (cap < 1) { cap = 1; }
    if (cap > job_count) { cap = job_count; }
    return cap;
}

static void free_jobs(Job *jobs, int njobs) {
    for (int i = 0; i < njobs; ++i) {
        free(jobs[i].src);
        free(jobs[i].tag);
        free(jobs[i].mzo);
        byte_buf_free(&jobs[i].emit);
        byte_buf_free(&jobs[i].diag);
    }
    free(jobs);
}

/* Build the full object set (RT asm + optional mzdev + libc + user TUs) in
   parallel and fill `out_objs` (already sl_init'd) in canonical link order.
   `extra_defines` (nullable) applies to the libc C modules and the user sources
   (never the asm jobs), exactly as cc-maize.sh applies its command-line -D flags.
   `emit_single` (nullable, single-source only) receives the qbe body of the sole
   user TU; *out_have_emit is set accordingly (the cache is bypassed for that TU).
   Returns 0 on success, 1 on any compile/assemble failure (failed-job diagnostics
   are flushed to stderr in canonical order first). */
static int build_objects_parallel(int dev, const Argv *extra_defines,
                                  const StrList *sources, int used_sources,
                                  ByteBuf *emit_single, int *out_have_emit,
                                  StrList *out_objs) {
    if (out_have_emit) { *out_have_emit = 0; }
    int multi = (sources->n >= 2) || used_sources;
    /* maize-302: crt0 + the freestanding libc are no longer compiled on the
       per-program path. They are prebuilt ONCE into the runtime archive (.mza)
       by ensure_runtime_archive and linked by mzld at the link sites. This is
       where the 473 RT-C cpp spawns (11 libc modules x 43 programs, maize-301's
       single largest structural cost) and the 129 RT-asm mazm spawns leave the
       warm build. The per-program object set is now just the conditional mzdev
       asm object (decision D3: mzdev stays per-build, it is --dev-only asm and
       contributes zero cpp spawns) plus the user TUs. extra_defines still
       reaches the user TUs; it no longer reaches libc (decision D1 / OQ1: the
       archive is built with the single canonical define set). */
    int n_dev = dev ? 1 : 0;
    int njobs = n_dev + sources->n;

    Job *jobs = (Job *)xmalloc((size_t)njobs * sizeof(Job));
    memset(jobs, 0, (size_t)njobs * sizeof(Job));

    int idx = 0;
    if (dev) {
        jobs[idx].kind = JOB_ASM;
        jobs[idx].src = joinstr(RT_DIR, "/mzdev.mazm", NULL, NULL);
        jobs[idx].tag = xstrdup("mzdev");
        ++idx;
    }
    /* User TUs (source order): multi -> u<i>_<base>, single -> bare base. */
    for (int i = 0; i < sources->n; ++i) {
        char *base = base_noext_c(sources->v[i]);
        jobs[idx].kind = JOB_C;
        jobs[idx].src = xstrdup(sources->v[i]);
        if (multi) {
            char pfx[32];
            snprintf(pfx, sizeof(pfx), "u%d_", i);
            jobs[idx].tag = joinstr(pfx, base, NULL, NULL);
        } else {
            jobs[idx].tag = xstrdup(base);
            jobs[idx].want_emit = (emit_single != NULL);
        }
        jobs[idx].extra = extra_defines;
        free(base);
        ++idx;
    }

    int cap = resolve_job_cap(njobs);
    int rc = mzcc_run_jobs(run_one_job, jobs, njobs, cap);

    /* Flush ONLY the lowest-index failed job's captured diagnostics (spec 3d:
       "the driver reports the LOWEST-index failed job's captured stderr").
       Cycle-1 review fix: this used to flush every failed job's diagnostics,
       which is deterministic (canonical order) but not what the spec asks
       for and noisier than the single deterministic report the spec wants
       when several jobs happen to fail together. */
    for (int i = 0; i < njobs; ++i) {
        if (!jobs[i].ok) {
            if (jobs[i].diag.len) {
                fwrite(jobs[i].diag.data, 1, jobs[i].diag.len, stderr);
            }
            break;
        }
    }

    if (rc != 0) {
        free_jobs(jobs, njobs);
        return 1;
    }

    /* Assemble the object vector in canonical (job) order, never completion
       order: this is the byte-identity guarantee vs the serial build. */
    for (int i = 0; i < njobs; ++i) {
        sl_push(out_objs, jobs[i].mzo);
    }
    if (emit_single && njobs > 0 && jobs[njobs - 1].want_emit && jobs[njobs - 1].have_emit) {
        byte_buf_init(emit_single);
        byte_buf_append(emit_single, jobs[njobs - 1].emit.data, jobs[njobs - 1].emit.len);
        if (out_have_emit) { *out_have_emit = 1; }
    }
    free_jobs(jobs, njobs);
    return 0;
}

/* ---- prebuilt runtime archive (maize-302) -------------------------------

   Build crt0 + the freestanding libc ONCE into a linkable archive (.mza), keyed
   so every program links the prebuilt runtime instead of recompiling it. The
   container is an ar-style member archive (decision D2): each runtime .mzo
   member verbatim, plus a member index, under its own magic. It is an internal,
   replaceable choice (design section 5, the DIRT ruling): the .mzo format and
   the ABI are the stability contract, not this container. v1 links the WHOLE
   archive (member selection deferred to maize-306, OQ2); mzld expands every
   member in declared order, placed first (before the user objects) so the
   layout matches the from-source build.

   The .mza layout constants mirror src/maize_obj.h's maize::obj MZA_* (that C++
   header is the format's source of truth; mzld reads what mzcc writes). This C
   file cannot include the C++ header, so the literals are duplicated here, the
   same way verify_mzx_image hardcodes the 'M','Z','X' magic. */
#define MZA_MAGIC0 'M'
#define MZA_MAGIC1 'Z'
#define MZA_MAGIC2 'A'
#define MZA_VERSION 0x01
#define MZA_HEADER_SIZE 16u
#define MZA_INDEX_ENTRY_SIZE 24u

static void put_u16le(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}
static void put_u32le(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; ++i) { p[i] = (uint8_t)((v >> (8 * i)) & 0xFF); }
}
static void put_u64le(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; ++i) { p[i] = (uint8_t)((v >> (8 * i)) & 0xFF); }
}

/* Absorb a length-prefixed byte run into the running SHA-256 (the length prefix
   stops two adjacent fields aliasing by a boundary shift, mirroring
   mzcc_cache.c's fp_absorb_file). */
static void arc_absorb(mzcc_sha256_ctx *c, const void *data, size_t len) {
    uint8_t lp[8];
    for (int i = 0; i < 8; ++i) { lp[i] = (uint8_t)((uint64_t)len >> (56 - i * 8)); }
    mzcc_sha256_update(c, lp, 8);
    mzcc_sha256_update(c, data ? data : "", len);
}
static void arc_absorb_str(mzcc_sha256_ctx *c, const char *s) {
    arc_absorb(c, s, s ? strlen(s) : 0);
}
static int arc_absorb_file(mzcc_sha256_ctx *c, const char *path) {
    ByteBuf b;
    if (read_file(path, &b) != 0) {
        byte_buf_free(&b);
        return -1;
    }
    arc_absorb(c, b.data, b.len);
    byte_buf_free(&b);
    return 0;
}
static int arc_cmp_names(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* Hash the byte contents of every regular file under `dir` (recursively), in a
   deterministic sorted order, into `c`. This over-covers the exact cpp include
   closure of the runtime sources (it also folds in headers, mzdev, and docs
   under toolchain/rt), which is the SAFE direction: an unrelated edit only
   forces one extra archive rebuild, never serves a stale archive. Editing any
   runtime header therefore rolls the archive key, which the coarser
   "raw .c bytes only" key would miss. Returns 0 on success, -1 on any read
   failure (treated as a hard error so a rolled key is never silently skipped). */
static int arc_absorb_tree(mzcc_sha256_ctx *c, const char *dir, const char *relprefix) {
    StrList ents;
    sl_init(&ents);
    if (list_dir(dir, &ents) != 0) {
        sl_free(&ents);
        return -1;
    }
    if (ents.n > 1) {
        qsort(ents.v, (size_t)ents.n, sizeof(char *), arc_cmp_names);
    }
    int rc = 0;
    for (int i = 0; i < ents.n; ++i) {
        char *full = joinstr(dir, "/", ents.v[i], NULL);
        char *rel = (relprefix && relprefix[0])
            ? joinstr(relprefix, "/", ents.v[i], NULL) : xstrdup(ents.v[i]);
        if (dir_exists(full)) {
            arc_absorb_str(c, rel);
            if (arc_absorb_tree(c, full, rel) != 0) { rc = -1; }
        } else if (is_regular_file(full)) {
            arc_absorb_str(c, rel);
            if (arc_absorb_file(c, full) != 0) { rc = -1; }
        }
        free(full);
        free(rel);
    }
    sl_free(&ents);
    return rc;
}

/* Derive the runtime-archive cache key (spec "When the archive is built"):
   content-addressed over the toolchain fingerprint (tool identities), the
   canonical define set, the member list identity, and the RT source tree bytes.
   Returns 0 and fills `out` on success, -1 if the fingerprint is unavailable or
   a source file cannot be read. */
static int compute_runtime_archive_key(char out[MZCC_SHA256_HEX_LEN + 1]) {
    uint8_t fp[MZCC_SHA256_DIGEST_LEN];
    if (!mzcc_cache_fingerprint(fp)) {
        return -1;
    }
    mzcc_sha256_ctx c;
    mzcc_sha256_init(&c);
    /* Domain tag so an archive key can never share a preimage with a per-TU
       object-cache key. */
    arc_absorb_str(&c, "mza-v1");
    arc_absorb(&c, fp, sizeof(fp));
    /* Canonical define set: the process-global EXTRA_CPPDEFS (empty for every
       current caller). The fixed cpp defines (compile_tu_ex) are baked into the
       mzcc binary and thus already covered by the fingerprint's self-hash.
       Per-build -D (extra_defines) deliberately does NOT enter the key: the
       archive is built with the single canonical define set (decision D1/OQ1). */
    for (int i = 0; i < EXTRA_CPPDEFS.n; ++i) {
        arc_absorb_str(&c, EXTRA_CPPDEFS.v[i]);
    }
    /* Member list identity, order-sensitive (the archive's declared order). */
    for (size_t i = 0; i < sizeof(RT_ASM) / sizeof(RT_ASM[0]); ++i) {
        arc_absorb_str(&c, RT_ASM[i]);
    }
    for (size_t i = 0; i < sizeof(RT_C) / sizeof(RT_C[0]); ++i) {
        arc_absorb_str(&c, RT_C[i]);
    }
    /* The runtime source tree bytes (sources + the header include closure). */
    if (arc_absorb_tree(&c, RT_DIR, "") != 0) {
        return -1;
    }
    uint8_t dig[MZCC_SHA256_DIGEST_LEN];
    mzcc_sha256_final(&c, dig);
    mzcc_sha256_hex(dig, out);
    return 0;
}

/* Compile the archive members (crt0, syscall, setjmp, then the 11 RT_C libc
   modules in declared order) through the SAME parallel + cached pipeline the
   per-program build uses (compile_tu_ex / assemble_mazm_file via run_one_job),
   with the canonical define set (extra=NULL). The maize-290 stable __FILE__
   identity applies exactly as on the per-program path (compile_tu_ex is shared),
   so members are byte-reproducible (AC8). Hands back parallel tag/obj arrays in
   declared order. Returns 0 on success, -1 on any member failure. */
static int build_archive_members(char ***out_tags, char ***out_objs, int *out_n) {
    int n_asm = (int)(sizeof(RT_ASM) / sizeof(RT_ASM[0]));
    int n_rtc = (int)(sizeof(RT_C) / sizeof(RT_C[0]));
    int n = n_asm + n_rtc;

    Job *jobs = (Job *)xmalloc((size_t)n * sizeof(Job));
    memset(jobs, 0, (size_t)n * sizeof(Job));

    int idx = 0;
    for (int i = 0; i < n_asm; ++i) {
        jobs[idx].kind = JOB_ASM;
        jobs[idx].src = joinstr(RT_DIR, "/", RT_ASM[i], ".mazm");
        jobs[idx].tag = xstrdup(RT_ASM[i]);
        ++idx;
    }
    for (int i = 0; i < n_rtc; ++i) {
        jobs[idx].kind = JOB_C;
        jobs[idx].src = joinstr(RT_DIR, "/", RT_C[i], ".c");
        jobs[idx].tag = joinstr("rt_", RT_C[i], NULL, NULL);
        jobs[idx].extra = NULL; /* canonical define set (D1/OQ1) */
        ++idx;
    }

    int cap = resolve_job_cap(n);
    int rc = mzcc_run_jobs(run_one_job, jobs, n, cap);

    /* Flush the lowest-index failed job's diagnostics (same discipline as
       build_objects_parallel). */
    for (int i = 0; i < n; ++i) {
        if (!jobs[i].ok) {
            if (jobs[i].diag.len) { fwrite(jobs[i].diag.data, 1, jobs[i].diag.len, stderr); }
            break;
        }
    }
    if (rc != 0) {
        free_jobs(jobs, n);
        return -1;
    }

    char **tags = (char **)xmalloc((size_t)n * sizeof(char *));
    char **objs = (char **)xmalloc((size_t)n * sizeof(char *));
    for (int i = 0; i < n; ++i) {
        tags[i] = xstrdup(jobs[i].tag);
        objs[i] = xstrdup(jobs[i].mzo);
    }
    free_jobs(jobs, n);
    *out_tags = tags;
    *out_objs = objs;
    *out_n = n;
    return 0;
}

/* Serialize the built member .mzo blobs (paths in `objs`, tags in `tags`, in
   declared order) into a .mza at `out_path`. Deterministic concatenation of the
   already-deterministic member bytes plus a deterministic index (AC8). Returns
   0 on success, -1 on failure. */
static int write_runtime_archive(const char *out_path, char **tags, char **objs, int n) {
    ByteBuf *blobs = (ByteBuf *)xmalloc((size_t)n * sizeof(ByteBuf));
    for (int i = 0; i < n; ++i) {
        if (read_file(objs[i], &blobs[i]) != 0) {
            fprintf(stderr, "mzcc: cannot read runtime member object %s\n", objs[i]);
            byte_buf_free(&blobs[i]);
            for (int j = 0; j < i; ++j) { byte_buf_free(&blobs[j]); }
            free(blobs);
            return -1;
        }
    }

    size_t index_off = MZA_HEADER_SIZE;
    size_t index_size = (size_t)n * MZA_INDEX_ENTRY_SIZE;
    size_t strtab_off = index_off + index_size;

    ByteBuf strtab;
    byte_buf_init(&strtab);
    uint32_t *name_off = (uint32_t *)xmalloc((size_t)n * sizeof(uint32_t));
    for (int i = 0; i < n; ++i) {
        name_off[i] = (uint32_t)(strtab_off + strtab.len);
        byte_buf_append(&strtab, tags[i], strlen(tags[i]));
        char z = 0;
        byte_buf_append(&strtab, &z, 1);
    }

    size_t members_off = strtab_off + strtab.len;
    uint64_t *moff = (uint64_t *)xmalloc((size_t)n * sizeof(uint64_t));
    size_t running = members_off;
    for (int i = 0; i < n; ++i) {
        moff[i] = running;
        running += blobs[i].len;
    }
    size_t total = running;

    uint8_t *buf = (uint8_t *)xmalloc(total);
    memset(buf, 0, total);
    buf[0] = MZA_MAGIC0;
    buf[1] = MZA_MAGIC1;
    buf[2] = MZA_MAGIC2;
    buf[3] = MZA_VERSION;
    put_u16le(buf + 4, 0);
    put_u16le(buf + 6, (uint16_t)n);
    put_u64le(buf + 8, (uint64_t)index_off);
    for (int i = 0; i < n; ++i) {
        uint8_t *e = buf + index_off + (size_t)i * MZA_INDEX_ENTRY_SIZE;
        put_u32le(e + 0, name_off[i]);
        put_u32le(e + 4, 0);
        put_u64le(e + 8, (uint64_t)moff[i]);
        put_u64le(e + 16, (uint64_t)blobs[i].len);
    }
    memcpy(buf + strtab_off, strtab.data, strtab.len);
    for (int i = 0; i < n; ++i) {
        if (blobs[i].len) { memcpy(buf + moff[i], blobs[i].data, blobs[i].len); }
    }

    int wrc = write_file(out_path, (const char *)buf, total);

    for (int i = 0; i < n; ++i) { byte_buf_free(&blobs[i]); }
    free(blobs);
    byte_buf_free(&strtab);
    free(name_off);
    free(moff);
    free(buf);
    return wrc == 0 ? 0 : -1;
}

/* Process-wide memo (decision D4): within one mzcc process (a batch build), the
   archive key is stable across programs, so hash-and-check once and reuse the
   built archive rather than re-hashing the RT tree for all 42 programs. */
static char *g_rt_archive_key = NULL;
static char *g_rt_archive_path = NULL;

static void remember_archive(const char *key, const char *path) {
    free(g_rt_archive_key);
    g_rt_archive_key = xstrdup(key);
    free(g_rt_archive_path);
    g_rt_archive_path = xstrdup(path);
}

/* Ensure the prebuilt runtime archive exists and is current, and return a fresh
   heap path to it (caller frees). Built lazily and idempotently, content
   addressed on the RT source bytes + tool identities + canonical define set
   (decision D4). On a warm build this is a key check plus a cache copy, so NO
   cpp/mazm runs for any runtime source (the 473-spawn win). Requires
   resolve_toolchain + ensure_scratch to have run. Returns NULL on failure. */
static char *ensure_runtime_archive(void) {
    char key[MZCC_SHA256_HEX_LEN + 1];
    if (compute_runtime_archive_key(key) != 0) {
        fprintf(stderr, "mzcc: could not derive the runtime-archive key "
                        "(toolchain fingerprint or RT sources unavailable)\n");
        return NULL;
    }

    if (g_rt_archive_key && strcmp(g_rt_archive_key, key) == 0
            && g_rt_archive_path && path_exists(g_rt_archive_path)) {
        return xstrdup(g_rt_archive_path);
    }

    char *scratch_mza = path_join(OBJ_DIR, "runtime.mza");

    /* Content-addressed hit: link the cached archive without preprocessing or
       compiling a single runtime source. */
    if (mzcc_cache_enabled() && mzcc_cache_archive_lookup(key, scratch_mza)) {
        remember_archive(key, scratch_mza);
        return scratch_mza;
    }

    /* Miss (or object cache disabled): build the members ONCE, pack the .mza,
       and (when caching) store it for every subsequent build. */
    char **tags = NULL;
    char **objs = NULL;
    int n = 0;
    if (build_archive_members(&tags, &objs, &n) != 0) {
        fprintf(stderr, "mzcc: FAILED building the runtime archive members\n");
        free(scratch_mza);
        return NULL;
    }
    int wrc = write_runtime_archive(scratch_mza, tags, objs, n);
    for (int i = 0; i < n; ++i) { free(tags[i]); free(objs[i]); }
    free(tags);
    free(objs);
    if (wrc != 0) {
        fprintf(stderr, "mzcc: FAILED writing the runtime archive\n");
        free(scratch_mza);
        return NULL;
    }
    if (mzcc_cache_enabled()) {
        mzcc_cache_archive_store(key, scratch_mza);
    }
    remember_archive(key, scratch_mza);
    return scratch_mza;
}

/* Resolve REPO_ROOT + RT_DIR once (idempotent). MAIZE_ROOT override, else two
   directory levels up from the mzcc exe (built to build/<preset>/mzcc), mirroring
   cc-maize.sh's script-relative self-location (decision DI9). */
void ensure_repo_root(void) {
    if (REPO_ROOT) { return; }
    const char *root_env = getenv("MAIZE_ROOT");
    if (root_env && root_env[0]) {
        REPO_ROOT = xstrdup(root_env);
        to_slashes(REPO_ROOT);
    } else {
        char selfpath[4096];
        if (mzcc_self_path(g_argv0, selfpath, sizeof(selfpath)) != 0) {
            die("cannot locate the mzcc executable; set MAIZE_ROOT to the repo root");
        }
        to_slashes(selfpath);
        REPO_ROOT = xstrdup(selfpath);
        strip_last_component(REPO_ROOT);   /* drop /mzcc[.exe] -> build/<preset> */
        strip_last_component(REPO_ROOT);   /* drop /<preset>   -> build */
        strip_last_component(REPO_ROOT);   /* drop /build      -> repo root */
    }
    RT_DIR = joinstr(REPO_ROOT, "/toolchain/rt", NULL, NULL);
}

/* Resolve every toolchain binary + the preprocessor for `preset`, exactly as
   main()'s tool-discovery block does. Prints the same actionable messages and
   returns 2 on the first tool-not-found; 0 on success. Idempotent: re-resolving
   the same preset frees and re-sets the tool globals (a batch loop calls this
   once per program, as cc-maize.sh re-resolves per invocation). */
int resolve_toolchain(const char *preset) {
    ensure_repo_root();

    free(BUILD_DIR);
    BUILD_DIR = joinstr(REPO_ROOT, "/build/", preset, NULL);

    /* Tool discovery (host-aware .exe resolution, maize-257). */
    free(CPROC_QBE);
    CPROC_QBE = resolve_exe(joinstr(REPO_ROOT, "/toolchain/cproc/cproc-qbe", NULL, NULL));
    if (!CPROC_QBE) { fprintf(stderr, "mzcc: cproc-qbe not found; run 'mzcc --build' (cproc/qbe).\n"); return 2; }
    free(QBE);
    QBE = resolve_exe(joinstr(REPO_ROOT, "/toolchain/qbe/obj/qbe", NULL, NULL));
    if (!QBE) { fprintf(stderr, "mzcc: qbe not found; run 'mzcc --build' (cproc/qbe).\n"); return 2; }
    free(MAZM);
    MAZM = resolve_exe(path_join(BUILD_DIR, "mazm"));
    if (!MAZM) { fprintf(stderr, "mzcc: mazm not found in %s; run scripts/install-mazm.sh (or run-tests.sh) first.\n", BUILD_DIR); return 2; }
    free(MAIZE);
    MAIZE = resolve_exe(path_join(BUILD_DIR, "maize"));
    if (!MAIZE) { fprintf(stderr, "mzcc: maize not found in %s; run scripts/install-mazm.sh (or run-tests.sh) first.\n", BUILD_DIR); return 2; }
    free(MZLD);
    MZLD = resolve_exe(path_join(BUILD_DIR, "mzld"));
    if (!MZLD) { fprintf(stderr, "mzcc: mzld not found in %s; run scripts/install-mazm.sh (or run-tests.sh) first.\n", BUILD_DIR); return 2; }

    /* Preprocessor discovery (cc-maize.sh:314-331, decision D4): $CC, else cc,
       else gcc, else (Windows) the vendored llvm-mingw clang. The section-3c
       macro matrix is applied identically regardless of which candidate wins. */
    free(CPP);
    CPP = NULL;
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
            fprintf(stderr, "mzcc: no C preprocessor (cc/gcc) found for #include expansion. On Windows,\n"
                            "       run scripts/bootstrap-toolchain.ps1 to fetch the vendored llvm-mingw clang.\n");
            return 2;
        }
    }

    /* Register the post-cpp tool binaries with the object cache (maize-274): the
       fingerprint over their raw bytes plus the running mzcc rolls every cache
       key on a tool rebuild/re-pin, so a stale object is never served (AC 9637).
       cpp is excluded (its effect is captured by the preprocessed bytes). */
    mzcc_cache_configure(CPROC_QBE, QBE, MAZM);

    /* Resolve the direct-mode cpp-output cache identity (maize-303): hash the
       resolved CPP file bytes + `<CPP> --version` output ONCE, here, on the
       main thread before any worker spawns (same serial-before-workers
       guarantee mzcc_cache_configure relies on). Fail-closed: if no real
       identity is obtainable, g_ppcache_disabled is set and the cpp cache is
       bypassed for the whole build. Also warms the RT_DIR shadow-guard
       listing. cpp is excluded from the object-cache fingerprint above; this is
       where its identity enters the direct-mode key. */
    ppcache_configure();

    /* Resolve MAIZE_CACHE_STATS here too (cycle-2 review finding 2): same
       serial-before-any-worker guarantee as mzcc_cache_configure above, so
       g_cache_stats_on is never first-written from a scheduler worker. */
    mzcc_cache_stats_resolve();
    return 0;
}

/* Create the scratch dirs once (idempotent): OBJ_DIR holds the .mzo objects and
   the linked image; CPP_CWD is an empty dir that becomes cpp's implicit
   quote-include base (DI6). Registers the exit-time cleanup. */
void ensure_scratch(void) {
    if (SCRATCH_ROOT) { return; }
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
}

/* MZX-magic + minimum-size validation (the verify_mzx equivalent, applied
   uniformly across all three subcommands including build-quesos, decision
   DI 9624). Returns 0 when `path` is a well-formed .mzx image, 1 otherwise. */
int verify_mzx_image(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0 || (st.st_mode & S_IFMT) != S_IFREG) {
        fprintf(stderr, "mzcc: MISSING output %s\n", path);
        return 1;
    }
    if (st.st_size < 24) {
        fprintf(stderr, "mzcc: output %s too small (%ld bytes, need >= 24-byte header)\n",
                path, (long)st.st_size);
        return 1;
    }
    char magic[3] = {0, 0, 0};
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "mzcc: MISSING output %s\n", path);
        return 1;
    }
    size_t got = fread(magic, 1, 3, f);
    fclose(f);
    if (got != 3 || magic[0] != 'M' || magic[1] != 'Z' || magic[2] != 'X') {
        fprintf(stderr, "mzcc: output %s is not a .mzx image (bad magic)\n", path);
        return 1;
    }
    return 0;
}

/* The link-profile seam (section 8 / DI9): link `objects[0..n_objects)` (in link
   order) to `out_path`. `base_hex` NULL uses mzld's default base; non-NULL is
   passed as `mzld -b <base_hex>`. This is the ENTIRE link-profile mechanism: a
   profile is (base address, object list), not a registry. Returns 0/1. */
int mzld_link(const char *base_hex, const char *out_path, char **objects, int n_objects) {
    Argv lav;
    av_init(&lav);
    av_add(&lav, MZLD);
    if (base_hex) {
        av_add(&lav, "-b");
        av_add(&lav, base_hex);
    }
    av_add(&lav, "-o");
    av_add(&lav, out_path);
    for (int i = 0; i < n_objects; ++i) {
        av_add(&lav, objects[i]);
    }
    ProcResult lr;
    int ran = run_proc(MZLD, lav.v, lav.n, "", 0, NULL, &lr);
    av_free(&lav);
    if (!ran || lr.exit_code != 0 || !path_exists(out_path)) {
        fprintf(stderr, "mzcc: mzld failed linking the image\n");
        if (lr.stderr_bytes.len) { fwrite(lr.stderr_bytes.data, 1, lr.stderr_bytes.len, stderr); }
        if (lr.stdout_bytes.len) { fwrite(lr.stdout_bytes.data, 1, lr.stdout_bytes.len, stderr); }
        byte_buf_free(&lr.stdout_bytes);
        byte_buf_free(&lr.stderr_bytes);
        return 1;
    }
    byte_buf_free(&lr.stdout_bytes);
    byte_buf_free(&lr.stderr_bytes);
    return 0;
}

/* Resolve tools, compile every source in `spec` through the default C link
   profile (RT asm + libc + body, entry _start, default base), link, and copy the
   result to `out_path`. Never runs the produced image. Returns 0/1/2 per mzcc's
   exit-code convention (2 = toolchain-resolve failure, 1 = compile/link). */
int build_default_c_image(const char *preset, const BuildSpec *spec, const char *out_path) {
    int trc = resolve_toolchain(preset);
    if (trc != 0) { return trc; }
    ensure_scratch();

    /* One parallel + cached object build for the whole set (RT asm + libc +
       user body), assembled in canonical order (maize-274, spec section 4).
       This is the shared path every batch subcommand inherits with no
       subcommand-specific code. */
    StrList all_objs;
    sl_init(&all_objs);
    if (build_objects_parallel(spec->dev, &spec->extra_defines, &spec->sources,
                               spec->used_sources, NULL, NULL, &all_objs) != 0) {
        sl_free(&all_objs);
        return 1;
    }

    /* Prebuilt runtime archive first (maize-302): ensure it exists (built once,
       cached), then pass it as the FIRST link input so mzld expands its members
       in declared order ahead of the user objects, reproducing the from-source
       layout (crt0/_start first, user code last). */
    char *archive = ensure_runtime_archive();
    if (!archive) { sl_free(&all_objs); return 1; }

    /* Link the default profile to the scratch image in canonical object order,
       then copy to out_path (mirroring the single-file path's
       link-to-scratch-then-copy, the maize-278 byte-parity shape). */
    Argv objs;
    av_init(&objs);
    av_add(&objs, archive);
    for (int i = 0; i < all_objs.n; ++i) { av_add(&objs, all_objs.v[i]); }
    sl_free(&all_objs);

    char *mzx = path_join(SCRATCH_ROOT, "prog.mzx");
    int lrc = mzld_link(NULL, mzx, objs.v, objs.n);
    av_free(&objs);
    free(archive);
    if (lrc != 0) { free(mzx); return 1; }

    char *odir = dir_of(out_path);
    if (!path_exists(odir)) { mkdir_p(odir); }
    free(odir);
    if (copy_file(mzx, out_path) != 0) {
        fprintf(stderr, "mzcc: could not write %s\n", out_path);
        free(mzx);
        return 1;
    }
    free(mzx);
    return 0;
}

/* Per-platform default RELEASE preset for the batch subcommands (section 3),
   matching each .sh script's own `case "$UNAME"` block. Note: Darwin maps to
   macos-debug, exactly as the three scripts do today (build-userland.sh:65 etc.),
   NOT macos-release; matching the scripts is what the byte-parity gate requires. */
const char *default_batch_preset(void) {
#if defined(_WIN32)
    return "windows-llvm-mingw-release";
#elif defined(__APPLE__)
    return "macos-debug";
#else
    return "linux-release";
#endif
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
    "usage: mzcc [--preset <name>] [-r|--run] [--emit] [-j N] [-o <path>] <file.c>\n"
    "       mzcc [--preset <name>] [-r|--run] [-j N] -o <path> <a.c> <b.c> [<c.c> ...]\n"
    "       mzcc [--preset <name>] [-r|--run] [-j N] -o <path> --sources <listfile>\n"
    "       mzcc --build";

int main(int argc, char **argv) {
    g_argv0 = argv[0];

    /* SHA-256 in-tree known-answer check (maize-274 cycle-1 review nit 4):
       the object cache's whole correctness rests on this hash actually being
       SHA-256 (a wrong transform would silently serve a wrong .mzo for the
       same key), a property this cycle only verified by hand. Assert the
       FIPS 180-4 "abc" vector at every debug-build startup so a transform
       regression is caught in-tree going forward. Debug builds are exactly
       what run-tests.sh / run-ctest.sh build and exercise locally and in CI
       (CMakePresets.json debug-common), so this runs on every unit-
       verification pass with no separate harness hook; NDEBUG (release
       builds) compiles the check away, matching assert()'s own convention. */
    assert(mzcc_sha256_selfcheck() && "SHA-256 self-check failed: FIPS 180-4 'abc' KAT mismatch");

    /* Subcommand dispatch (maize-280, decision DI 9614): a bare argv[1] verb
       token (no leading '-') is checked FIRST, before the existing option scan.
       A verb has no leading dash so it cannot collide with any flag, and the
       three fixed strings take precedence over the file-existence positional
       resolution below (git/cargo-style), so a stray file named build-userland
       in the CWD is never ambiguous. Everything else (including a bare `foo.c`)
       falls through to the single-file/multi-source compile path unchanged. */
    if (argc > 1 && argv[1][0] != '-') {
        if (strcmp(argv[1], "build-userland") == 0) { return cmd_build_userland(argc - 2, argv + 2); }
        if (strcmp(argv[1], "build-demos") == 0)    { return cmd_build_demos(argc - 2, argv + 2); }
        if (strcmp(argv[1], "build-quesos") == 0)   { return cmd_build_quesos(argc - 2, argv + 2); }
    }

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
        } else if (strcmp(a, "-j") == 0) {
            mzcc_set_jobs_override((i + 1 < argc) ? atoi(argv[++i]) : 0);
        } else if (strncmp(a, "-j", 2) == 0) {
            mzcc_set_jobs_override(atoi(a + 2));
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

    /* REPO_ROOT + RT_DIR (extracted to ensure_repo_root, maize-280): MAIZE_ROOT
       override, else two directory levels up from the mzcc exe, mirroring
       cc-maize.sh's script-relative self-location (decision DI9). */
    ensure_repo_root();

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
        /* One path per line; blank/# lines skipped, CR stripped (read_list_file,
           mzcc_fs.c, maize-280: the same parser the userland/oksh .list files
           use). Existence-check each entry here, as before. */
        StrList entries;
        sl_init(&entries);
        if (read_list_file(lf, &entries) != 0) {
            sl_free(&entries);
            die("cannot read --sources listfile: %s", lf);
        }
        for (int k = 0; k < entries.n; ++k) {
            if (!is_regular_file(entries.v[k])) {
                die("no such file: %s", entries.v[k]);
            }
            sl_push(&src_list, entries.v[k]);
        }
        sl_free(&entries);
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

    /* Tool discovery + preprocessor (extracted to resolve_toolchain, maize-280).
       On any tool-not-found this prints the same actionable message and returns
       2; propagate that as the process exit code. */
    int trc = resolve_toolchain(opt.preset);
    if (trc != 0) { return trc; }

    /* Scratch dirs (extracted to ensure_scratch, maize-280): OBJ_DIR holds the
       .mzo objects and the linked image; CPP_CWD is cpp's empty quote-include
       base (DI6). */
    ensure_scratch();

    /* ---- RT object set + user sources through the ONE parallel + cached
       object build (maize-274). The RT set / tagging / link order stay the
       single point of truth in build_objects_parallel; the single-file path
       differs only in the emit / produce-beside / run tail below. ----------- */
    ByteBuf emit_body;
    int have_emit_body = 0;
    byte_buf_init(&emit_body);
    StrList all_objs;
    sl_init(&all_objs);
    if (build_objects_parallel(opt.dev, NULL, &src_list, opt.used_sources,
                               opt.emit ? &emit_body : NULL, &have_emit_body,
                               &all_objs) != 0) {
        byte_buf_free(&emit_body);
        sl_free(&all_objs);
        return 1;
    }

    /* ---- link (default C profile: RT + libc + body, entry _start, default
       base), object vector in canonical order. The link-profile seam is the
       object set + base; build-quesos's minimal profile slots into the same
       mzld_link (section 8). ------------------------------------------------ */
    char *mzx = path_join(SCRATCH_ROOT, "prog.mzx");
    /* Prebuilt runtime archive first (maize-302): the runtime leaves the
       per-program compile path and is linked from the prebuilt .mza, expanded by
       mzld ahead of the user objects to reproduce the from-source layout. */
    char *archive = ensure_runtime_archive();
    if (!archive) {
        sl_free(&all_objs);
        byte_buf_free(&emit_body);
        free(mzx);
        return 1;
    }
    Argv lav;
    av_init(&lav);
    av_add(&lav, archive);
    for (int i = 0; i < all_objs.n; ++i) { av_add(&lav, all_objs.v[i]); }
    sl_free(&all_objs);
    free(archive);
    if (mzld_link(NULL, mzx, lav.v, lav.n) != 0) {
        av_free(&lav);
        byte_buf_free(&emit_body);
        return 1;
    }
    av_free(&lav);

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
