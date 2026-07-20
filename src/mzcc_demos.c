/* mzcc_demos.c (maize-280): the `mzcc build-demos` subcommand, absorbing
   demos/build-demos.sh (kilo + doom). Both demos already fit the general
   multi-source / --dev / -D shape build_default_c_image implements, so this
   subcommand builds nothing bespoke: it assembles a BuildSpec per demo and calls
   build_default_c_image.

   Exit-code semantics preserve build-demos.sh's FAIL-FAST-on-2 loop (decision
   DI 9623), which is the OPPOSITE of build-userland's always-continue loop. A
   setup/usage-class failure (exit 2, e.g. a bad --preset that resolves no tools)
   hits every demo, so it fails fast and propagates 2 without attempting later
   demos; a genuine per-demo compile/link failure (1) is accumulated and the loop
   continues. This subcommand and build-userland look structurally similar; the
   two loop semantics are deliberately NOT homogenized.

   Usage: mzcc build-demos [--preset <name>] --out <dir> [demo ...]. */
#include "mzcc_internal.h"
#include "mzcc_fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Build kilo: a single-source build straight through the default C profile.
   Returns build_default_c_image's own code (0/1/2), or 1 on a verify failure. */
static int build_kilo(const char *preset, const char *out_dir) {
    char *kilo_c = joinstr(REPO_ROOT, "/demos/kilo/kilo.c", NULL, NULL);
    char *out = joinstr(out_dir, "/kilo.mzx", NULL, NULL);

    BuildSpec spec;
    sl_init(&spec.sources);
    av_init(&spec.extra_defines);
    spec.used_sources = 0;
    spec.dev = 0;
    sl_push(&spec.sources, kilo_c);
    free(kilo_c);

    int rc = build_default_c_image(preset, &spec, out);
    sl_free(&spec.sources);
    av_free(&spec.extra_defines);
    if (rc != 0) {
        fprintf(stderr, "mzcc: FAILED building kilo (exit %d)\n", rc);
        free(out);
        return rc;
    }
    if (verify_mzx_image(out) != 0) { free(out); return 1; }
    fprintf(stderr, "mzcc: built %s\n", out);
    free(out);
    return 0;
}

/* Build doom: the whole doomgeneric + DOOM tree. Checks the engine submodule is
   initialized first (die with the exact init command, exit 2, no auto-init).
   Sources are the two positionals FIRST then every doom.sources entry (matching
   cc-maize's positionals-then---sources collection order, which is the mzld link
   order); --dev links mzdev; the two RESX/RESY defines apply to every compile. */
static int build_doom(const char *preset, const char *out_dir) {
    char *engine = joinstr(REPO_ROOT, "/demos/doom/doomgeneric/doomgeneric", NULL, NULL);
    if (!dir_exists(engine)) {
        free(engine);
        die("submodule not initialized: demos/doom/doomgeneric (git submodule update --init demos/doom/doomgeneric)");
    }
    free(engine);

    char *out = joinstr(out_dir, "/doom.mzx", NULL, NULL);
    char *doom_main = joinstr(REPO_ROOT, "/demos/doom/doom_main.c", NULL, NULL);
    char *doom_gen = joinstr(REPO_ROOT, "/demos/doom/doomgeneric_maize.c", NULL, NULL);
    char *doom_sources = joinstr(REPO_ROOT, "/demos/doom/doom.sources", NULL, NULL);

    BuildSpec spec;
    sl_init(&spec.sources);
    av_init(&spec.extra_defines);
    spec.used_sources = 1; /* --sources doom.sources: force multi-source tagging */
    spec.dev = 1;

    sl_push(&spec.sources, doom_main);
    sl_push(&spec.sources, doom_gen);
    free(doom_main);
    free(doom_gen);
    /* doom.sources entries are repo-relative (resolved against CWD=REPO_ROOT),
       passed exactly as cc-maize receives them so paths and sibling-include
       resolution stay byte-identical. */
    if (read_list_file(doom_sources, &spec.sources) != 0) {
        fprintf(stderr, "mzcc: cannot read doom source list %s\n", doom_sources);
        free(doom_sources); free(out);
        sl_free(&spec.sources); av_free(&spec.extra_defines);
        return 1;
    }
    free(doom_sources);

    av_add(&spec.extra_defines, "-D");
    av_add(&spec.extra_defines, "DOOMGENERIC_RESX=320");
    av_add(&spec.extra_defines, "-D");
    av_add(&spec.extra_defines, "DOOMGENERIC_RESY=200");

    int rc = build_default_c_image(preset, &spec, out);
    sl_free(&spec.sources);
    av_free(&spec.extra_defines);
    if (rc != 0) {
        fprintf(stderr, "mzcc: FAILED building doom (exit %d)\n", rc);
        free(out);
        return rc;
    }
    if (verify_mzx_image(out) != 0) { free(out); return 1; }
    fprintf(stderr, "mzcc: built %s\n", out);
    free(out);
    return 0;
}

int cmd_build_demos(int argc, char **argv) {
    const char *preset = NULL;
    char *out = NULL;
    StrList demos;
    sl_init(&demos);

    /* Hand-rolled argv scan mirroring build-demos.sh's flag names verbatim. */
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
        } else if (a[0] == '-') {
            die("unknown option: %s", a);
        } else {
            sl_push(&demos, a);
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

    /* v1 demo set (kilo doom) when none named. */
    if (demos.n == 0) {
        sl_push(&demos, "kilo");
        sl_push(&demos, "doom");
    }

    /* Validate every requested demo up front (decision 9488), so an unknown name
       fails fast (exit 2) before any build work happens. */
    for (int i = 0; i < demos.n; ++i) {
        if (strcmp(demos.v[i], "kilo") != 0 && strcmp(demos.v[i], "doom") != 0) {
            die("unknown demo: %s (known: kilo, doom)", demos.v[i]);
        }
    }

    /* FAIL-FAST-on-2 loop (decision DI 9623): a setup/usage failure (exit 2)
       aborts immediately; a per-demo build failure (1) accumulates in rc. */
    int rc = 0;
    for (int i = 0; i < demos.n; ++i) {
        int r;
        if (strcmp(demos.v[i], "kilo") == 0) {
            r = build_kilo(preset, out);
        } else {
            r = build_doom(preset, out);
        }
        if (r == 2) {
            sl_free(&demos);
            free(out);
            return 2;
        }
        if (r != 0) { rc = 1; }
    }
    sl_free(&demos);
    free(out);
    return rc;
}
