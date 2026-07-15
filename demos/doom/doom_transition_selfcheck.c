/* demos/doom/doom_transition_selfcheck.c -- headless DOOM LEVEL-TRANSITION gate
 * (maize-193).
 *
 * Distinct from doom_render_selfcheck.c (maize-154 Phase C), which boots one level
 * via `-warp 1 1` and asserts a single rendered frame. That gate stops at the FIRST
 * level; nothing exercised G_ExitLevel -> intermission (wi_stuff.c) -> G_WorldDone ->
 * P_SetupLevel for the NEXT map. This TU boots MAP01 of a two-map COMMERCIAL
 * synthetic IWAD, drives the level transition explicitly, and asserts MAP02 loads
 * and renders. It reproduces (and now guards against) the "maize exits at level
 * completion" defect: the transition path hit an I_Error's unconditional exit(-1).
 *
 * WHY COMMERCIAL (MAP01/MAP02, not E1M1/E1M2): D_IdentifyVersion sets
 * gamemode = commercial when a MAP01 lump is present (d_main.c:736). The reported
 * bug reproduces against DOOM2.WAD (commercial), and wi_stuff.c's intermission lump
 * naming diverges by gamemode (CWILV%2.2d commercial vs WILV%d%d episode). An
 * episode-format harness would exercise DIFFERENT W_GetNumForName calls than the
 * real defect, so the synthetic IWAD is generated in commercial mode.
 *
 * DRIVING THE TRANSITION HEADLESSLY: with no player input reaching the exit linedef,
 * we call G_ExitLevel() directly after the MAP01 render (as the card's suggested
 * approach states). One tick later G_Ticker runs G_DoCompleted -> WI_Start ->
 * WI_loadData, which W_CacheLumpName's the whole commercial intermission lump set
 * (CWILV00..31, WI* widgets, INTERPIC): a missing lump there is the classic
 * transition I_Error. The single-player intermission then parks at sp_state 10
 * waiting for a use/fire press that never arrives headlessly (wi_stuff.c:1430), so
 * we advance it by calling the exported G_WorldDone() directly; one tick later
 * G_DoWorldDone sets gamemap = wminfo.next+1 (== 2) and G_DoLoadLevel loads MAP02.
 * We then tick until MAP02's viewport renders.
 *
 * ASSERTION: after a bounded tick budget, BOTH (a) gamemap == 2 and (b) a real 3D
 * render of MAP02's viewport. MAP02 reuses MAP01's single-room geometry and player-1
 * spawn (make_min_iwad.c add_map_lumps shares the THINGS/geometry payloads), so its
 * viewport samples must match the render gate's exactly: maize-156 upgrades this
 * post-transition check from the weak distinct-color heuristic to the shared HARDENED
 * dm_viewport_render_ok (exact gamma-mapped palette XRGB at three fixed points). The
 * MAP01 PRE-transition check stays on the cheap dm_viewport_rendered distinct-color
 * heuristic (decision below): it is only a precondition confirming MAP01 booted before
 * we drive the transition, not the hardened assertion this gate is about, so it does
 * not need pixel-exactness. A transition that never completes FAILs on the tick cap,
 * it does not hang.
 *
 * Prints exactly "doom-transition: PASS" on success (else "doom-transition: FAIL"),
 * so run-ctest.sh gates on the line.
 */
#include "doomgeneric/doomgeneric/doomgeneric.h"  /* doomgeneric_Create/Tick */
#include "doom_viewport_check.h"                   /* dm_viewport_rendered */
#include "mzdev.h"                                 /* fb_width / fb_height */
#include "stdint.h"                                /* uint32_t */
#include "stdio.h"                                 /* printf / puts */

/* Present buffer + geometry-guard flag defined by the Phase B platform TU. */
extern uint32_t *DG_MaizeFB;
extern int       DG_MaizeInitError;

/* Engine globals / entry points reached the same way other harness TUs reach DOOM
 * state: plain externs against g_game.c / doomstat.c (avoids pulling the whole DOOM
 * header soup into this TU). gamemap is `int` (g_game.c:101). */
extern int  gamemap;
extern void G_ExitLevel(void);   /* g_game.h:60 -- sets gameaction = ga_completed */
extern void G_WorldDone(void);   /* g_game.h:63 -- sets gameaction = ga_worlddone */

/* Ticks allowed for the initial MAP01 render (mirrors doom_render_selfcheck.c). */
#define BOOT_TICKS       60
/* Ticks allowed to enter GS_INTERMISSION and run WI_Start's lump loading after
 * G_ExitLevel. TryRunTics runs >= 1 tic per call, so a handful is ample; this only
 * bounds a never-completes failure. */
#define INTERMISSION_TICKS 30
/* Ticks allowed for MAP02 to load (gamemap == 2) after G_WorldDone. */
#define NEXTMAP_TICKS    60
/* Fixed settle ticks after MAP02 has loaded, before the hardened sample (mirrors the
 * render gate's fixed-tick-count-past-boot rule; the sampled points are stable from
 * the first rendered tick, so this is pure margin over any load-frame wipe). */
#define MAP02_SETTLE_TICKS 30

/* MAP01 precondition: cheap distinct-color heuristic (not the hardened check). */
static int viewport_rendered(const char *tag)
{
    return dm_viewport_rendered(tag, DG_MaizeFB, DG_MaizeInitError,
                                fb_width(), fb_height());
}

/* MAP02 post-transition: the hardened sampled-pixel assertion (maize-156). */
static int viewport_render_ok(const char *tag)
{
    return dm_viewport_render_ok(tag, DG_MaizeFB, DG_MaizeInitError,
                                 fb_width(), fb_height());
}

int main(int argc, char **argv)
{
    int ok;
    int t;

    /* Runs DG_Init + D_DoomMain; -warp autostart loads MAP01, renders, returns. */
    doomgeneric_Create(argc, argv);

    /* (1) Confirm MAP01 actually rendered before we drive the transition. */
    ok = viewport_rendered("doom-transition-map01");
    for (t = 0; t < BOOT_TICKS && !ok; t++) {
        doomgeneric_Tick();
        ok = viewport_rendered("doom-transition-map01");
    }
    if (!ok) {
        puts("doom-transition: FAIL (MAP01 never rendered)");
        return 0;
    }

    /* (2) Complete the level. Next G_Ticker runs G_DoCompleted -> WI_Start ->
     * WI_loadData (the commercial intermission lump load). */
    G_ExitLevel();
    for (t = 0; t < INTERMISSION_TICKS; t++) {
        doomgeneric_Tick();
    }

    /* (3) Advance the parked single-player intermission to the next map. */
    G_WorldDone();

    /* (4) Tick until MAP02 has loaded (gamemap == 2), then settle a fixed count and
     * apply the hardened sampled-pixel assertion once (not "first tick it renders"). */
    ok = 0;
    for (t = 0; t < NEXTMAP_TICKS; t++) {
        doomgeneric_Tick();
        if (gamemap == 2) {
            ok = 1;
            break;
        }
    }
    if (ok) {
        for (t = 0; t < MAP02_SETTLE_TICKS; t++) {
            doomgeneric_Tick();
        }
        ok = viewport_render_ok("doom-transition-map02");
    }

    if (ok) {
        printf("doom-transition: gamemap=%d\n", gamemap);
        puts("doom-transition: PASS");
    } else {
        printf("doom-transition: gamemap=%d (want 2)\n", gamemap);
        puts("doom-transition: FAIL");
    }
    return 0;
}
