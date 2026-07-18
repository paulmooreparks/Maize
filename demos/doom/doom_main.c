/*
 * demos/doom/doom_main.c: maize-145 DOOM Phase A entry translation unit.
 *
 * This is the interactive entry point Phase C keeps: main() hands argc/argv to
 * doomgeneric_Create() (engine + WAD + zone init) and then drives the frame loop
 * with doomgeneric_Tick(). Phase A does NOT run this; the loop only needs to
 * EXIST so the interactive entry is real and the whole object set is linked.
 *
 * mzld links every object handed to it (no dead-strip), so an undefined symbol in
 * ANY listed TU fails the link regardless of main's call graph, which is exactly
 * the Phase A gap sweep. doom.sources is entry-free so Phase C can reuse it
 * verbatim for the headless self-check; this entry TU is passed on the command
 * line alongside it.
 *
 * doomgeneric.h is reached by its repo-relative subpath from this TU's own
 * directory (demos/doom/), which cc-maize.sh puts on the include path.
 */

#include "doomgeneric/doomgeneric/doomgeneric.h"

#include <stdio.h>   /* maize-251: fprintf(stderr, ...) for the reportable init-error path */

/* maize-251: the platform layer's DG_Init sets DG_MaizeInitError != 0 when the framebuffer
 * cannot be brought up (geometry mismatch=1, null screenbuffer=2, register failure=3 which
 * covers -ENODEV under a display-less --fb-no-display session, mmap failure=4). Report it and
 * exit with a distinct code (4, vs maize-221's own exit code 3) rather than spinning a frame
 * loop against a dead framebuffer -- so `doom` at the oksh prompt on a display-less session
 * fails loud and cleanly, and the reap transcript / stderr diagnostic is observable. */
extern int DG_MaizeInitError;

int main(int argc, char **argv)
{
    doomgeneric_Create(argc, argv);

    if (DG_MaizeInitError != 0) {
        fprintf(stderr, "doom: framebuffer init failed (code %d)\n", DG_MaizeInitError);
        return 4;
    }

    for (;;)
    {
        doomgeneric_Tick();
    }

    return 0;
}
