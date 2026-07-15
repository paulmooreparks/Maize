/* demos/doom/doom_viewport_check.h -- shared 3D-viewport render checks.
 *
 * Factored out of doom_render_selfcheck.c (maize-154) so the maize-193 transition
 * gate reuses the exact same sampling logic rather than copying it, and so the
 * hardened maize-156 assertion lives in ONE place both harness TUs call. Header-only
 * (static) so each TU that includes it gets its own copy with no extra link unit.
 *
 * TWO checks live here:
 *
 *   dm_viewport_rendered() -- the ORIGINAL weak heuristic: bucket-count distinct
 *   XRGB colors in the 3D viewport region (rows y < ST_Y) and require >= 2 distinct
 *   colors AND a substantial second-color area. Kept as a cheap PRECONDITION gate
 *   (transition MAP01, before we drive the transition) and as a triage diagnostic
 *   printed on a hardened-check failure. It is NO LONGER the PASS/FAIL decision for
 *   the render gate or the transition MAP02 gate: it cannot tell a correct render
 *   from a garbage/partial/shifted frame that happens to clear to two colors, the
 *   exact false-pass that sent a maize-155 play-test investigation down a wrong path.
 *
 *   dm_viewport_render_ok() -- the maize-156 HARDENED assertion: sample a small,
 *   fixed set of (x, y) points and require each masked pixel to equal the EXACT XRGB
 *   value its room surface must present. Every expected value derives from
 *   make_min_iwad.c's build_playpal palette RGB for the three surfaces (wall CW=200 ->
 *   (255,0,0); floor CF=100 -> (0,208,0); ceiling CC=50 -> (32,64,255)), mapped through
 *   the engine's OWN gammatable[usegamma] exactly as i_video.c's I_SetPalette does
 *   before a palette index reaches the framebuffer (usegamma=0's table is a +1 shift
 *   on low components, NOT identity, so e.g. wall red reaches the fb as 0x00FF0101,
 *   not 0x00FF0000). No separately-captured magic number is pinned: change a generator
 *   palette constant or the engine gamma table and the expected value tracks it.
 *   Three distinct exact values strictly subsume the old ">= 2 colors" heuristic and
 *   additionally catch a shifted/rotated camera, a wrong-colored render, or a
 *   correct-distinct-colors-but-wrong-position frame the old check false-passed.
 */
#ifndef DOOM_VIEWPORT_CHECK_H
#define DOOM_VIEWPORT_CHECK_H

#include "stdint.h"   /* uint32_t */
#include "stdio.h"    /* printf */

/* Status bar height is 32 rows (st_stuff.h: ST_HEIGHT=32, ST_Y=SCREENHEIGHT-32).
 * The 3D viewport at the default screen size (screenblocks=10, fullscreen with
 * status bar) is rows [0, ST_Y). RESY is 200 at the geometry override. */
#define DM_ST_HEIGHT      32
#define DM_VIEWPORT_MAX_Y (200 - DM_ST_HEIGHT)   /* == ST_Y == 168 */

/* Second-largest color area required (in pixels) to count as a real band rather
 * than a stray pixel. The viewport is 320*168 = 53760 px; the room's smallest
 * band is far larger than this floor. */
#define DM_MIN_SECOND_AREA 400

#define DM_NBUCKETS 32

/* Engine palette-to-RGB gamma, reached the same way the harness TUs reach other
 * engine state: plain externs against tables.c / i_video.c. I_SetPalette maps each
 * palette component through gammatable[usegamma] before it lands in the framebuffer
 * (i_video.c I_SetPalette). usegamma is 0 here (never changed by our args), and
 * gammatable[0] is a +1 shift on low values, so we must map the same way to predict
 * the exact framebuffer XRGB rather than assume the raw palette RGB. */
extern const unsigned char gammatable[5][256];
extern int usegamma;

/* Fixed sample points (masked pixel fb[y*w+x] & 0x00FFFFFF) for the two rock-solid
 * bands. Confirmed empirically (maize-156) against the synthetic room at the player-1
 * spawn (map origin, angle 0 / facing east):
 *   - ceiling: viewport center, high row -- a large, full-width blue band [y 0..47].
 *   - floor:   viewport center, low row  -- a large, full-width green band [y 102..167].
 * Both points hold their exact value byte-identically at every tick 1..100 and across
 * E1M1 / MAP01 / MAP02 (pure guest computation, so also host-independent).
 *
 * The wall (CW red) is NOT sampled at a fixed point: in this degenerate single-subsector
 * room the far east wall renders only as a small wedge (E1M1: x[0..8], y[16..101], 144 px;
 * the center of the wall band is background black), and the wedge's exact position is NOT
 * stable across level loads (MAP02 places it differently from E1M1). So CW is tied in by
 * a red-pixel COUNT instead: require at least DM_WALL_MIN_PIXELS pixels in the viewport to
 * equal the exact CW-red value. Empirically E1M1 has 144 and the transition MAP02 has a
 * comparable count; DM_WALL_MIN_PIXELS is set well below both, high enough that a blank,
 * wrong-palette, or ceiling/floor-only frame (zero red) fails. */
#define DM_CEIL_X   160
#define DM_CEIL_Y   24
#define DM_FLOOR_X  160
#define DM_FLOOR_Y  140

/* Minimum exact-CW-red pixel count required in the viewport. Set below the observed
 * counts (E1M1 144, MAP02 comparable) with margin; a garbage/blank frame has zero. */
#define DM_WALL_MIN_PIXELS 40

/* Map a palette RGB triple through the active gamma table into a masked XRGB value,
 * matching i_video.c's I_SetPalette packing (pix = r<<16 | g<<8 | b). */
static uint32_t dm_xrgb(int r, int g, int b)
{
    const unsigned char *t = gammatable[usegamma];
    return ((uint32_t)t[r] << 16) | ((uint32_t)t[g] << 8) | (uint32_t)t[b];
}

/* Weak distinct-color heuristic (precondition / diagnostic only, see file header).
 * Returns 1 when the viewport has >= 2 distinct XRGB colors AND the second-most-common
 * color covers >= DM_MIN_SECOND_AREA pixels. `err` is the platform init-error flag;
 * `fb` is the presented framebuffer (0 when unavailable). */
static int dm_viewport_rendered(const char *tag, const uint32_t *fb, int err,
                                unsigned w, unsigned h)
{
    unsigned maxy, x, y;
    uint32_t colors[DM_NBUCKETS];
    unsigned counts[DM_NBUCKETS];
    unsigned ndistinct = 0;
    unsigned overflow = 0;
    unsigned top1 = 0, top2 = 0;
    unsigned i;

    if (err != 0 || fb == 0) {
        return 0;
    }

    maxy = DM_VIEWPORT_MAX_Y;
    if (maxy > h) {
        maxy = h;
    }

    for (i = 0; i < DM_NBUCKETS; i++) {
        colors[i] = 0;
        counts[i] = 0;
    }

    for (y = 0; y < maxy; y++) {
        for (x = 0; x < w; x++) {
            uint32_t px = fb[y * w + x] & 0x00FFFFFFu;
            unsigned found = 0;
            for (i = 0; i < ndistinct; i++) {
                if (colors[i] == px) {
                    counts[i]++;
                    found = 1;
                    break;
                }
            }
            if (!found) {
                if (ndistinct < DM_NBUCKETS) {
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

    printf("%s: viewport %ux%u distinct=%u%s top1=%u top2=%u\n",
           tag, w, maxy, ndistinct, overflow ? "+" : "", top1, top2);

    if (ndistinct < 2) {
        return 0;
    }
    if (top2 < DM_MIN_SECOND_AREA) {
        return 0;
    }
    return 1;
}

/* Hardened sampled-pixel assertion (the maize-156 PASS/FAIL decision). Returns 1 only
 * when all three surface sample points equal their exact gamma-mapped palette XRGB.
 * Prints one diagnostic line with expected/actual per point; on failure also runs the
 * distinct-color scan for triage. */
static int dm_viewport_render_ok(const char *tag, const uint32_t *fb, int err,
                                 unsigned w, unsigned h)
{
    uint32_t exp_ceil, exp_wall, exp_floor;
    uint32_t got_ceil, got_floor;
    unsigned wall_px = 0;
    int ok;

    if (err != 0 || fb == 0) {
        printf("%s: no framebuffer (err=%d)\n", tag, err);
        return 0;
    }
    /* Keep the fixed sample offsets in-bounds (fb is 320x200 at the geometry override). */
    if (w <= DM_CEIL_X || w <= DM_FLOOR_X || h <= DM_FLOOR_Y) {
        printf("%s: framebuffer too small (%ux%u)\n", tag, w, h);
        return 0;
    }

    exp_ceil  = dm_xrgb(32, 64, 255);   /* CC=50  ceiling blue  */
    exp_wall  = dm_xrgb(255, 0, 0);     /* CW=200 wall red      */
    exp_floor = dm_xrgb(0, 208, 0);     /* CF=100 floor green   */

    got_ceil  = fb[(unsigned)DM_CEIL_Y  * w + DM_CEIL_X ] & 0x00FFFFFFu;
    got_floor = fb[(unsigned)DM_FLOOR_Y * w + DM_FLOOR_X] & 0x00FFFFFFu;

    /* Wall CW is tied in by PRESENCE (a red-pixel count), not a single fixed pixel:
     * unlike the full-width ceiling/floor bands, the far east wall renders in this
     * degenerate single-subsector room only as a small wedge whose exact position is
     * NOT stable across level loads (E1M1 vs the transition's MAP02 place the wedge
     * differently), so a pinned (x, y) wall sample false-fails on MAP02. Counting exact
     * CW-red pixels still ties the assertion to the CW palette constant (a blank,
     * wrong-palette, or all-ceiling/floor frame has zero) while tolerating the wedge's
     * position. */
    {
        unsigned maxy = DM_VIEWPORT_MAX_Y, x, y;
        if (maxy > h) maxy = h;
        for (y = 0; y < maxy; y++) {
            for (x = 0; x < w; x++) {
                if ((fb[y * w + x] & 0x00FFFFFFu) == exp_wall) {
                    wall_px++;
                }
            }
        }
    }

    ok = (got_ceil == exp_ceil) && (got_floor == exp_floor)
         && (wall_px >= DM_WALL_MIN_PIXELS);

    printf("%s: ceil(%d,%d)=%06X/%06X floor(%d,%d)=%06X/%06X wall_red=%u/>=%u %s\n",
           tag,
           DM_CEIL_X, DM_CEIL_Y, got_ceil, exp_ceil,
           DM_FLOOR_X, DM_FLOOR_Y, got_floor, exp_floor,
           wall_px, (unsigned)DM_WALL_MIN_PIXELS,
           ok ? "OK" : "MISMATCH");

    if (!ok) {
        dm_viewport_rendered(tag, fb, err, w, h);   /* triage: distinct-color scan */
    }
    return ok;
}

#endif /* DOOM_VIEWPORT_CHECK_H */
