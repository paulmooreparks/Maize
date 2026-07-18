/*
 * demos/doom/doomgeneric_maize.c: maize-153 DOOM Phase B platform layer.
 *
 * This is the Maize replacement for doomgeneric's per-host backend
 * (doomgeneric_sdl.c / _xlib.c / etc.), which doom.sources deliberately omits.
 * Phase A shipped minimal linkable stubs; Phase B replaces them with REAL
 * behaviour over the frozen Maize device / clock / libc surfaces so DOOM can
 * present frames, read the ms clock, and translate keys. Full DOOM boot and
 * first-level render from a real IWAD are Phase C (maize-85).
 *
 * The include path for this TU under cc-maize.sh is `-I toolchain/rt -I demos/doom`
 * (the TU's own directory), NOT the inner submodule dir, so the doomgeneric
 * headers are reached by their repo-relative subpath from demos/doom/. A quoted
 * #include inside those headers (e.g. i_sound.h's "doomtype.h") resolves relative
 * to the header's own directory, so the chain completes without extra -I flags.
 *
 * GEOMETRY (load-bearing, DEC-5): every build compiles this TU with
 * -D DOOMGENERIC_RESX=320 -D DOOMGENERIC_RESY=200 (via the cc-maize.sh -D
 * passthrough, DEC-6). That overrides doomgeneric.h's #ifndef-guarded 640x400
 * defaults, so DG_ScreenBuffer is a native 320x200 XRGB8888 buffer (fb_scaling=1,
 * no scale) exactly matching the default 320x200 Maize framebuffer, and its
 * 0x00RRGGBB packing is byte-identical to Maize format id 1 XRGB8888 with no
 * channel swap (DEC-7). maize-201: DG_MaizeFB is registered as an alias of
 * DG_ScreenBuffer itself (not a separate copy target), so DOOM renders straight
 * into the presented buffer and DG_DrawFrame has no memcpy, just a present.
 *
 * The submodule is NEVER edited; all Maize glue lives here, outside it.
 */

#include "doomgeneric/doomgeneric/doomgeneric.h"  /* DG_* signatures, DG_ScreenBuffer, pixel_t */
#include "doomgeneric/doomgeneric/doomkeys.h"     /* KEY_* symbols for the Set-1 -> DOOM table */
#include "doomgeneric/doomgeneric/i_sound.h"      /* sound_module_t / music_module_t for the stub structs */

#include "mzdev.h"    /* fb_* / kbd_* device stubs (linked via cc-maize.sh --dev, bare-VM) */
#include "syscall.h"  /* sys_clock_ms ($F0), sys_fb_mmap ($FC), sys_fb_present/kbd_read ($FD/$FE) */
#include "string.h"   /* memset / memcpy */

/* maize-251: the two quesOS fb syscalls this shim needs that are NOT in syscall.h's public
 * surface (they are guest-only; the os/quesos fixtures declare them locally the same way).
 * Under bare-VM sys_fb_geometry ($F7) hits the native table's unmatched-case (returns 0,
 * touches NO guest memory), which is exactly the world-detection discriminator below. */
long sys_fb_geometry(void *out);   /* SYS $F7: writes {u32 w,h,format}; quesOS only */
long sys_fb_register(void *base);  /* SYS $F8: RV = slot, or -errno; quesOS only */

/*
 * maize-251 world detection: the SAME doom.mzx binary runs both bare-VM (the standalone
 * demo, direct device stubs) and as a quesOS child (video/input via the fb registration
 * syscalls). dm_probe_world runs a one-time SYS $F7 with a zero-initialized buffer: under
 * quesOS the call writes the real {320,200,1}; under bare-VM the native unmatched-case
 * touches no guest memory, so the buffer stays zero. That difference is a deterministic
 * discriminator (Decision 9324), reusing os/quesos/fb_register.c's own first assertion. The
 * geometry is cached so the world-aware wrappers do not re-probe per frame.
 */
static int      dm_under_quesos = -1;      /* -1 = unprobed, 0 = bare-VM, 1 = quesOS */
static unsigned dm_geo[3] = { 0, 0, 0 };   /* cached {w,h,format} once probed under quesOS */

static int dm_probe_world(void)
{
    if (dm_under_quesos < 0) {
        unsigned int geo[3] = { 0, 0, 0 };
        sys_fb_geometry(geo);
        if (geo[0] == 320 && geo[1] == 200 && geo[2] == 1) {
            dm_under_quesos = 1;
            dm_geo[0] = geo[0]; dm_geo[1] = geo[1]; dm_geo[2] = geo[2];
        } else {
            dm_under_quesos = 0;
        }
    }
    return dm_under_quesos;
}

/* World-aware geometry / present wrappers. Under quesOS use the cached probe geometry and
 * the fb syscalls; under bare-VM defer to the mzdev.h device stubs (unchanged). */
static unsigned dm_fb_width(void)  { return dm_probe_world() ? dm_geo[0] : fb_width(); }
static unsigned dm_fb_height(void) { return dm_probe_world() ? dm_geo[1] : fb_height(); }
static unsigned dm_fb_format(void) { return dm_probe_world() ? dm_geo[2] : fb_format(); }
static void     dm_fb_present(void)
{
    if (dm_under_quesos) { sys_fb_present(); } else { fb_present(); }
}

/*
 * Non-static globals the headless self-check (doom_selfcheck.c) reads back:
 *   DG_MaizeFB          - the present buffer whose base is registered with fb_set_base.
 *                         maize-201: DG_MaizeFB now ALIASES DG_ScreenBuffer (same address,
 *                         set once in DG_Init); DOOM renders straight into the presented
 *                         buffer and there is no longer a copy step.
 *   DG_MaizeInitError   - DG_Init guard result: 0 = ok, 1 = fb geometry mismatch,
 *                         2 = DG_ScreenBuffer is NULL (caller's allocation didn't happen).
 *   DG_MaizeWindowTitle - the last DG_SetWindowTitle pointer (Maize has no WM).
 */
uint32_t   *DG_MaizeFB = 0;
int         DG_MaizeInitError = 0;
const char *DG_MaizeWindowTitle = 0;

/*
 * Set-1 (XT) make code -> DOOM key code, indexed by the 7-bit make code (sc & 0x7F).
 * 0 = unmapped (dropped this tick). Entries are keyed to doomkeys.h SYMBOLS and ASCII,
 * never numeric literals. The make codes mirror src/devices.cpp's map_scancode (the
 * codes the Phase-C real keyboard will deliver); TAB (0x0F) is included per the DG
 * contract even though the current SDL map does not yet emit it (OQ-3, a Phase-C gap).
 * Letters deliver LOWERCASE ASCII unconditionally; DOOM does its own shift casing from
 * the KEY_RSHIFT event (DEC: do not case in the platform).
 *
 * Fire and use map to DOOM's ACTION codes (KEY_FIRE / KEY_USE), not the physical keys:
 * the default in-game bindings are key_fire = KEY_FIRE and key_use = KEY_USE (m_controls.c),
 * so gamekeydown[key_fire]/[key_use] only ever match those action codes. This mirrors the
 * reference doomgeneric_sdl.c, where LCTRL/RCTRL -> KEY_FIRE and SPACE -> KEY_USE.
 */
static const unsigned char scancode_to_doom[128] = {
    [0x01] = KEY_ESCAPE,
    [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4', [0x06] = '5',
    [0x07] = '6', [0x08] = '7', [0x09] = '8', [0x0A] = '9', [0x0B] = '0',
    [0x0C] = KEY_MINUS, [0x0D] = KEY_EQUALS,   /* view size / automap zoom out, in */
    [0x0E] = KEY_BACKSPACE,                     /* menu text editing */
    [0x0F] = KEY_TAB,
    [0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r', [0x14] = 't',
    [0x15] = 'y', [0x16] = 'u', [0x17] = 'i', [0x18] = 'o', [0x19] = 'p',
    [0x1C] = KEY_ENTER,
    [0x1D] = KEY_FIRE,      /* ctrl -> fire: DOOM binds key_fire = KEY_FIRE (0xa3), NOT
                               KEY_RCTRL; gamekeydown[key_fire] never sees a raw KEY_RCTRL.
                               Mirrors doomgeneric_sdl.c's LCTRL/RCTRL -> KEY_FIRE. */
    [0x1E] = 'a', [0x1F] = 's', [0x20] = 'd', [0x21] = 'f', [0x22] = 'g',
    [0x23] = 'h', [0x24] = 'j', [0x25] = 'k', [0x26] = 'l',
    [0x2A] = KEY_RSHIFT,    /* LShift -> DOOM run/strafe modifier */
    [0x2C] = 'z', [0x2D] = 'x', [0x2E] = 'c', [0x2F] = 'v', [0x30] = 'b',
    [0x31] = 'n', [0x32] = 'm',
    /* comma (0x33) / period (0x34) intentionally unmapped: strafe is Alt + arrow (the
       official binding on the HELP screen), so the , / . strafe keys are omitted. */
    [0x36] = KEY_RSHIFT,    /* RShift -> run/strafe modifier (same as LShift) */
    [0x38] = KEY_RALT,      /* Alt -> DOOM strafe modifier (key_strafe default) */
    [0x39] = KEY_USE,       /* space -> use: DOOM binds key_use = KEY_USE (0xa2), NOT the
                               ' ' character; mirrors doomgeneric_sdl.c's SPACE -> KEY_USE. */
    [0x3B] = KEY_F1,  [0x3C] = KEY_F2,  [0x3D] = KEY_F3,  [0x3E] = KEY_F4,
    [0x3F] = KEY_F5,  [0x40] = KEY_F6,  [0x41] = KEY_F7,  [0x42] = KEY_F8,
    [0x43] = KEY_F9,  [0x44] = KEY_F10, [0x57] = KEY_F11, [0x58] = KEY_F12,
    [0x45] = KEY_PAUSE,     /* SDL Pause is mapped to the otherwise-free Set-1 slot 0x45 */
    [0x48] = KEY_UPARROW,
    [0x4B] = KEY_LEFTARROW,
    [0x4D] = KEY_RIGHTARROW,
    [0x50] = KEY_DOWNARROW,
};

/*
 * DG_Init: register DG_ScreenBuffer itself as the present buffer in guest RAM
 * (maize-201). DOOM's renderer already writes into DG_ScreenBuffer every frame, so
 * aliasing DG_MaizeFB to that same address (instead of a separately malloc'd copy
 * target) makes DG_DrawFrame's present a straight fb_present() with no memcpy.
 *
 * Geometry guard (loud fail on mismatch): require the fb to be exactly
 * DOOMGENERIC_RESX x DOOMGENERIC_RESY (== 320x200 under the -D override) and format
 * id 1 (XRGB8888). A mismatch would make the straight no-convert present show
 * garbage, so on mismatch we set DG_MaizeInitError and do NOT set a base or present
 * (the self-check asserts DG_MaizeInitError == 0).
 *
 * doomgeneric_Create mallocs DG_ScreenBuffer before calling DG_Init and never frees
 * or reallocates it for the life of the process (confirmed: the only allocation site
 * in the submodule), so registering its address here once is safe for the whole
 * guest run. DG_Init no longer allocates anything itself; a NULL DG_ScreenBuffer
 * means the caller's allocation didn't happen, so we fail loud instead of presenting
 * from an unset base.
 */
void DG_Init(void)
{
    unsigned w = dm_fb_width();
    unsigned h = dm_fb_height();
    unsigned f = dm_fb_format();

    if (w != DOOMGENERIC_RESX || h != DOOMGENERIC_RESY || f != 1) {
        DG_MaizeInitError = 1;
        return;
    }
    if (DG_ScreenBuffer == 0) {
        DG_MaizeInitError = 2;
        return;
    }
    DG_MaizeInitError = 0;

    if (dm_under_quesos) {
        /* quesOS (maize-251): DG_MaizeFB is a SEPARATE, physically-contiguous fb-mmap buffer
         * (not an alias of the malloc'd DG_ScreenBuffer), because sys_fb_register's hardened
         * contiguity check requires it. Map the buffer, then register it as the scanout base.
         * Under maize-264's session-host transport the sys_fb_register call also launches or
         * attaches the presenter window (blocking the VM up to ~3s the first time). A mmap
         * failure is init-error 4; a register failure (incl. -ENODEV under --fb-no-display,
         * -EBUSY/-ENOSPC on a re-register) is init-error 3. */
        long va = sys_fb_mmap();
        if (va <= 0) {
            DG_MaizeInitError = 4;
            return;
        }
        DG_MaizeFB = (uint32_t *)(unsigned long)va;
        if (sys_fb_register(DG_MaizeFB) < 0) {
            DG_MaizeInitError = 3;
            return;
        }
    } else {
        /* bare-VM: the existing maize-201 zero-copy alias. DOOM renders straight into the
         * presented buffer, so DG_DrawFrame is a plain present with no copy. */
        DG_MaizeFB = (uint32_t *)DG_ScreenBuffer;
        fb_set_base(DG_MaizeFB);
    }
}

/*
 * DG_DrawFrame: present the buffer DOOM already rendered into (maize-201). Since
 * DG_Init aliased DG_MaizeFB to DG_ScreenBuffer, there is no longer a separate copy
 * target: doomgeneric's renderer writes straight into the presented buffer every
 * frame, so drawing a frame is just the present call.
 */
void DG_DrawFrame(void)
{
    if (dm_under_quesos) {
        /* quesOS (maize-251): DG_MaizeFB (fb-mmap'd, contiguity-guaranteed) and DG_ScreenBuffer
         * (malloc'd, inside the small sbrk heap) are two DIFFERENT buffers, so copy the frame
         * DOOM rendered into the registered scanout buffer before presenting. */
        memcpy(DG_MaizeFB, DG_ScreenBuffer,
               (unsigned long)DOOMGENERIC_RESX * (unsigned long)DOOMGENERIC_RESY * 4u);
    }
    dm_fb_present();
}

/*
 * DG_SleepMs: busy-wait on the monotonic ms clock. sys_clock_ms is real host time and
 * advances independent of guest activity, so the loop always terminates.
 */
void DG_SleepMs(uint32_t ms)
{
    unsigned long start = sys_clock_ms();
    while ((unsigned long)(sys_clock_ms() - start) < (unsigned long)ms) {
        /* spin */
    }
}

/*
 * DG_GetTicksMs: low-32 truncation of the 64-bit monotonic ms clock. DOOM uses ms
 * deltas, so the ~49.7-day wrap is irrelevant.
 */
uint32_t DG_GetTicksMs(void)
{
    return (uint32_t)sys_clock_ms();
}

/*
 * DG_GetKey: dequeue Set-1 make/break events and translate each to a DOOM key.
 *
 * doomgeneric's input layer (i_input.c I_GetEvent) drains this in a tight loop each
 * frame until it returns 0, then D_ProcessEvents dispatches the posted events and
 * TryRunTics builds the game tics that sample gamekeydown[]. At a low frame rate a
 * quick TAP delivers both the make (keydown) and its break (keyup) inside ONE drain
 * pass, so gamekeydown[key] is set and cleared before any tic samples it and the
 * shot/use is silently lost (observed: fire fires only unreliably).
 *
 * Fix (no submodule edit): defer a break to the NEXT drain pass whenever the same
 * key's make was delivered in the CURRENT pass. That guarantees the key reads as down
 * across at least one built tic, so a tap registers regardless of frame rate. Held
 * keys (make and break land in different passes) are never deferred, so continuous
 * movement and the run/strafe modifiers are unchanged. A pass ends when the device
 * drains (returns 0) or when a break is deferred; per-pass make tracking resets then.
 *
 * Returns 1 with *pressed/*key set on a delivered event, else 0 (pass complete).
 */
int DG_GetKey(int *pressed, unsigned char *key)
{
    static int made_this_pass[128];   /* make code -> its make already went out this pass */
    static int have_held = 0;         /* a scancode consumed but deferred to the next pass */
    static unsigned held_sc = 0;

    unsigned sc;
    unsigned make;
    unsigned char dk;
    int is_break;

    for (;;) {
        if (have_held) {
            sc = held_sc;              /* deliver the deferred break first, new pass */
            have_held = 0;
        }
        else if (dm_under_quesos) {
            /* quesOS (maize-251): drain one scancode via the ACL-gated keyboard syscall. A
             * result < 0 means no key this pass -- either -EAGAIN (queue drained) or -EACCES
             * (this process is not the active fb-slot owner); both end the pass. */
            long r = sys_kbd_read();
            if (r < 0) {
                memset(made_this_pass, 0, sizeof made_this_pass);
                return 0;
            }
            sc = (unsigned)r;
        }
        else {
            if (!(kbd_status() & 1u)) {
                memset(made_this_pass, 0, sizeof made_this_pass);
                return 0;              /* device drained: pass over */
            }
            sc = kbd_read();           /* consumes + clears key-available */
        }

        make = sc & 0x7Fu;
        dk = scancode_to_doom[make];
        if (dk == 0) {
            continue;                  /* unmapped: drop it, keep draining */
        }

        is_break = (sc & 0x80u) != 0;
        if (is_break && made_this_pass[make]) {
            held_sc = sc;              /* same key's make went out this pass: defer break */
            have_held = 1;
            memset(made_this_pass, 0, sizeof made_this_pass);
            return 0;                  /* end the pass so a tic samples the key as down */
        }
        if (!is_break) {
            made_this_pass[make] = 1;
        }
        *pressed = is_break ? 0 : 1;   /* make = down, break = up */
        *key = dk;
        return 1;
    }
}

/*
 * DG_SetWindowTitle: Maize has no window manager, so this only stores the pointer (which
 * makes it self-checkable).
 */
void DG_SetWindowTitle(const char *title)
{
    DG_MaizeWindowTitle = title;
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
 * device exists in the Maize standard set, which is exactly the intended behaviour.
 *
 * Stubbed symbols named for the record (name-your-gaps convention):
 *   - DG_sound_module  (sound_module_t),  replaces i_sdlsound.c's descriptor
 *   - DG_music_module  (music_module_t),  replaces i_sdlmusic.c's descriptor
 */

sound_module_t DG_sound_module = { 0 };
music_module_t DG_music_module = { 0 };
