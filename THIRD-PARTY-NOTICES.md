# Third-Party Notices

Maize is licensed under Apache-2.0. It embeds and vendors third-party components;
this file records attribution and license terms for material embedded in the
source tree. The full audit of every vendored/borrowed dependency (SDL2, cproc,
qbe, llvm-mingw, the borrowed kilo editor, DOOM/doomgeneric, and future borrowed
userland) is tracked on the board as maize-224.

## Embedded console fonts

### src/font8x8.h -- font8x8 (Daniel Hepper) -- Public Domain
Daniel Hepper's `font8x8` (https://github.com/dhepper/font8x8), derived from the
IBM PC BIOS font. Public domain. Embedded as the demos/terminal guest console font.

### src/font8x16.h -- IBM PS/2 Model 30 ROM font (8x16, CP437)
The Maize host VM text console (src/devices.cpp) uses the 8x16 IBM PS/2 Model 30
ROM font. The raw glyph bitmaps (ASCII block 0x20-0x7F) were extracted from the
`Bm437_IBM_Model30r0` `.FON` in VileR's "The Ultimate Oldschool PC Font Pack"
(https://int10h.org/oldschool-pc-fonts/) and converted to a C array.

Licensing: **only the raw bitmap glyph data is embedded, and that data is not
copyrightable.** VileR (the digitizer) states explicitly:

> "The raw bitmap typefaces are not copyrightable, unlike fonts in specific
> formats such as .fon and TrueType (which qualify as software) ... I do not
> claim any rights to the original raster binary data charsets."

The `.FON` file itself is VileR's software work, licensed CC BY-SA 4.0; Maize does
**not** redistribute the `.FON` (or any CC BY-SA material), it embeds only the
uncopyrightable raw bitmap. The original character set is (c) IBM.

Courtesy attribution (per VileR's request for credit): digitization by VileR,
https://int10h.org/oldschool-pc-fonts/ . An alternate source of the same raster
data: https://github.com/retro-vault/font-vault .
