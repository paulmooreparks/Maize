/* demos/doom/doom_selfcheck.c -- headless CI gate for the Phase B DG platform (maize-153).
 *
 * A standalone guest-C `main` linked ONLY with the platform TU doomgeneric_maize.c (a
 * minimal link: these two TUs + the mzdev device shim via --dev + the RT libc set). It
 * does NOT pull doom.sources or doomgeneric.c, so no full DOOM boot happens here (that
 * needs an IWAD, which is Phase C). Instead it exercises each DG_* primitive in isolation
 * through the real cc-maize.sh pipeline, mirroring demos/terminal/terminal_selfcheck.c.
 *
 * The build compiles both TUs with -D DOOMGENERIC_RESX=320 -D DOOMGENERIC_RESY=200 (the
 * cc-maize.sh -D passthrough), so DOOMGENERIC_RESX/RESY == 320/200 == the default Maize
 * framebuffer, DG_ScreenBuffer is native 320x200, and DG_DrawFrame is a straight memcpy.
 *
 * This TU DEFINES DG_ScreenBuffer (the extern doomgeneric.c would otherwise provide), so
 * no doomgeneric object is needed. Phases:
 *   Present : fill DG_ScreenBuffer with a deterministic pattern, DG_Init + DG_DrawFrame,
 *             assert the geometry guard passed, fb_present_valid()==1, and every presented
 *             pixel in DG_MaizeFB equals the source over the full fb region.
 *   Keymap  : drain DG_GetKey across an injected Set-1 make/break scancode sequence and
 *             assert each (pressed,key) against doomkeys.h SYMBOLS / ASCII.
 *   Clock   : DG_GetTicksMs advances >= 10 across DG_SleepMs(10); back-to-back reads are
 *             non-decreasing.
 *   WAD-read: fopen/fread/fseek/ftell/fclose a committed binary fixture, byte-exact (the
 *             same libc FILE* path w_file_stdc.c uses for the IWAD).
 *   Zone    : an ~8 MiB malloc round-trips first/last byte and frees (de-risks Z_Init).
 *   Title   : DG_SetWindowTitle stores the pointer.
 *
 * Prints exactly "doom: PASS" on success (else "doom: FAIL"). Wired into
 * scripts/run-ctest.sh (run_doom_selfcheck), which mounts the fixture :ro and pipes the
 * scancode sequence via `maize --input=keyboard`.
 */
#include "doomgeneric/doomgeneric/doomgeneric.h"  /* DG_* signatures, DG_ScreenBuffer, pixel_t */
#include "doomgeneric/doomgeneric/doomkeys.h"     /* KEY_* symbols */
#include "mzdev.h"                                /* fb_* / kbd_* */
#include "stdio.h"                                /* FILE*, fopen/fread/fseek/ftell/fclose, puts */
#include "stdlib.h"                               /* malloc / free */

/* DG_ScreenBuffer is the extern declared in doomgeneric.h; the self-check owns it here
 * (a tentative definition), so no doomgeneric .c object is co-linked. */
pixel_t *DG_ScreenBuffer;

/* Globals defined by the platform TU (doomgeneric_maize.c), read back here. */
extern uint32_t   *DG_MaizeFB;
extern int         DG_MaizeInitError;
extern const char *DG_MaizeWindowTitle;

static int selfcheck_ok = 1;

static void expect(int cond)
{
    if (!cond) {
        selfcheck_ok = 0;
    }
}

/* Deterministic per-pixel pattern, masked to XRGB (top byte 0). */
static uint32_t px_rule(unsigned i)
{
    uint32_t x = (uint32_t)i * 2654435761u;
    return (0x00112233u ^ x) & 0x00FFFFFFu;
}

/* Deterministic per-byte fixture pattern. 31 is coprime to 256, so (31*i+7) mod 256 is a
 * bijection over i in [0,256): every byte value 0x00..0xFF (incl 0x0A/0x0D/0xFF) appears in
 * the first 256 bytes, which is exactly the region the WAD-read phase compares. */
static unsigned char file_rule(unsigned i)
{
    return (unsigned char)((i * 31u + 7u) & 0xFFu);
}

/* Present: fill the source, present it, and read the framebuffer back pixel-for-pixel. */
static void check_present(void)
{
    unsigned n = (unsigned)DOOMGENERIC_RESX * (unsigned)DOOMGENERIC_RESY;   /* 64000 */
    unsigned w, h, m, i;
    unsigned mism = 0;

    DG_ScreenBuffer = (pixel_t *)malloc((size_t)n * sizeof(pixel_t));
    expect(DG_ScreenBuffer != 0);
    if (DG_ScreenBuffer == 0) {
        return;
    }
    for (i = 0; i < n; i++) {
        DG_ScreenBuffer[i] = px_rule(i);
    }

    DG_Init();
    expect(DG_MaizeInitError == 0);   /* geometry guard passed */
    if (DG_MaizeInitError != 0) {
        return;
    }

    DG_DrawFrame();
    expect(fb_present_valid() == 1);

    w = fb_width();
    h = fb_height();
    m = w * h;
    for (i = 0; i < m; i++) {
        if (DG_MaizeFB[i] != DG_ScreenBuffer[i]) {
            mism = 1;
            break;
        }
    }
    expect(mism == 0);
}

/* Keymap: drain DG_GetKey until the 11 injected make/break events are collected, then
 * compare each (pressed,key) against the expected doomkeys.h SYMBOL / ASCII. The injected
 * sequence (run-ctest side, octal): 036 236 110 113 115 120 035 071 034 001 017 =
 *   1E('a' make), 9E('a' break), 48(up), 4B(left), 4D(right), 50(down),
 *   1D(ctrl), 39(space), 1C(enter), 01(esc), 0F(tab). */
static void check_keymap(void)
{
    static const unsigned char exp_key[11] = {
        'a', 'a',
        KEY_UPARROW, KEY_LEFTARROW, KEY_RIGHTARROW, KEY_DOWNARROW,
        KEY_RCTRL, ' ', KEY_ENTER, KEY_ESCAPE, KEY_TAB
    };
    static const int exp_pressed[11] = { 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1 };
    static int got_pressed[11];
    static unsigned char got_key[11];
    int p, kc = 0, j;
    unsigned char k;

    while (kc < 11) {
        if (DG_GetKey(&p, &k)) {
            got_pressed[kc] = p;
            got_key[kc] = k;
            kc++;
        }
    }
    for (j = 0; j < 11; j++) {
        expect(got_pressed[j] == exp_pressed[j]);
        expect(got_key[j] == exp_key[j]);
    }
}

/* Clock: DG_GetTicksMs advances across a DG_SleepMs(10); a follow-up read is non-decreasing. */
static void check_clock(void)
{
    uint32_t t0 = DG_GetTicksMs();
    uint32_t t1, t2;

    DG_SleepMs(10);
    t1 = DG_GetTicksMs();
    expect((uint32_t)(t1 - t0) >= 10u);

    t2 = DG_GetTicksMs();
    expect(t2 >= t1);
}

/* WAD-read: the libc FILE* primitive on the committed binary fixture (the IWAD path). */
static void check_wad(void)
{
    unsigned char buf[256];
    FILE *f;
    unsigned i;
    long pos;

    f = fopen("/ro/doomread.bin", "rb");
    expect(f != 0);
    if (f == 0) {
        return;
    }

    expect(fread(buf, 1, 256, f) == 256);
    for (i = 0; i < 256; i++) {
        expect(buf[i] == file_rule(i));   /* byte-exact incl 0x0A/0x0D: no text mangling */
    }

    expect(fseek(f, 200, SEEK_SET) == 0);
    expect(ftell(f) == 200);
    expect(fread(buf, 1, 1, f) == 1);
    expect(buf[0] == file_rule(200));

    expect(fseek(f, 0, SEEK_END) == 0);
    pos = ftell(f);
    expect(pos == 512);

    expect(fclose(f) == 0);
}

/* Zone smoke: de-risk Z_Init's big zone allocation on Maize sparse memory. */
static void check_zone(void)
{
    unsigned long sz = 8ul * 1024ul * 1024ul;
    unsigned char *big = (unsigned char *)malloc(sz);

    expect(big != 0);
    if (big == 0) {
        return;
    }
    big[0] = 0xAB;
    big[sz - 1] = 0xCD;
    expect(big[0] == 0xAB);
    expect(big[sz - 1] == 0xCD);
    free(big);
}

/* Title: DG_SetWindowTitle stores the pointer (Maize has no window manager). */
static void check_title(void)
{
    const char *t = "DOOM";
    DG_SetWindowTitle(t);
    expect(DG_MaizeWindowTitle == t);
}

int
main(void)
{
    check_present();
    check_keymap();
    check_clock();
    check_wad();
    check_zone();
    check_title();

    puts(selfcheck_ok ? "doom: PASS" : "doom: FAIL");
    return 0;
}
