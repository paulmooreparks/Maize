// Emacs style mode select   -*- C++ -*-
//-----------------------------------------------------------------------------
//
// $Id:$
//
// Copyright (C) 1993-1996 by id Software, Inc.
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// $Log:$
//
// DESCRIPTION:
//	DOOM graphics stuff for X11, UNIX.
//
//-----------------------------------------------------------------------------
//
// maize-213 i_video OVERRIDE (Maize, outside the pinned doomgeneric submodule).
//
// This is a faithful transcription of
// demos/doom/doomgeneric/doomgeneric/i_video.c with EXACTLY two behavioral
// changes; every other i_video interface symbol is byte-for-byte the same logic,
// so nothing can diverge from the submodule's per-pixel output:
//
//   1. I_SetPalette additionally bakes a 256-entry uint32 XRGB8888 LUT
//      (maize_fb_palette) exactly as the submodule builds colors[], so the LUT
//      equals colors[] reinterpreted as uint32.
//   2. I_FinishUpdate replaces the per-scanline cmap_to_fb 8bpp -> XRGB8888 loop
//      with one sys_palette_blit (SYS $F3) that runs dst[i] = lut[src[i]] at host
//      memcpy speed. Output is byte-identical to cmap_to_fb by construction.
//
// The file-local converters cmap_to_fb / cmap_to_rgb565 and the struct color
// colors[256] array are DROPPED: their only reader was cmap_to_fb, which the blit
// replaces. doom.sources swaps the submodule i_video.c for this TU, so exactly one
// definition of each i_video symbol remains.
//
// INCLUDE STYLE (load-bearing): cc-maize.sh compiles this TU with cpp flags
// `-nostdinc -I toolchain/rt -I demos/doom` (the TU's own directory), NOT the
// inner submodule dir, so bare "i_video.h" would not resolve. Top-level includes
// use the repo-relative doomgeneric/doomgeneric/ subpath, exactly like
// doomgeneric_maize.c; quoted includes INSIDE those headers resolve relative to
// the header's own directory, so the chain completes.
//-----------------------------------------------------------------------------

static const char
rcsid[] = "$Id: i_x.c,v 1.6 1997/02/03 22:45:10 b1 Exp $";

#include "doomgeneric/doomgeneric/config.h"
#include "doomgeneric/doomgeneric/v_video.h"
#include "doomgeneric/doomgeneric/m_argv.h"
#include "doomgeneric/doomgeneric/d_event.h"
#include "doomgeneric/doomgeneric/d_main.h"
#include "doomgeneric/doomgeneric/i_video.h"
#include "doomgeneric/doomgeneric/i_system.h"
#include "doomgeneric/doomgeneric/z_zone.h"

#include "doomgeneric/doomgeneric/tables.h"
#include "doomgeneric/doomgeneric/doomkeys.h"

#include "doomgeneric/doomgeneric/doomgeneric.h"

#include "syscall.h"   /* sys_palette_blit (SYS $F3, maize-213) */

#include <stdbool.h>
#include <stdlib.h>

#include <fcntl.h>

#include <stdarg.h>

#include <sys/types.h>

//#define CMAP256

struct FB_BitField
{
	uint32_t offset;			/* beginning of bitfield	*/
	uint32_t length;			/* length of bitfield		*/
};

struct FB_ScreenInfo
{
	uint32_t xres;			/* visible resolution		*/
	uint32_t yres;
	uint32_t xres_virtual;		/* virtual resolution		*/
	uint32_t yres_virtual;

	uint32_t bits_per_pixel;		/* guess what			*/

							/* >1 = FOURCC			*/
	struct FB_BitField red;		/* bitfield in s_Fb mem if true color, */
	struct FB_BitField green;	/* else only length is significant */
	struct FB_BitField blue;
	struct FB_BitField transp;	/* transparency			*/
};

static struct FB_ScreenInfo s_Fb;
int fb_scaling = 1;
int usemouse = 0;

/* maize-213: the 256-entry XRGB8888 LUT the palette-blit syscall indexes. Baked
   in I_SetPalette from the same gammatable read + RGB pack the submodule applied
   to colors[], so it equals the old colors[] reinterpreted as uint32. Replaces
   the dropped struct color colors[256] (whose only reader was cmap_to_fb). */
static uint32_t maize_fb_palette[256];

void I_GetEvent(void);

// The screen buffer; this is modified to draw things to the screen

byte *I_VideoBuffer = NULL;

// If true, game is running as a screensaver

boolean screensaver_mode = false;

// Flag indicating whether the screen is currently visible:
// when the screen isnt visible, don't render the screen

boolean screenvisible;

// Mouse acceleration
//
// This emulates some of the behavior of DOS mouse drivers by increasing
// the speed when the mouse is moved fast.
//
// The mouse input values are input directly to the game, but when
// the values exceed the value of mouse_threshold, they are multiplied
// by mouse_acceleration to increase the speed.

float mouse_acceleration = 2.0;
int mouse_threshold = 10;

// Gamma correction level to use

int usegamma = 0;

typedef struct
{
	byte r;
	byte g;
	byte b;
} col_t;

// Palette converted to RGB565

static uint16_t rgb565_palette[256];

void I_InitGraphics (void)
{
    int i, gfxmodeparm;
    char *mode;

	memset(&s_Fb, 0, sizeof(struct FB_ScreenInfo));
	s_Fb.xres = DOOMGENERIC_RESX;
	s_Fb.yres = DOOMGENERIC_RESY;
	s_Fb.xres_virtual = s_Fb.xres;
	s_Fb.yres_virtual = s_Fb.yres;

#ifdef CMAP256

	s_Fb.bits_per_pixel = 8;

#else  // CMAP256

	gfxmodeparm = M_CheckParmWithArgs("-gfxmode", 1);

	if (gfxmodeparm) {
		mode = myargv[gfxmodeparm + 1];
	}
	else {
		// default to rgba8888 like the old behavior, for compatibility
		// maybe could warn here?
		mode = "rgba8888";
	}

	if (strcmp(mode, "rgba8888") == 0) {
		// default mode
		s_Fb.bits_per_pixel = 32;

		s_Fb.blue.length = 8;
		s_Fb.green.length = 8;
		s_Fb.red.length = 8;
		s_Fb.transp.length = 8;

		s_Fb.blue.offset = 0;
		s_Fb.green.offset = 8;
		s_Fb.red.offset = 16;
		s_Fb.transp.offset = 24;
	}

	else if (strcmp(mode, "rgb565") == 0) {
		s_Fb.bits_per_pixel = 16;

		s_Fb.blue.length = 5;
		s_Fb.green.length = 6;
		s_Fb.red.length = 5;
		s_Fb.transp.length = 0;

		s_Fb.blue.offset = 11;
		s_Fb.green.offset = 5;
		s_Fb.red.offset = 0;
		s_Fb.transp.offset = 16;
	}
	else
		I_Error("Unknown gfxmode value: %s\n", mode);


#endif  // CMAP256

    printf("I_InitGraphics: framebuffer: x_res: %d, y_res: %d, x_virtual: %d, y_virtual: %d, bpp: %d\n",
            s_Fb.xres, s_Fb.yres, s_Fb.xres_virtual, s_Fb.yres_virtual, s_Fb.bits_per_pixel);

    printf("I_InitGraphics: framebuffer: RGBA: %d%d%d%d, red_off: %d, green_off: %d, blue_off: %d, transp_off: %d\n",
            s_Fb.red.length, s_Fb.green.length, s_Fb.blue.length, s_Fb.transp.length, s_Fb.red.offset, s_Fb.green.offset, s_Fb.blue.offset, s_Fb.transp.offset);

    printf("I_InitGraphics: DOOM screen size: w x h: %d x %d\n", SCREENWIDTH, SCREENHEIGHT);


    i = M_CheckParmWithArgs("-scaling", 1);
    if (i > 0) {
        i = atoi(myargv[i + 1]);
        fb_scaling = i;
        printf("I_InitGraphics: Scaling factor: %d\n", fb_scaling);
    } else {
        fb_scaling = s_Fb.xres / SCREENWIDTH;
        if (s_Fb.yres / SCREENHEIGHT < fb_scaling)
            fb_scaling = s_Fb.yres / SCREENHEIGHT;
        printf("I_InitGraphics: Auto-scaling factor: %d\n", fb_scaling);
    }


    /* Allocate screen to draw to */
	I_VideoBuffer = (byte*)Z_Malloc (SCREENWIDTH * SCREENHEIGHT, PU_STATIC, NULL);  // For DOOM to draw on

	screenvisible = true;

    extern void I_InitInput(void);
    I_InitInput();
}

void I_ShutdownGraphics (void)
{
	Z_Free (I_VideoBuffer);
}

void I_StartFrame (void)
{

}

void I_StartTic (void)
{
	I_GetEvent();
}

void I_UpdateNoBlit (void)
{
}

//
// I_FinishUpdate
//

void I_FinishUpdate (void)
{
    /* maize-213: Maize pins the framebuffer to 320x200 XRGB8888 with fb_scaling==1
       (DG_Init guards format id 1), so the submodule's scanline cmap_to_fb loop
       reduces to one contiguous SCREENWIDTH*SCREENHEIGHT == 64000 pixel blit with
       x_offset == x_offset_end == y_offset == 0. Hard-assert the pinned geometry
       rather than reproduce the scaled / rgb565 fallback Maize never exercises
       (OQ-2). */
    if (fb_scaling != 1
        || s_Fb.bits_per_pixel != 32
        || s_Fb.xres != (uint32_t)SCREENWIDTH
        || s_Fb.yres != (uint32_t)SCREENHEIGHT)
    {
        I_Error("I_FinishUpdate: unsupported framebuffer geometry "
                "(%dx%d bpp=%d scaling=%d); Maize requires 320x200 XRGB8888",
                s_Fb.xres, s_Fb.yres, s_Fb.bits_per_pixel, fb_scaling);
        return;
    }

    /* dst[i] = maize_fb_palette[src[i]] for the whole frame, at host memcpy speed.
       Byte-identical to the submodule's per-pixel cmap_to_fb: the LUT bakes the
       same gammatable read + (r<<16)|(g<<8)|b pack cmap_to_fb applied per pixel. */
    sys_palette_blit((void *)DG_ScreenBuffer,
                     (const void *)I_VideoBuffer,
                     (const unsigned int *)maize_fb_palette,
                     (unsigned long)(SCREENWIDTH * SCREENHEIGHT));

	DG_DrawFrame();
}

//
// I_ReadScreen
//
void I_ReadScreen (byte* scr)
{
    memcpy (scr, I_VideoBuffer, SCREENWIDTH * SCREENHEIGHT);
}

//
// I_SetPalette
//
#define GFX_RGB565(r, g, b)			((((r & 0xF8) >> 3) << 11) | (((g & 0xFC) >> 2) << 5) | ((b & 0xF8) >> 3))
#define GFX_RGB565_R(color)			((0xF800 & color) >> 11)
#define GFX_RGB565_G(color)			((0x07E0 & color) >> 5)
#define GFX_RGB565_B(color)			(0x001F & color)

void I_SetPalette (byte* palette)
{
	int i;

    /* maize-213: bake the 256-entry XRGB8888 LUT the palette-blit syscall indexes.
       This is EXACTLY the pixel the submodule's cmap_to_fb wrote: the submodule set
       colors[i] = { a=0, r=gammatable[usegamma][R], g=gammatable[usegamma][G],
       b=gammatable[usegamma][B] }, whose raw little-endian uint32 (struct color is
       b:8,g:8,r:8,a:8) is (r<<16)|(g<<8)|b, and cmap_to_fb's 32bpp path packed the
       identical value (red.offset=16, green.offset=8, blue.offset=0; transp NOT
       ORed). Baking it here (256x per palette change, not per pixel) makes bit-
       identity follow from a shared source expression.

       The three gammatable reads are kept in SEPARATE statements so R, G, B are
       consumed in that fixed order (a single (..*p++)|(..*p++)|(..*p++) expression
       leaves the three *p++ subexpressions unsequenced, so its channel order is
       unspecified); this matches the submodule's own three sequenced assignments. */
	for (i = 0; i < 256; ++i) {
		byte r = gammatable[usegamma][*palette++];
		byte g = gammatable[usegamma][*palette++];
		byte b = gammatable[usegamma][*palette++];
		maize_fb_palette[i] = ((uint32_t)r << 16)
		                    | ((uint32_t)g <<  8)
		                    |  (uint32_t)b;
	}
}

// Given an RGB value, find the closest matching palette index.

int I_GetPaletteIndex (int r, int g, int b)
{
    int best, best_diff, diff;
    int i;
    col_t color;

    printf("I_GetPaletteIndex\n");

    best = 0;
    best_diff = INT_MAX;

    for (i = 0; i < 256; ++i)
    {
    	color.r = GFX_RGB565_R(rgb565_palette[i]);
    	color.g = GFX_RGB565_G(rgb565_palette[i]);
    	color.b = GFX_RGB565_B(rgb565_palette[i]);

        diff = (r - color.r) * (r - color.r)
             + (g - color.g) * (g - color.g)
             + (b - color.b) * (b - color.b);

        if (diff < best_diff)
        {
            best = i;
            best_diff = diff;
        }

        if (diff == 0)
        {
            break;
        }
    }

    return best;
}

void I_BeginRead (void)
{
}

void I_EndRead (void)
{
}

void I_SetWindowTitle (char *title)
{
	DG_SetWindowTitle(title);
}

void I_GraphicsCheckCommandLine (void)
{
}

void I_SetGrabMouseCallback (grabmouse_callback_t func)
{
}

void I_EnableLoadingDisk(void)
{
}

void I_BindVideoVariables (void)
{
}

void I_DisplayFPSDots (boolean dots_on)
{
}

void I_CheckIsScreensaver (void)
{
}
