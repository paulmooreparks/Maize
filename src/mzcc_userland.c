/* mzcc_userland.c (maize-280): the `mzcc build-userland` subcommand, absorbing
   userland/build-userland.sh (the wave-1 sbase set + oksh). It stages a scratch
   checkout of each vendored submodule, applies its patch series in order, overlays
   the Maize-local shim headers, then compiles each program through the SAME
   default C pipeline (build_default_c_image) the single-file path uses.

   Staging is done fully in-process (mzcc_fs.c's copy_tree / copy_file_into_every_
   subdir replace `cp -a` and `find -exec cp`, decision DI 9619); the only external
   spawn this file introduces is `patch` (a compiled binary, not a shell, decision
   DI 9618). The maize-263 stage cache and the WSL-native mirror / SHA-precompute /
   throttling are dropped, not reproduced (decisions DI 9621 / DI 9622): every
   invocation stages fresh, and stage_project stays a pure (tree, patches, shims)
   -> staged-tree function that maize-274's cache can wrap unchanged.

   Exit-code semantics preserve build-userland.sh's ALWAYS-CONTINUE / collapse-to-1
   loop (decision DI 9623): every per-program failure (compile/link/verify, or even
   a toolchain-resolve failure) is collapsed to a per-program failure and the loop
   continues through every remaining program, exiting 1 at the end. This is the
   OPPOSITE of build-demos's fail-fast-on-2 loop; the two are deliberately not
   homogenized. Setup errors (missing submodule, missing sources list, unresolved
   patch tool) still die immediately with exit 2, matching the script's own die().

   Usage: mzcc build-userland [--preset <name>] --out <dir> [prog ...]. */
#include "mzcc_internal.h"
#include "mzcc_fs.h"
#include "mzcc_proc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The wave-1 default set (build-userland.sh:89 SBASE_WAVE1): the 10 sbase utils
   that do not require POSIX regex. ed is deferred (needs regcomp/regexec). oksh is
   appended separately below (cmd_build_userland), matching build-userland.sh:90. */
static const char *WAVE1[] = {
    "true", "false", "echo", "printf", "pwd", "cat", "cp", "mv", "rm", "ls"
};

/* The wave-2 default set (maize-292, build-userland.sh's SBASE_WAVE2): every sbase
   tool an actual build AND a real quesOS smoke run confirmed compiles, links, and
   BEHAVES correctly against the current guest RT, plus the patched `kill`. 15 tools
   the card's spec expected to be build-ready turned out, empirically, to need an
   RT/toolchain surface out of this card's stdin-only scope (decision 9695) and are
   NOT in this list: dd, env, nohup, pathchk, tail, test (Group A) and sort, split,
   strings, tr, uudecode, wc (Group B) fail to build or link; expand and unexpand
   build and link clean but fail their smoke run (a pre-existing cproc/qbe 64-bit
   signed/unsigned ternary mis-promotion in parselist()'s
   MIN(LLONG_MAX, SIZE_MAX), unrelated to any libc gap); uuencode ALSO builds and
   links clean but crashes the whole VM (an uncaught page fault) even as the sole
   check in its own single-tool smoke fixture. See build-userland.sh's SBASE_WAVE2
   comment for the per-tool reason. Single point of truth for both waves, moved
   into the driver (mirrors the wave-1 comment this replaces). */
static const char *WAVE2[] = {
    "basename", "cal", "cksum", "dirname", "logname", "mkdir", "printenv", "sleep",
    "sponge", "tee", "unlink", "yes",
    "cmp", "cols", "comm", "cut", "fold", "head", "join", "md5sum",
    "paste", "rev", "sha1sum", "sha224sum", "sha256sum", "sha384sum", "sha512sum",
    "sha512-224sum", "sha512-256sum", "tsort", "uniq",
    "kill"
};

/* The stage scratch root (its own temp dir, distinct from the compile scratch),
   created lazily and removed at exit. */
static char *g_stage_root = NULL;

static void cleanup_stage_root(void) {
    if (g_stage_root) {
        mzcc_remove_tree(g_stage_root);
    }
}

static const char *stage_root(void) {
    if (!g_stage_root) {
        char troot[4096];
        if (mzcc_make_temp_dir(troot, sizeof(troot)) != 0) {
            die("could not create a staging directory");
        }
        to_slashes(troot);
        g_stage_root = xstrdup(troot);
        atexit(cleanup_stage_root);
    }
    return g_stage_root;
}

/* Resolve the external patch binary once (decision DI 9618): MAIZE_PATCH override,
   then `patch` on PATH, then the Git-for-Windows-bundled patch.exe. Dies (exit 2)
   when none resolves. Returns a cached, owned path string. */
static const char *resolve_patch(void) {
    static char *cached = NULL;
    if (cached) { return cached; }

    const char *env = getenv("MAIZE_PATCH");
    if (env && env[0]) {
        cached = xstrdup(env);
        return cached;
    }
    if (which_exists("patch")) {
        cached = xstrdup("patch");
        return cached;
    }
    const char *cands[] = {
        "C:/Program Files/Git/usr/bin/patch.exe",
        "C:/Program Files (x86)/Git/usr/bin/patch.exe",
        NULL
    };
    for (int i = 0; cands[i]; ++i) {
        if (path_exists(cands[i])) {
            cached = xstrdup(cands[i]);
            return cached;
        }
    }
    die("patch not found; install Git for Windows, or set MAIZE_PATCH to the patch binary");
    return NULL; /* unreachable */
}

/* Apply one unified-diff patch to `stage_dir` (cwd = the staged tree, stdin = the
   patch bytes), reproducing `patch -p1 --forward --silent < <patchfile>`. Dies
   (exit 2) on a spawn or apply failure, matching build-userland.sh:156-158's
   die(). */
static void apply_patch(const char *proj, const char *patch_path, const char *stage_dir) {
    ByteBuf bytes;
    if (read_file(patch_path, &bytes) != 0) {
        byte_buf_free(&bytes);
        die("cannot read patch %s", patch_path);
    }
    const char *patch = resolve_patch();
    Argv av;
    av_init(&av);
    av_add(&av, patch);
    av_add(&av, "-p1");
    av_add(&av, "--forward");
    av_add(&av, "--silent");
    ProcResult r;
    int ran = run_proc(patch, av.v, av.n, bytes.data, bytes.len, stage_dir, &r);
    av_free(&av);
    byte_buf_free(&bytes);
    if (!ran) {
        die("could not spawn patch (%s)", patch);
    }
    int code = r.exit_code;
    ByteBuf err = r.stderr_bytes;
    ByteBuf outb = r.stdout_bytes;
    if (code != 0) {
        if (err.len) { fwrite(err.data, 1, err.len, stderr); }
        if (outb.len) { fwrite(outb.data, 1, outb.len, stderr); }
        byte_buf_free(&outb);
        byte_buf_free(&err);
        die("patch failed for %s: %s", proj, patch_path);
    }
    byte_buf_free(&outb);
    byte_buf_free(&err);
}

/* strcmp comparator over StrList's char* elements (patch-series lexical order). */
static int cmp_str(const void *a, const void *b) {
    const char *sa = *(const char *const *)a;
    const char *sb = *(const char *const *)b;
    return strcmp(sa, sb);
}

/* Stage a pristine submodule into a fresh scratch dir and apply its patch series
   in order, then overlay the generic shim headers into every directory. A pure
   function of (submodule tree, patch bytes, shim bytes) -> staged tree (the seam
   maize-274's cache wraps). Returns the staged tree path (owned by the caller).
   Dies (exit 2) if the submodule is not initialized. */
static char *stage_project(const char *proj) {
    char *sub = joinstr(REPO_ROOT, "/userland/", proj, NULL);
    if (!dir_exists(sub)) {
        die("submodule not initialized: %s (git submodule update --init)", sub);
    }
    char *stage = joinstr(stage_root(), "/", proj, NULL);
    mzcc_remove_tree(stage); /* fresh every invocation (stage cache dropped) */
    if (copy_tree(sub, stage) != 0) {
        die("could not stage %s", sub);
    }
    free(sub);

    /* Patch series: the ".patch" files under userland/patches/<proj>, applied in
       lexical (0001, 0002, ...) order, matching the shell glob's sorted expansion. */
    char *patch_dir = joinstr(REPO_ROOT, "/userland/patches/", proj, NULL);
    if (dir_exists(patch_dir)) {
        StrList names;
        sl_init(&names);
        if (list_dir(patch_dir, &names) == 0) {
            StrList patches;
            sl_init(&patches);
            for (int i = 0; i < names.n; ++i) {
                size_t len = strlen(names.v[i]);
                if (len >= 6 && strcmp(names.v[i] + len - 6, ".patch") == 0) {
                    char *full = joinstr(patch_dir, "/", names.v[i], NULL);
                    sl_push(&patches, full);
                    free(full);
                }
            }
            if (patches.n > 1) {
                qsort(patches.v, (size_t)patches.n, sizeof(char *), cmp_str);
            }
            for (int i = 0; i < patches.n; ++i) {
                apply_patch(proj, patches.v[i], stage);
            }
            sl_free(&patches);
        }
        sl_free(&names);
    }
    free(patch_dir);

    /* Shim-header overlay: copy every ".h" under userland/include into EVERY
       directory of the staged tree, so a per-source `-I <source dir>` resolves the
       angle-bracket shim no matter which subdir a source lives in. */
    char *include_dir = joinstr(REPO_ROOT, "/userland/include", NULL, NULL);
    if (dir_exists(include_dir)) {
        StrList hs;
        sl_init(&hs);
        if (list_dir(include_dir, &hs) == 0) {
            for (int i = 0; i < hs.n; ++i) {
                size_t len = strlen(hs.v[i]);
                if (len >= 2 && strcmp(hs.v[i] + len - 2, ".h") == 0) {
                    char *h = joinstr(include_dir, "/", hs.v[i], NULL);
                    if (copy_file_into_every_subdir(h, stage) != 0) {
                        die("could not overlay shim header %s", h);
                    }
                    free(h);
                }
            }
        }
        sl_free(&hs);
    }
    free(include_dir);

    return stage;
}

/* Build one sbase util from its per-program source list, prefixed with the staged
   sbase tree. Returns 0 on success, nonzero on a build/verify failure (the caller
   collapses any nonzero to the run's exit 1). Dies (exit 2) on a missing/empty
   sources list, matching build_sbase_util's die(). */
static int build_sbase_util(const char *name, const char *sbase_stage,
                            const char *preset, const char *out_dir) {
    char *list = joinstr(REPO_ROOT, "/userland/sources/sbase/", name, ".list");
    if (!is_regular_file(list)) {
        die("no sources list for sbase/%s: %s", name, list);
    }
    StrList rel;
    sl_init(&rel);
    if (read_list_file(list, &rel) != 0) {
        die("cannot read sources list: %s", list);
    }
    if (rel.n == 0) {
        die("empty sources list: %s", list);
    }

    BuildSpec spec;
    sl_init(&spec.sources);
    av_init(&spec.extra_defines);
    spec.used_sources = 0;
    spec.dev = 0;
    for (int i = 0; i < rel.n; ++i) {
        char *src = joinstr(sbase_stage, "/", rel.v[i], NULL);
        sl_push(&spec.sources, src);
        free(src);
    }
    sl_free(&rel);
    free(list);

    char *out = joinstr(out_dir, "/", name, ".mzx");
    int rc = build_default_c_image(preset, &spec, out);
    sl_free(&spec.sources);
    av_free(&spec.extra_defines);
    if (rc != 0) {
        fprintf(stderr, "mzcc: FAILED building sbase/%s\n", name);
        free(out);
        return rc;
    }
    if (verify_mzx_image(out) != 0) { free(out); return 1; }
    fprintf(stderr, "mzcc: built %s\n", out);
    free(out);
    return 0;
}

/* Build the vendored oksh shell. Like build_sbase_util, but oksh carries a
   Maize-local include overlay (patches/oksh/include -> the staged root, landing
   AFTER the generic shim pass so it can override), and compiles with -D EMACS
   (satisfy config.h's EMACS-or-VI guard) and -D volatile= (neutralize the
   volatile keyword cproc-qbe cannot yet lower). Returns 0/nonzero. */
static int build_oksh(const char *oksh_stage, const char *preset, const char *out_dir) {
    /* oksh-specific overlay: patches/oksh/include/. -> the staged oksh root, a
       recursive copy that preserves the sys/ subdir the flat shim pass cannot
       place (build-userland.sh:206-211 ordering: after the generic shim). */
    char *overlay = joinstr(REPO_ROOT, "/userland/patches/oksh/include", NULL, NULL);
    if (dir_exists(overlay)) {
        if (copy_tree(overlay, oksh_stage) != 0) {
            free(overlay);
            fprintf(stderr, "mzcc: FAILED overlaying oksh include headers\n");
            return 1;
        }
    }
    free(overlay);

    char *list = joinstr(REPO_ROOT, "/userland/sources/oksh/oksh.list", NULL, NULL);
    if (!is_regular_file(list)) {
        die("no sources list for oksh: %s", list);
    }
    StrList rel;
    sl_init(&rel);
    if (read_list_file(list, &rel) != 0) {
        die("cannot read sources list: %s", list);
    }
    if (rel.n == 0) {
        die("empty sources list: %s", list);
    }

    BuildSpec spec;
    sl_init(&spec.sources);
    av_init(&spec.extra_defines);
    spec.used_sources = 0;
    spec.dev = 0;
    for (int i = 0; i < rel.n; ++i) {
        char *src = joinstr(oksh_stage, "/", rel.v[i], NULL);
        sl_push(&spec.sources, src);
        free(src);
    }
    sl_free(&rel);
    free(list);

    av_add(&spec.extra_defines, "-D");
    av_add(&spec.extra_defines, "EMACS");
    av_add(&spec.extra_defines, "-D");
    av_add(&spec.extra_defines, "volatile=");

    char *out = joinstr(out_dir, "/oksh.mzx", NULL, NULL);
    int rc = build_default_c_image(preset, &spec, out);
    sl_free(&spec.sources);
    av_free(&spec.extra_defines);
    if (rc != 0) {
        fprintf(stderr, "mzcc: FAILED building oksh\n");
        free(out);
        return rc;
    }
    if (verify_mzx_image(out) != 0) { free(out); return 1; }
    fprintf(stderr, "mzcc: built %s\n", out);
    free(out);
    return 0;
}

int cmd_build_userland(int argc, char **argv) {
    const char *preset = NULL;
    char *out = NULL;
    StrList progs;
    sl_init(&progs);

    /* Hand-rolled argv scan mirroring build-userland.sh's flag names verbatim. */
    for (int i = 0; i < argc; ++i) {
        const char *a = argv[i];
        if (strcmp(a, "--preset") == 0) {
            preset = (i + 1 < argc) ? argv[++i] : "";
        } else if (strncmp(a, "--preset=", 9) == 0) {
            preset = a + 9;
        } else if (strcmp(a, "--out") == 0) {
            free(out);
            out = xstrdup((i + 1 < argc) ? argv[++i] : "");
        } else if (strncmp(a, "--out=", 6) == 0) {
            free(out);
            out = xstrdup(a + 6);
        } else if (strcmp(a, "-j") == 0) {
            mzcc_set_jobs_override((i + 1 < argc) ? atoi(argv[++i]) : 0);
        } else if (strncmp(a, "-j", 2) == 0) {
            mzcc_set_jobs_override(atoi(a + 2));
        } else if (a[0] == '-') {
            die("unknown option: %s", a);
        } else {
            sl_push(&progs, a);
        }
    }
    if (!out || !out[0]) {
        die("an output dir is required: --out <dir>");
    }
    if (!preset || !preset[0]) {
        preset = default_batch_preset();
    }
    mkdir_p(out);
    ensure_repo_root();

    /* Wave-1 + wave-2 + oksh default set when no programs are named (build-
       userland.sh:90's `${SBASE_WAVE1} ${SBASE_WAVE2} oksh`, maize-292 AC 9691:
       both drivers build the same union with no explicit prog names). */
    if (progs.n == 0) {
        for (size_t i = 0; i < sizeof(WAVE1) / sizeof(WAVE1[0]); ++i) {
            sl_push(&progs, WAVE1[i]);
        }
        for (size_t i = 0; i < sizeof(WAVE2) / sizeof(WAVE2[0]); ++i) {
            sl_push(&progs, WAVE2[i]);
        }
        sl_push(&progs, "oksh");
    }

    /* Stage sbase once up front (matching the script); oksh is staged lazily on
       its first appearance. */
    char *sbase_stage = stage_project("sbase");
    char *oksh_stage = NULL;

    /* ALWAYS-CONTINUE / collapse-to-1 loop (decision DI 9623). */
    int rc = 0;
    for (int i = 0; i < progs.n; ++i) {
        int r;
        if (strcmp(progs.v[i], "oksh") == 0) {
            if (!oksh_stage) { oksh_stage = stage_project("oksh"); }
            r = build_oksh(oksh_stage, preset, out);
        } else {
            r = build_sbase_util(progs.v[i], sbase_stage, preset, out);
        }
        if (r != 0) { rc = 1; }
    }

    free(sbase_stage);
    free(oksh_stage);
    sl_free(&progs);
    free(out);
    return rc;
}
