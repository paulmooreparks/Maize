/* demos/doom/doom_input_selfcheck.c -- headless DOOM ENGINE-LEVEL INPUT gate (maize-156).
 *
 * Distinct from every existing DOOM gate:
 *   - doom_selfcheck.c (maize-153 Phase B) exercises DG_GetKey in ISOLATION (no engine
 *     boot); it proves the platform layer translates Set-1 scancodes to the right
 *     doomkeys.h constants, but nothing downstream consumes them.
 *   - doom_render_selfcheck.c / doom_transition_selfcheck.c boot the full engine but
 *     inject ZERO keyboard input; they exercise the render path only.
 * The maize-155 defect (a physical key mapped to a keycode with no matching in-game
 * binding) was invisible to both: doom_selfcheck reported the correct raw translation,
 * and the render gates never injected a key. This TU closes that gap: it boots the
 * WHOLE engine against the synthetic min.wad AND injects a real Set-1 scancode through
 * the SAME doomgeneric_maize.c DG_GetKey path production input uses, then asserts the
 * injected key drove an in-SIM state change (position + ammo), not just a render.
 *
 * INJECTION (load-bearing): the scancode bytes arrive on the guest's keyboard device
 * via `maize --input=keyboard` (stdin -> devices.cpp keyboard device -> kbd_status/
 * kbd_read -> doomgeneric_maize.c DG_GetKey -> scancode_to_doom -> D_PostEvent), the
 * exact path production input travels and the exact layer maize-155's bug lived in. We
 * do NOT call D_PostEvent / write gamekeydown[] directly; that would bypass the
 * scancode_to_doom table and defeat the gate. run-ctest.sh pipes two MAKE-only bytes
 * (no break): 0x48 (KEY_UPARROW / key_up) and 0x1D (Ctrl / KEY_FIRE, the exact maize-155
 * physical key). A make with no following break holds the binding down for every
 * subsequent tic (DG_GetKey drains once, gamekeydown[] stays set), so both controls are
 * held for the whole run; we check movement first, then the ammo decrement.
 *
 * ASSERTIONS (both, bounded so a never-registers input FAILs rather than hangs):
 *   1. MOVEMENT: holding up moves the player forward. The spawn is map origin at angle 0
 *      (make_min_iwad.c build_things: angle=0), and DOOM's BAM convention puts angle 0 on
 *      +x (east, cos(0)=1). Confirmed empirically during Implement: mo->x increases
 *      strongly (dx > 0) from the first tic, mo->y is incidental jitter. We require
 *      mo->x to rise by more than one map unit (FRACUNIT) above spawn.
 *   2. FIRE: holding Ctrl fires the pistol; A_FirePistol -> DecreaseAmmo strictly
 *      decrements ammo[am_clip]. (The synthetic WAD gained the pistol fire-frame + bullet
 *      puff sprites in make_min_iwad.c so the firing animation does not I_Error on a
 *      missing sprite lump.) We require ammo[am_clip] to strictly drop below spawn.
 *
 * Reaching player state needs struct-field access (position/ammo live inside player_t /
 * mobj_t, not bare ints), so unlike the transition gate's bare-extern minimalism this TU
 * includes doomstat.h for `player_t players[MAXPLAYERS]` (which transitively supplies
 * mobj_t and am_clip). See the decision recorded on the card.
 *
 * Prints exactly "doom-input: PASS" on success (else "doom-input: FAIL"), so
 * run-ctest.sh gates on the line.
 */
#include "doomgeneric/doomgeneric/doomgeneric.h"  /* doomgeneric_Create/Tick */
#include "doomgeneric/doomgeneric/doomstat.h"     /* player_t players[]; am_clip; mobj_t */
#include "stdint.h"                                /* uint32_t */
#include "stdio.h"                                 /* printf / puts */

/* Geometry-guard flag from the Phase B platform TU: nonzero means the framebuffer
 * geometry did not match, so the engine never came up cleanly. */
extern int DG_MaizeInitError;

/* Bounded tick budget for BOTH sub-checks. Empirically movement registers at tic 1 and
 * the first pistol shot at tic 4, so 90 is generous headroom; it also bounds a
 * never-registers regression to a FAIL rather than a hang. */
#define MAX_INPUT_TICKS 90

/* One map unit in 16.16 fixed point: the minimum forward delta we count as real motion
 * (not spawn sub-unit placement). Empirically the first tic already moves ~13 units. */
#define DM_MOVE_MIN_FIXED 65536

int main(int argc, char **argv)
{
    int t;
    int x0, ammo0;
    int moved = 0, fired = 0;
    int move_tick = -1, fire_tick = -1;
    int dx_final = 0, ammo_final = 0;

    /* Runs DG_Init + D_DoomMain; -warp autostart loads the room, spawns the player. */
    doomgeneric_Create(argc, argv);

    if (DG_MaizeInitError != 0) {
        printf("doom-input: init error %d\n", DG_MaizeInitError);
        puts("doom-input: FAIL");
        return 0;
    }
    if (players[0].mo == 0) {
        puts("doom-input: FAIL (no player mobj at spawn)");
        return 0;
    }

    x0 = players[0].mo->x;                 /* spawn forward coordinate (fixed_t) */
    ammo0 = players[0].ammo[am_clip];      /* spawn clip ammo */
    ammo_final = ammo0;

    for (t = 1; t <= MAX_INPUT_TICKS; t++) {
        doomgeneric_Tick();

        if (players[0].mo != 0) {
            int dx = players[0].mo->x - x0;
            dx_final = dx;
            if (!moved && dx > DM_MOVE_MIN_FIXED) {
                moved = 1;
                move_tick = t;
            }
        }

        ammo_final = players[0].ammo[am_clip];
        if (!fired && ammo_final < ammo0) {
            fired = 1;
            fire_tick = t;
        }

        if (moved && fired) {
            break;
        }
    }

    printf("doom-input: move dx=%d tick=%d; fire ammo %d->%d tick=%d\n",
           dx_final, move_tick, ammo0, ammo_final, fire_tick);

    if (moved && fired) {
        puts("doom-input: PASS");
    } else {
        printf("doom-input: FAIL (moved=%d fired=%d)\n", moved, fired);
        puts("doom-input: FAIL");
    }
    return 0;
}
