/* mzcc_quesos.c (maize-280): the `mzcc build-quesos` subcommand, absorbing
   os/quesos/build-quesos.sh. quesOS is guest C (quesos.c) plus the metal it needs
   (quesos_boot.mazm), linked through the SAME cproc -> qbe -> mazm pipeline as the
   default C image but with two departures from the stock C link (decision D8):

     1. A NON-DEFAULT link base (0x00100000), so it does not collide with the
        0x2000 children it execs into the shared flat address space.
     2. A MINIMAL object set: quesos_boot.mzo (its own entry _start + cause-7
        handler), the raw syscall stubs (syscall.mzo), and the quesos.c body. NO
        crt0 (quesOS supplies its own entry), NO libc (quesos.c defines its own
        memcpy/memset and writes through the raw stubs).

   The minimal link is the whole point of the link-profile seam (decision DI 9616):
   a profile is (base address, explicit object list) handed to mzld_link, not a
   registry. This subcommand supplies its own 3-object list and non-default base;
   nothing else about the pipeline changes.

   Usage: mzcc build-quesos [--preset <name>] -o <out.mzx>  (mirrors the .sh
   script's own `-o` for a single image path, deliberately not --out). */
#include "mzcc_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* quesOS's reserved non-default link base (decision D8). */
#define QUESOS_BASE "0x00100000"

int cmd_build_quesos(int argc, char **argv) {
    const char *preset = NULL;
    char *out = NULL;

    /* Hand-rolled argv scan matching build-quesos.sh's flag names verbatim
       (decision DI 9614): -o for the single output image, --preset, nothing
       else. Any other argument is a usage error (exit 2). */
    for (int i = 0; i < argc; ++i) {
        const char *a = argv[i];
        if (strcmp(a, "--preset") == 0) {
            preset = (i + 1 < argc) ? argv[++i] : "";
        } else if (strncmp(a, "--preset=", 9) == 0) {
            preset = a + 9;
        } else if (strcmp(a, "-o") == 0) {
            out = xstrdup((i + 1 < argc) ? argv[++i] : "");
        } else if (strncmp(a, "-o", 2) == 0) {
            out = xstrdup(a + 2);
        } else {
            die("unknown argument: %s", a);
        }
    }
    if (!out || !out[0]) {
        die("an output path is required: -o <out.mzx>");
    }
    if (!preset || !preset[0]) {
        preset = default_batch_preset();
    }

    /* 1. Resolve the toolchain (same CPP-discovery order as the default path, so
       build-quesos.sh's own CPP block is now literally the same code path). */
    int trc = resolve_toolchain(preset);
    if (trc != 0) { return trc; }
    ensure_scratch();

    char *quesos_c    = joinstr(REPO_ROOT, "/os/quesos/quesos.c", NULL, NULL);
    char *boot_mazm   = joinstr(REPO_ROOT, "/os/quesos/quesos_boot.mazm", NULL, NULL);
    char *syscall_mazm = joinstr(RT_DIR, "/syscall.mazm", NULL, NULL);

    /* 2. Compile quesos.c to quesos.body.mzo (the same .body infix convention). */
    char *body_mzo = compile_tu_ex(quesos_c, "quesos", NULL, NULL);
    free(quesos_c);
    if (!body_mzo) {
        fprintf(stderr, "mzcc: FAILED building quesOS (quesos.c)\n");
        free(out); free(boot_mazm); free(syscall_mazm);
        return 1;
    }

    /* 3. Assemble the metal (quesos_boot) and 4. the raw syscall stubs. */
    char *boot_mzo = assemble_mazm_file(boot_mazm, "quesos_boot");
    free(boot_mazm);
    if (!boot_mzo) {
        fprintf(stderr, "mzcc: FAILED building quesOS (quesos_boot.mazm)\n");
        free(out); free(syscall_mazm); free(body_mzo);
        return 1;
    }
    char *syscall_mzo = assemble_mazm_file(syscall_mazm, "syscall");
    free(syscall_mazm);
    if (!syscall_mzo) {
        fprintf(stderr, "mzcc: FAILED building quesOS (syscall.mazm)\n");
        free(out); free(body_mzo); free(boot_mzo);
        return 1;
    }

    /* 5. Link at the non-default base. Entry is _start (mzld default), defined
       by quesos_boot.mzo. Exactly the 3-object minimal set, in the script's link
       order: quesos_boot, syscall, quesos body. Parent dir created if missing. */
    char *odir = dir_of(out);
    if (!path_exists(odir)) { mkdir_p(odir); }
    free(odir);
    char *objects[3] = { boot_mzo, syscall_mzo, body_mzo };
    int lrc = mzld_link(QUESOS_BASE, out, objects, 3);
    free(body_mzo);
    free(boot_mzo);
    free(syscall_mzo);
    if (lrc != 0) {
        free(out);
        return 1;
    }

    /* 6. verify_mzx_image (NEW for quesOS, decision DI 9624: build-quesos.sh
       omits it; a pure safety net that cannot change the linked bytes). */
    if (verify_mzx_image(out) != 0) {
        free(out);
        return 1;
    }
    fprintf(stderr, "mzcc: build-quesos linked %s (base %s)\n", out, QUESOS_BASE);
    free(out);
    return 0;
}
