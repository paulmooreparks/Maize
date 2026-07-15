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
 * and returns. We then tick a fixed, generous count and sample the frame.
 *
 * THE ASSERTION (maize-156 hardening): sample three fixed viewport points and
 * require each to equal the exact gamma-mapped palette XRGB of the room surface it
 * lands on (dm_viewport_render_ok in doom_viewport_check.h, shared with the
 * transition gate). This REPLACES the earlier weak ">= 2 distinct colors" heuristic
 * (previously inlined in this TU as viewport_rendered(); the maize-193 review nit was
 * that it duplicated doom_viewport_check.h's copy) which could not tell a correct
 * render from a garbage/partial/shifted frame that clears to two colors. See the
 * header for how each expected value traces to a make_min_iwad.c palette constant.
 *
 * TICK TIMING (maize-156): the OLD loop sampled at the FIRST tick the weak heuristic
 * returned true, which is not guaranteed to be a fully-converged frame and is more
 * fragile under a pixel-exact assertion. We now tick a FIXED, generous count past
 * boot before sampling once. Empirically (verified during Implement) the three
 * sampled points are byte-identical at EVERY tick from 1 to 100: the intro screen
 * D_Display melt-wipe does not perturb these coordinates at all, so SAMPLE_TICKS=90
 * (comfortably past the historical MAX_TICKS=60 ceiling) clears any wipe with full
 * margin. Two-tick-count stability was confirmed by comparing the full per-color
 * viewport footprints at tick 60 vs tick 100 (byte-identical) before pinning.
 *
 * Prints exactly "doom: PASS" on a real render (else "doom: FAIL"), mirroring
 * doom_selfcheck.c so run-ctest.sh gates on the line.
 */
#include "doomgeneric/doomgeneric/doomgeneric.h"  /* doomgeneric_Create/Tick, pixel_t, RESX/RESY */
#include "doom_viewport_check.h"                   /* dm_viewport_render_ok (hardened assertion) */
#include "mzdev.h"                                 /* fb_width / fb_height */
#include "stdint.h"                                /* uint32_t */
#include "stdio.h"                                 /* printf / puts */

/* Present buffer + geometry-guard flag defined by the Phase B platform TU. */
extern uint32_t *DG_MaizeFB;
extern int       DG_MaizeInitError;

/* Fixed sample tick: tick this many times past boot, then sample once. Generous
 * margin over the (empirically zero-effect at our sample points) intro wipe; see
 * the file header for the stability evidence. */
#define SAMPLE_TICKS   90

int main(int argc, char **argv)
{
    int ok;
    int t;

    /* Runs DG_Init + D_DoomMain; -warp autostart renders one frame and returns. */
    doomgeneric_Create(argc, argv);

    /* Tick a fixed, generous count past boot (NOT "first tick that looks rendered"). */
    for (t = 0; t < SAMPLE_TICKS; t++) {
        doomgeneric_Tick();
    }

    ok = dm_viewport_render_ok("doom_render", DG_MaizeFB, DG_MaizeInitError,
                               fb_width(), fb_height());

    puts(ok ? "doom: PASS" : "doom: FAIL");
    return 0;
}
