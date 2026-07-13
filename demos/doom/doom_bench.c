/* demos/doom/doom_bench.c -- headless DOOM performance benchmark.
 *
 * A fixed, deterministic workload for measuring Maize VM speed across
 * optimizations. It boots the WHOLE DOOM engine against the minimal synthetic
 * IWAD (same link shape as doom_render_selfcheck.c) and reports two numbers,
 * both measured with the guest monotonic ms clock (SYS $F0 -> real host time),
 * so the emulated wall-clock IS the metric:
 *
 *   - boot ms:  doomgeneric_Create (WAD load + zone init + first render). This is
 *               uncapped and extremely memory-access heavy, so it isolates the
 *               guest-memory-subsystem cost with no frame-rate limiter in the way.
 *   - frame ms: BENCH_FRAMES doomgeneric_Tick calls on the static -warp/-nomonsters
 *               scene (render + game logic + present). While the VM runs below
 *               DOOM's 35 Hz cap (it does), this is VM-bound and reflects raw speed.
 *
 * Run headless, mirroring the render gate:
 *   maize --mount <minwad-dir>=/ro:ro doom_bench.mzx -iwad /ro/min.wad -warp 1 1 -nomonsters
 *
 * Lower ms = faster. Not a CI gate; a developer benchmark.
 */
#include "doomgeneric/doomgeneric/doomgeneric.h"  /* doomgeneric_Create/Tick */
#include "syscall.h"                              /* sys_clock_ms (SYS $F0) */
#include "stdio.h"                                /* printf */

#ifndef BENCH_FRAMES
#define BENCH_FRAMES 120
#endif

int main(int argc, char **argv)
{
    unsigned long boot0, boot1, run0, run1, boot_ms, run_ms;
    int i;

    boot0 = sys_clock_ms();
    doomgeneric_Create(argc, argv);   /* DG_Init + D_DoomMain + first render */
    boot1 = sys_clock_ms();

    run0 = sys_clock_ms();
    for (i = 0; i < BENCH_FRAMES; i++) {
        doomgeneric_Tick();
    }
    run1 = sys_clock_ms();

    boot_ms = boot1 - boot0;
    run_ms = run1 - run0;

    printf("bench: boot %lu ms; %d frames %lu ms (%lu us/frame)\n",
           boot_ms, BENCH_FRAMES, run_ms,
           run_ms ? (run_ms * 1000UL) / (unsigned long)BENCH_FRAMES : 0UL);
    return 0;
}
