/* demos/doom/doom_viewport_check.h -- shared 3D-viewport "real render" check.
 *
 * Factored out of doom_render_selfcheck.c (maize-154) so the maize-193 transition
 * gate reuses the exact same sampling logic rather than copying it, and so
 * maize-156 can swap the body for a reference-frame checksum in ONE place without
 * touching both harness TUs. Header-only (static) so each TU that includes it gets
 * its own copy with no extra link unit.
 *
 * THE ASSERTION (load-bearing, per doom_render_selfcheck.c's OQ3): sample ONLY the
 * 3D viewport region (rows y < ST_Y, ABOVE the status bar). The status bar blits
 * widgets every tick regardless of the 3D view, so a booted-but-blank 3D render
 * over a black column buffer would leave black + status-bar color = 2 distinct
 * colors and spuriously pass a whole-framebuffer check. Sampling only the viewport
 * defeats that: a blank 3D render is a single color there and fails. We require
 * >= 2 distinct colors AND a substantial second-color area (not a stray pixel).
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

/* Analyse the 3D viewport region of the presented framebuffer. Returns 1 when it
 * looks like a real render: >= 2 distinct XRGB colors AND the second-most-common
 * color covers >= DM_MIN_SECOND_AREA pixels. `tag` labels the diagnostic line so
 * the two gates print distinguishable output. `err` is the platform init-error
 * flag; `fb` is the presented framebuffer (0 when unavailable). */
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

#endif /* DOOM_VIEWPORT_CHECK_H */
