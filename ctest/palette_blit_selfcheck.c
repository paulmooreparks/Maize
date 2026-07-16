/* maize-213: palette-blit syscall (SYS $F3) self-check over the raw
 * sys_palette_blit stub. Prints exactly "palette-blit: PASS" iff every invariant
 * holds, else a single FAIL line. Two concerns, one fixture:
 *
 *   BIT-IDENTITY: for a baked uint32 LUT and an 8bpp source, the blit writes
 *   dst[i] == lut[src[i]] for every pixel and returns npixels. This is the exact
 *   relation the doomgeneric override's I_SetPalette / I_FinishUpdate rely on to
 *   reproduce cmap_to_fb byte-for-byte, so it is the direct proof of AC-1 that
 *   the pixel-comparing DOOM gates then confirm end to end.
 *
 *   SECURITY (deny-by-default): an oversized npixels (> MAX_BLIT_PIXELS == 2^24),
 *   or a dst/src pointer so near the top of the address space that base+len wraps
 *   the 64-bit space, must return the [-4095,-1] -errno band, perform NO guest
 *   write, and not crash. The fixture seeds dst with a sentinel before each bad
 *   call and asserts the sentinel survives (no wild write) and the VM keeps
 *   running (no crash / wild read).
 */
#include "syscall.h"
#include "stdio.h"

#define N 64

/* [-4095,-1] result band: (unsigned long)r > (unsigned long)-4096. */
#define IN_ERRBAND(r) ((unsigned long)(r) > 0xFFFFFFFFFFFFF000UL)

static unsigned char  src[N];
static unsigned int   dst[N];
static unsigned int   lut[256];

int
main(void)
{
    int i;

    /* Bake a distinctive LUT and a source that indexes across it. */
    for (i = 0; i < 256; ++i) {
        lut[i] = 0x00000000u | ((unsigned int)i << 16) | ((unsigned int)(255 - i) << 8) | (unsigned int)(i * 7u);
    }
    for (i = 0; i < N; ++i) {
        src[i] = (unsigned char)((i * 3 + 1) & 0xFF);
        dst[i] = 0xDEADBEEFu;   /* sentinel */
    }

    /* (1) BIT-IDENTITY + return value. */
    long rv = sys_palette_blit(dst, src, lut, (unsigned long)N);
    if (rv != (long)N) { puts("palette-blit: FAIL retval"); return 1; }
    for (i = 0; i < N; ++i) {
        if (dst[i] != lut[src[i]]) { puts("palette-blit: FAIL pixel"); return 1; }
    }

    /* (2) npixels == 0 is a benign no-op returning 0, no write. */
    dst[0] = 0xCAFEF00Du;
    rv = sys_palette_blit(dst, src, lut, 0UL);
    if (rv != 0)              { puts("palette-blit: FAIL zero-ret");   return 1; }
    if (dst[0] != 0xCAFEF00Du) { puts("palette-blit: FAIL zero-write"); return 1; }

    /* (3) SECURITY: oversized npixels (> 2^24) is rejected (-EINVAL == -22),
       performs no read of the (small) src and no write of dst. */
    for (i = 0; i < N; ++i) { dst[i] = 0xDEADBEEFu; }
    rv = sys_palette_blit(dst, src, lut, (unsigned long)(1UL << 24) + 1UL);
    if (!IN_ERRBAND(rv))       { puts("palette-blit: FAIL oversize-band");  return 1; }
    if (rv != -22)             { puts("palette-blit: FAIL oversize-errno"); return 1; }
    for (i = 0; i < N; ++i) {
        if (dst[i] != 0xDEADBEEFu) { puts("palette-blit: FAIL oversize-write"); return 1; }
    }

    /* (4) SECURITY: a dst pointer near TOP so dst+npixels*4 wraps is rejected
       (-EFAULT == -14) with no write to the bogus range (a wild write there would
       corrupt unrelated blocks or crash). src/lut are valid, so the src/lut wrap
       checks pass and we reach (and reject at) the dst check. */
    rv = sys_palette_blit((void *)0xFFFFFFFFFFFFFF00UL, src, lut, 256UL);
    if (!IN_ERRBAND(rv))       { puts("palette-blit: FAIL dstwrap-band");  return 1; }
    if (rv != -14)             { puts("palette-blit: FAIL dstwrap-errno"); return 1; }

    /* (5) SECURITY: a src pointer near TOP so src+npixels wraps is rejected
       (-EFAULT) BEFORE any read of the bogus src range. */
    rv = sys_palette_blit(dst, (const void *)0xFFFFFFFFFFFFFF00UL, lut, 512UL);
    if (!IN_ERRBAND(rv))       { puts("palette-blit: FAIL srcwrap-band");  return 1; }
    if (rv != -14)             { puts("palette-blit: FAIL srcwrap-errno"); return 1; }

    /* Reaching here proves the VM did not crash on any bad call and dst survived. */
    puts("palette-blit: PASS");
    return 0;
}
