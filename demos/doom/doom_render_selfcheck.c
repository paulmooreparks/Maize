/* demos/doom/doom_render_selfcheck.c -- headless DOOM RENDER gate (maize-154 Phase C).
 *
 * Distinct from Phase B's doom_selfcheck.c (which exercised the DG_* platform
 * primitives in isolation, no engine). This TU boots the WHOLE DOOM engine
 * against the minimal synthetic IWAD (demos/doom/tools/make_min_iwad.c) and
 * asserts a REAL 3D level actually rendered.
 *
 * Link shape: this entry TU + the entry-free doom.sources core set + the Phase B
 * platform doomgeneric_maize.c + the mzdev device shim (--dev) + the RT libc,
 * all at the 320x200 geometry override. It REPLACES doom_main.c as the entry
 * (doom_main.c loops forever; this one caps the tick loop and self-checks the
 * framebuffer). doomgeneric.c (from doom.sources) owns DG_ScreenBuffer and
 * doomgeneric_Create/Tick; this TU only owns main().
 *
 * Boot path: doomgeneric_Create runs DG_Init + D_DoomMain. DOOM args reach the
 * guest via maize's guest-argv passthrough:
 *   maize --mount <minwad-dir>=/ro:ro doom_render.mzx \
 *         -iwad /ro/min.wad -warp 1 1 -nomonsters
 * With -warp autostart D_DoomMain loads E1M1, inits graphics, renders one frame,
 * and returns. We then tick until the viewport is a real render (capped).
 *
 * THE ASSERTION (OQ3, load-bearing): sample ONLY the 3D VIEWPORT region
 * (rows y < ST_Y, ABOVE the status bar), NOT the whole framebuffer. The status
 * bar (st_stuff.c) blits widgets every tick regardless of the 3D view, so a
 * booted-but-blank 3D render (R_RenderPlayerView runs but draws nothing, e.g. a
 * codegen gap in the seg/plane path) over a black-cleared column buffer would
 * leave black + status-bar-color = 2 distinct colors and spuriously PASS a
 * whole-framebuffer check. Sampling only the viewport defeats that: a blank 3D
 * render is a single color there and fails. We require >= 2 distinct colors AND
 * a substantial second-color area (not a stray pixel), which on the minimal room
 * is the ceiling / wall / floor bands (Decision D3/D4 gives them distinct RGB).
 *
 * Prints exactly "doom: PASS" on a real render (else "doom: FAIL"), mirroring
 * doom_selfcheck.c so run-ctest.sh gates on the line.
 */
#include "doomgeneric/doomgeneric/doomgeneric.h"  /* doomgeneric_Create/Tick, pixel_t, RESX/RESY */
#include "mzdev.h"                                /* fb_width / fb_height */
#include "stdint.h"                               /* uint32_t */
#include "stdio.h"                                /* printf / puts */

/* Present buffer + geometry-guard flag defined by the Phase B platform TU. */
extern uint32_t *DG_MaizeFB;
extern int       DG_MaizeInitError;

/* Status bar height is 32 rows (st_stuff.h: ST_HEIGHT=32, ST_Y=SCREENHEIGHT-32).
 * The 3D viewport at the default screen size (screenblocks=10, fullscreen with
 * status bar) is rows [0, ST_Y). */
#define ST_HEIGHT      32
#define VIEWPORT_MAX_Y (200 - ST_HEIGHT)   /* == ST_Y == 168 (RESY is 200) */

/* Tick budget: after doomgeneric_Create's first render we tick until the level
 * is fully rendered (past the D_Display screen-melt wipe). Standing still with
 * -nomonsters the scene is static, so this converges in a handful of ticks; the
 * cap only bounds a never-renders failure. */
#define MAX_TICKS      60

/* Second-largest color area required (in pixels) to count as a real band rather
 * than a stray pixel. The viewport is 320*168 = 53760 px; the room's smallest
 * band is far larger than this floor. */
#define MIN_SECOND_AREA 400

#define NBUCKETS 32

/* Analyse the 3D viewport region of the presented framebuffer. Returns 1 when it
 * looks like a real render: >= 2 distinct XRGB colors AND the second-most-common
 * color covers >= MIN_SECOND_AREA pixels. */
static int viewport_rendered(void)
{
    unsigned w, h, maxy, x, y;
    uint32_t colors[NBUCKETS];
    unsigned counts[NBUCKETS];
    unsigned ndistinct = 0;
    unsigned overflow = 0;
    unsigned top1 = 0, top2 = 0;
    unsigned i;

    if (DG_MaizeInitError != 0 || DG_MaizeFB == 0) {
        return 0;
    }

    w = fb_width();
    h = fb_height();
    maxy = VIEWPORT_MAX_Y;
    if (maxy > h) {
        maxy = h;
    }

    for (i = 0; i < NBUCKETS; i++) {
        colors[i] = 0;
        counts[i] = 0;
    }

    for (y = 0; y < maxy; y++) {
        for (x = 0; x < w; x++) {
            uint32_t px = DG_MaizeFB[y * w + x] & 0x00FFFFFFu;
            unsigned found = 0;
            for (i = 0; i < ndistinct; i++) {
                if (colors[i] == px) {
                    counts[i]++;
                    found = 1;
                    break;
                }
            }
            if (!found) {
                if (ndistinct < NBUCKETS) {
                    colors[ndistinct] = px;
                    counts[ndistinct] = 1;
                    ndistinct++;
                } else {
                    overflow = 1;   /* more than NBUCKETS colors: plenty distinct */
                }
            }
        }
    }

    for (i = 0; i < ndistinct; i++) {
        if (counts[i] >= top1) {
            top2 = top1;
            top1 = counts[i];
        } else if (counts[i] > top2) {
            top2 = counts[i];
        }
    }

    printf("doom_render: viewport %ux%u distinct=%u%s top1=%u top2=%u\n",
           w, maxy, ndistinct, overflow ? "+" : "", top1, top2);

    if (ndistinct < 2) {
        return 0;
    }
    if (top2 < MIN_SECOND_AREA) {
        return 0;
    }
    return 1;
}

int main(int argc, char **argv)
{
    int ok;
    int t;

    /* Runs DG_Init + D_DoomMain; -warp autostart renders one frame and returns. */
    doomgeneric_Create(argc, argv);

    ok = viewport_rendered();
    for (t = 0; t < MAX_TICKS && !ok; t++) {
        doomgeneric_Tick();
        ok = viewport_rendered();
    }

    puts(ok ? "doom: PASS" : "doom: FAIL");
    return 0;
}
