# Third-Party Notices

Maize is licensed under Apache-2.0. It embeds, vendors, and borrows third-party
components; this file records attribution and license terms for that material. Build
provenance and version pins for the toolchain dependencies live in
[toolchain/VENDORING.md](toolchain/VENDORING.md); this file is the license/attribution
companion to it.

Three categories of third-party material are covered below:

- **Linked into the VM** (SDL2): shipped with the `maize` binaries.
- **Build toolchain** (cproc, qbe, llvm-mingw): builds Maize and the C toolchain but is
  not part of the VM binary; its runtime libraries may be statically linked into the
  binaries Maize ships.
- **Borrowed demo programs** (kilo, DOOM via doomgeneric): standalone guest programs
  under `demos/`, each its own binary, NOT part of or linked into the Maize VM.

## Linked into the VM

### SDL2 -- zlib license

The opt-in `--display` window backend (`MAIZE_DISPLAY=ON`, `src/devices.cpp`) links
SDL2 (https://libsdl.org, (c) Sam Lantinga and the SDL contributors), which is
licensed under the zlib license. On Linux/macOS SDL2 is the system package
(`libsdl2-dev`); on Windows it is the pinned `SDL2-devel-2.32.8-mingw` release, fetched
by `scripts/bootstrap-sdl2.ps1`. The graphical Maize binary redistributes the SDL2
runtime (`SDL2.dll`) alongside `maize.exe`; the zlib license permits this and does not
require the DLL be marked, though this notice records the attribution. The console
Maize binary does not link SDL2.

## Build toolchain (not part of the VM binary)

### cproc -- ISC license

The Maize C toolchain uses cproc (https://sr.ht/~mcf/cproc/, (c) 2019-2026 Michael
Forney), pinned as the `toolchain/cproc` submodule. ISC license (permissive, Apache-2.0
compatible). cproc is a build/host tool; it is not part of the Maize VM.

### qbe (and the in-repo Maize backend) -- MIT license

The C toolchain's code generator is qbe (https://c9x.me/compile/, (c) 2015-2019 Quentin
Carbonneaux), pinned as the `toolchain/qbe` submodule under the MIT license. The Maize
code-generation target lives in-repo at `toolchain/qbe-maize/` and is a derivative work
of qbe (overlaid onto the pristine qbe checkout at build time); it carries qbe's MIT
terms. qbe is a build/host tool; it is not part of the Maize VM.

### llvm-mingw -- build toolchain; statically linked runtime

The Windows Maize binaries are built with llvm-mingw
(https://github.com/mstorsjo/llvm-mingw), a downloaded build tool that is NOT committed
to this repo (fetched by `scripts/bootstrap-toolchain.ps1` into the gitignored
`.toolchains/`). The shipped Windows binaries statically link its runtime: the
mingw-w64 runtime (public domain / permissive per its own COPYING notices) and the LLVM
C++ runtime and builtins (libc++, libc++abi, compiler-rt), which are licensed under
Apache-2.0 with the LLVM exception. No attribution shipping obligation beyond preserving
those upstream license texts, which travel with the llvm-mingw distribution.

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

## Borrowed demo programs (separate binaries, not part of the VM)

These are standalone guest programs under `demos/`, each compiled to its own Maize
executable. They are NOT linked into or distributed as part of the Maize VM; their
licenses govern only their own demo binaries.

### demos/kilo -- kilo (Salvatore Sanfilippo) -- BSD-2-Clause
The kilo editor demo (`demos/kilo/kilo.c`) is Salvatore Sanfilippo's kilo
(https://github.com/antirez/kilo), (c) 2016 Salvatore Sanfilippo, BSD-2-Clause. The
full license text is preserved at `demos/kilo/LICENSE`.

### demos/doom -- DOOM via doomgeneric -- GPL-2.0
The DOOM demo is built from doomgeneric (https://github.com/ozkl/doomgeneric, pinned as
the `demos/doom/doomgeneric` submodule), which wraps the original id Software DOOM
source. That source is licensed under the **GNU General Public License, version 2**
(full text at `demos/doom/doomgeneric/LICENSE`), so the resulting DOOM demo binary
(`demos/doom/doom.mzx`) is a GPL-2.0 work.

**GPL boundary:** the Maize VM (Apache-2.0) does not incorporate, link, or derive from
any GPL code. DOOM is a guest program that runs ON Maize the same way it would run on
any machine; the GPL applies to the DOOM demo binary alone, not to the VM, the
toolchain, or any other Maize component. The Maize-specific platform shim
(`demos/doom/doomgeneric_maize.c`) is part of that GPL-2.0 demo work.
