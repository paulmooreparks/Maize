/*
 * demos/doom/doomgeneric_maize.c: maize-145 DOOM Phase A stub platform layer.
 *
 * This is the Maize replacement for doomgeneric's per-host backend
 * (doomgeneric_sdl.c / _xlib.c / etc.), which doom.sources deliberately omits.
 * Phase A provides MINIMAL, LINKABLE stub bodies ONLY: the goal is to get the
 * ~50k-line doomgeneric + DOOM tree to COMPILE and LINK to a .mzx, not to run,
 * render, clock, or read a keyboard. Every real behaviour (framebuffer present,
 * ms clock, Set-1 -> DOOM key table, WAD) is Phase B.
 *
 * The include path for this TU under cc-maize.sh is `-I toolchain/rt -I demos/doom`
 * (the TU's own directory), NOT the inner submodule dir, so the doomgeneric
 * headers are reached by their repo-relative subpath from demos/doom/. A quoted
 * #include inside those headers (e.g. i_sound.h's "doomtype.h") resolves relative
 * to the header's own directory, so the chain completes without extra -I flags.
 *
 * The submodule is NEVER edited; all Maize glue lives here, outside it.
 */

#include "doomgeneric/doomgeneric/doomgeneric.h"  /* DG_* signatures, DG_ScreenBuffer, pixel_t */
#include "doomgeneric/doomgeneric/doomkeys.h"     /* DOOM key codes (Phase B key table; harmless now) */
#include "doomgeneric/doomgeneric/i_sound.h"      /* sound_module_t / music_module_t for the stub structs */

/*
 * The six DG_* platform functions, with the exact doomgeneric.h signatures.
 * Stubbed for Phase A; the trailing comment on each names the Phase B behaviour.
 */

void DG_Init(void)
{
    /* no-op stub (Phase B: point the framebuffer base at DG_ScreenBuffer). */
}

void DG_DrawFrame(void)
{
    /* no-op stub (Phase B: copy DG_ScreenBuffer to the framebuffer + present). */
}

void DG_SleepMs(uint32_t ms)
{
    (void)ms;   /* no-op stub (Phase B: busy-wait / sys sleep). */
}

uint32_t DG_GetTicksMs(void)
{
    return 0;   /* stub (Phase B: read the ms clock via a syscall/port). */
}

int DG_GetKey(int *pressed, unsigned char *key)
{
    (void)pressed;
    (void)key;
    return 0;   /* stub: no key available (Phase B: Set-1 -> DOOM key table). */
}

void DG_SetWindowTitle(const char *title)
{
    (void)title;   /* no-op stub (Maize has no window manager). */
}

/*
 * Residual sound / music backend symbols.
 *
 * doom.sources drops i_sdlsound.c and i_sdlmusic.c (they reach for host SDL /
 * SDL_mixer), but the retained interface module i_sound.c references two backend
 * descriptor structs by name (DG_sound_module at i_sound.c:77 and
 * DG_music_module at i_sound.c:134), so the link needs them defined. Zero-filled
 * descriptors are correct AND safe: i_sound.c's I_InitSound loop matches a module
 * by its sound_devices list, and with num_sound_devices == 0 no device matches,
 * so the active sound/music module stays NULL and DOOM runs silent. No host audio
 * device exists in the Maize standard set, which is exactly the intended Phase A/B
 * behaviour.
 *
 * Stubbed symbols named for the record (name-your-gaps convention):
 *   - DG_sound_module  (sound_module_t),  replaces i_sdlsound.c's descriptor
 *   - DG_music_module  (music_module_t),  replaces i_sdlmusic.c's descriptor
 */

sound_module_t DG_sound_module = { 0 };
music_module_t DG_music_module = { 0 };
