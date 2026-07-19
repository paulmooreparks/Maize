/* demos/doom/doom_render_selfcheck_quesos.c -- maize-251 quesOS-child DOOM render gate.
 *
 * The quesOS twin of doom_render_selfcheck.c (bare-VM). It boots the WHOLE DOOM engine as a
 * quesOS worklist child against the minimal synthetic IWAD, ticks a fixed generous count, and
 * asserts the same pixel-exact 3-point viewport check (doom_viewport_check.h) reading DG_MaizeFB
 * -- which, under quesOS, is the fb-mmap'd buffer DG_DrawFrame copies each rendered frame into
 * and presents via sys_fb_present. Same SAMPLE_TICKS empirical-stability pin as the bare-VM
 * gate (doom_render_selfcheck.c's SAMPLE_TICKS=90 comment); the three sampled points are pure
 * guest computation, so they are byte-identical to the bare-VM render and host-independent.
 *
 * Because a quesOS worklist child receives only argv[0] (its path), DOOM's own args are
 * synthesized here; the runner mounts the synthetic IWAD read-only at /ro. This exercises the
 * full quesOS display path end to end: world detection (sys_fb_geometry), the fb-mmap +
 * fb-register scanout, sys_fb_present, AND the sys_bigalloc fallback that satisfies DOOM's
 * ~6 MiB zone-heap malloc (over the sbrk ceiling).
 *
 * It also carries doom_main.c's exit-code-4 reportable-init-failure guard, so the SAME image
 * proves that path under a display-less session (--fb-no-display -> sys_fb_register -ENODEV ->
 * DG_MaizeInitError == 3 -> stderr diagnostic + exit 4), rather than sampling a dead
 * framebuffer. Prints "doom: PASS" on a real quesOS-mediated render.
 */
#include "doomgeneric/doomgeneric/doomgeneric.h"  /* doomgeneric_Create/Tick, RESX/RESY */
#include "doom_viewport_check.h"                   /* dm_viewport_render_ok (hardened assertion) */
#include "stdint.h"                                /* uint32_t */
#include "stdio.h"                                 /* fprintf, puts */

/* Present buffer + geometry-guard flag defined by the platform TU (doomgeneric_maize.c). */
extern uint32_t *DG_MaizeFB;
extern int       DG_MaizeInitError;

/* Deterministic-clock switch (maize-251), defined by the platform TU. A headless selfcheck
 * ticks a FIXED count and checksums the whole presented frame, so the render must be a pure
 * function of the tick count. Setting this makes DOOM's timing advance on a virtual clock keyed
 * off tick/sleep count instead of real host time, so the melt-wipe and game tics settle in the
 * same animation state (hence the same full-frame checksum) regardless of how slow each tick
 * runs (linux-debug vs linux-asan-ubsan). Interactive doom_main.c leaves this 0 for real-time
 * play. Must be set BEFORE doomgeneric_Create, since D_DoomMain's first frame already wipes. */
extern int       DG_MaizeDeterministicClock;

/* Same empirical-stability pin as doom_render_selfcheck.c (see that file's SAMPLE_TICKS note). */
#define SAMPLE_TICKS   90

int main(void)
{
    int ok;
    int t;

    /* A quesOS worklist child gets only argv[0], so build DOOM's args here (the runner mounts
     * the synthetic IWAD at /ro). Mutable buffers, not string literals, since DOOM's arg layer
     * treats myargv as writable. -warp autostart renders E1M1 and returns. */
    static char a_doom[]  = "doom";
    static char a_iwad[]  = "-iwad";
    static char a_wad[]   = "/ro/min.wad";
    static char a_warp[]  = "-warp";
    static char a_e[]     = "1";
    static char a_m[]     = "1";
    static char a_nomon[] = "-nomonsters";
    static char *av[]     = { a_doom, a_iwad, a_wad, a_warp, a_e, a_m, a_nomon, 0 };

    /* Deterministic render: advance DOOM's clock on tick/sleep count, not real host time, so N
     * ticks always produce the same animation state and the pinned full-frame checksum is
     * reachable AND stable on both linux-debug and linux-asan-ubsan (maize-251). */
    DG_MaizeDeterministicClock = 1;

    /* Runs DG_Init (world probe -> fb-mmap + fb-register under quesOS) + D_DoomMain. */
    doomgeneric_Create(7, av);

    /* Reportable init failure (mirrors doom_main.c): a display-less session leaves
     * DG_MaizeInitError == 3 (sys_fb_register -ENODEV). Report and exit 4 rather than
     * sampling a framebuffer that was never brought up. */
    if (DG_MaizeInitError != 0) {
        fprintf(stderr, "doom: framebuffer init failed (code %d)\n", DG_MaizeInitError);
        return 4;
    }

    /* Tick a fixed, generous count past boot (NOT "first tick that looks rendered"). */
    for (t = 0; t < SAMPLE_TICKS; t++) {
        doomgeneric_Tick();
    }

    /* Geometry is pinned to the compile-time override (320x200); a quesOS child must NOT read
     * the privileged fb geometry ports directly, so pass the fixed dims. */
    ok = dm_viewport_render_ok("doom_quesos", DG_MaizeFB, DG_MaizeInitError,
                               DOOMGENERIC_RESX, DOOMGENERIC_RESY);

    puts(ok ? "doom: PASS" : "doom: FAIL");
    return 0;
}
