/* maize-343 M1 native reference: a null doomgeneric frontend + timedemo main.
 * Mirrors doom_bench.c: virtual clock (advances only via DG_SleepMs) => pure
 * throughput, wall time measured with CLOCK_MONOTONIC. No display, no audio. */
#include "doomgeneric.h"
#include "i_sound.h"
#include <stdio.h>
#include <stdint.h>
#include <time.h>

#ifndef BENCH_FRAMES
#define BENCH_FRAMES 240
#endif

/* Sound/music descriptor stubs the DOOM link needs (i_sound.c references these
 * by name; normally provided by i_sdlsound.c / i_sdlmusic.c). */
sound_module_t DG_sound_module = { 0 };
music_module_t DG_music_module = { 0 };

static uint32_t g_virtual_ms = 0;

void DG_Init(void) {}
void DG_DrawFrame(void) {}
void DG_SleepMs(uint32_t ms) { g_virtual_ms += ms; }
uint32_t DG_GetTicksMs(void) { return g_virtual_ms; }
int DG_GetKey(int *pressed, unsigned char *key) { (void)pressed; (void)key; return 0; }
void DG_SetWindowTitle(const char *title) { (void)title; }

static unsigned long now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long)ts.tv_sec * 1000000UL + (unsigned long)ts.tv_nsec / 1000UL;
}

int main(int argc, char **argv)
{
    unsigned long b0, b1, r0, r1, boot_us, run_us;
    int i;

    b0 = now_us();
    doomgeneric_Create(argc, argv);
    b1 = now_us();

    r0 = now_us();
    for (i = 0; i < BENCH_FRAMES; i++) { doomgeneric_Tick(); }
    r1 = now_us();

    boot_us = b1 - b0;
    run_us = r1 - r0;
    printf("native: boot %lu us; %d frames %lu us (%lu us/frame)\n",
           boot_us, BENCH_FRAMES, run_us, run_us / (unsigned long)BENCH_FRAMES);
    return 0;
}
