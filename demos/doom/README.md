# demos/doom: DOOM on Maize (port bring-up)

This directory hosts the Maize DOOM port. The engine itself is the
vendored `doomgeneric` submodule; every Maize-specific glue file lives here,
OUTSIDE the submodule, which is never edited.

## Build and run it yourself

This is the whole path from a fresh clone to DOOM in a window. It splits into two
independent halves:

- The DOOM image `demos/doom/doom.mzx` is **portable Maize bytecode**. It is
  already committed, so if you only want to *play* you can skip to step 3.
- Running it in a window needs a **display-enabled `maize` built for your OS**.

You supply your own WAD. The freely-redistributable shareware `doom1.wad` works,
as do retail `DOOM.WAD` / `DOOM2.WAD`. No game asset is committed to this repo.

### 1. Build a display-enabled `maize`

The SDL2 window backend is opt-in (`-DMAIZE_DISPLAY=ON`). This is all you need to
*run* the committed image. From the repo root (see the top-level `README.md`,
"Build a display-enabled `maize`", for other platforms):

Windows (PowerShell). SDL2 is bundled under `.toolchains/`, nothing to install:

    cmake --preset windows-llvm-mingw-release -DMAIZE_DISPLAY=ON
    cmake --build --preset windows-llvm-mingw-release

Linux (bash):

    sudo apt-get install -y cmake ninja-build libsdl2-dev
    cmake --preset linux-release -DMAIZE_DISPLAY=ON
    cmake --build --preset linux-release

The tools land in `build/<preset>/` (`maize`, plus `mazm`/`mzld`/`mzdis`).

### 2. (optional) Rebuild the DOOM image from source

Skip this to use the committed `demos/doom/doom.mzx`. To rebuild it you also need
the C cross-compiler (vendored cproc + QBE, which lower DOOM's C to Maize code)
and the engine submodule. cproc/QBE are **POSIX-only**, so this half runs under
Linux, macOS, WSL, or MSYS2 (never native Windows) even though the resulting
bytecode then runs on the native `maize.exe`:

    git submodule update --init demos/doom/doomgeneric   # pinned engine source
    scripts/build-toolchain.sh                           # cproc + QBE + Maize QBE target

    scripts/cc-maize.sh --preset linux-release --dev \
        -D DOOMGENERIC_RESX=320 -D DOOMGENERIC_RESY=200 \
        -o demos/doom/doom.mzx \
        --sources demos/doom/doom.sources \
        demos/doom/doom_main.c demos/doom/doomgeneric_maize.c

`cc-maize.sh` compiles all ~83 DOOM translation units through cproc -> QBE ->
mazm and links them with the Maize C runtime into `demos/doom/doom.mzx` (about
675 KB of Maize bytecode). `--dev` adds the device-access shim the
framebuffer/keyboard platform layer needs; the `-D` flags pin the frame to a
native 320x200 (see "Geometry" below). `--preset` picks which `build/<preset>/`
`mazm`/`mzld` to drive, so build those tools for the same POSIX preset first
(e.g. via `cmake --preset linux-release && cmake --build --preset linux-release`,
or `scripts/run-tests.sh`). The image is portable, so you can build it under WSL
and run it with the native Windows `maize.exe` from step 1.

### 3. Run it in a window

Point the display-enabled `maize` at a directory holding your WAD and launch:

    build/<preset>/maize --display --input=keyboard \
        --mount "<your-wad-dir>=/doom:ro" \
        demos/doom/doom.mzx -iwad /doom/doom1.wad

`--mount HOST=/doom:ro` gives the guest a read-only view of your WAD directory at
the guest path `/doom`; `-iwad /doom/doom1.wad` is what DOOM opens. Replace
`<preset>` and the mount path with your own. Handy extras: `-warp 1 1` jumps to
E1M1, `-nomonsters` quiets the level, and `--show-perf` overlays guest MIPS + FPS
(and prints the peaks on exit).

Windows / Git Bash note: the shell rewrites a bare `/doom/...` argument into a
Windows path before `maize.exe` sees it. `-iwad` is a GUEST path (resolved by
maize's sandbox), so exempt it: prefix the command with
`MSYS2_ARG_CONV_EXCL='/doom'`. The `--mount` host side is a real `C:\...` path and
needs no exemption.

Simpler, persistent alternative: maize's default sandbox root is `~/.maize/root`
(mounted as `/`, startup cwd `/home/user`). Drop your WAD once at
`~/.maize/root/home/user/doom/doom1.wad`, then no `--mount` is needed:

    build/<preset>/maize --display demos/doom/doom.mzx -iwad doom/doom1.wad

You can also make `--display` and `--input=keyboard` the defaults by putting
`display=true` and `input=keyboard` in `~/.maize/config` (one `key=value` per
line); a CLI flag always overrides the file.

### Controls

Arrows move, Ctrl fires, Space uses, Alt strafes, Enter/Escape drive the menus,
the number keys select weapons: the Set-1 to DOOM keymap in
`doomgeneric_maize.c`.

## Layout

- `doomgeneric/`: pinned git submodule, `github.com/ozkl/doomgeneric`, commit
  `dcb7a8dbc7a16ce3dda29382ac9aae9d77d21284` ("boolean fix"). PRISTINE; do not edit.
  Fetch it with `git submodule update --init demos/doom/doomgeneric`.
- `doom.sources`: the `--sources` listfile, the platform-agnostic
  doomgeneric + DOOM `.c` set for a headless build (SDL Makefile's `SRC_DOOM`
  minus the SDL platform + SDL-audio TUs). Names no entry `main` TU so Phase C
  can reuse it verbatim.
- `doomgeneric_maize.c`: the Maize platform layer, the real Phase B bodies for the
  six `DG_*` functions (framebuffer present, ms clock, Set-1 to DOOM key table)
  plus the two residual sound-module descriptor structs.
- `doom_selfcheck.c`: a standalone headless self-check that links ONLY the platform
  layer (not the full engine) and verifies each `DG_*` primitive in isolation.
- `doom_render_selfcheck.c`: the headless RENDER gate. It boots the WHOLE engine
  against the minimal synthetic IWAD, ticks until the first level renders, and
  asserts the 3D viewport is a real (non-blank) render.
- `tools/make_min_iwad.c`: a generator for a minimal, license-clean synthetic
  IWAD that boots DOOM to a one-room `E1M1`. Every lump byte is computed in the
  program; no real-game asset is copied. Compiled and run at test time by the
  render gate; the committed artifact is the source, never a WAD binary.
- `testdata/doomread.bin`: a committed 512-byte binary fixture the self-check reads
  through the libc `FILE*` path (the same path DOOM uses to open a WAD).
- `doom_main.c`: the entry TU; `main` calls `doomgeneric_Create` then loops
  `doomgeneric_Tick`. This is the interactive image for the operator SDL demo.

## Geometry

The engine defaults to 640x400 (`doomgeneric.h`), which would render DOOM's native
320x200 frame scaled 2x. Both Maize builds instead pass
`-D DOOMGENERIC_RESX=320 -D DOOMGENERIC_RESY=200` (a `cc-maize.sh` cpp-define
passthrough) so the frame is native 320x200 with no scaling, matching the default
320x200 XRGB8888 Maize framebuffer exactly. The frame present is then a straight
memcpy with no pixel conversion.

## Build command (compile + link the full tree)

The entry TU and the platform layer are passed positionally alongside the
entry-free `doom.sources` core set (which names no `main`, so the render phase
reuses it verbatim):

    scripts/cc-maize.sh --dev -D DOOMGENERIC_RESX=320 -D DOOMGENERIC_RESY=200 \
        -o demos/doom/doom.mzx \
        --sources demos/doom/doom.sources \
        demos/doom/doom_main.c demos/doom/doomgeneric_maize.c

## Run the headless self-check

The self-check needs the binary fixture mounted read-only at `/ro` and a Set-1
scancode sequence piped in on stdin. It prints `doom: PASS` on success:

    scripts/cc-maize.sh --dev -D DOOMGENERIC_RESX=320 -D DOOMGENERIC_RESY=200 \
        -o /tmp/doom_selfcheck.mzx \
        demos/doom/doom_selfcheck.c demos/doom/doomgeneric_maize.c

    printf '\036\236\110\113\115\120\035\071\034\001\017' \
        | build/<preset>/maize --input=keyboard \
            --mount "demos/doom/testdata=/ro:ro" /tmp/doom_selfcheck.mzx

`scripts/run-ctest.sh` wires this up as `run_doom_selfcheck` on both hosts, next
to the `run_doom_link` build gate.

## The minimal synthetic IWAD

DOOM refuses to boot without a structurally valid IWAD: it checks a large set of
required lumps (palette, colormap, textures, flats, the status-bar and font
graphics, and a map). Committing the ~4 MB real `doom1.wad` is neither legal nor
necessary, so `tools/make_min_iwad.c` computes a tiny (~32 KB) synthetic IWAD
from scratch: a full 256-color palette, an identity colormap, one shared 8x8
patch reused across the wall texture and every font / status-bar lump, two flats,
the shareware switch textures, a dummy level-music lump, and a single convex
`E1M1` room with a player start. Every byte is generated; nothing is copied from
any real WAD. Build and run it directly with:

    cc -O2 -o make_min_iwad demos/doom/tools/make_min_iwad.c
    ./make_min_iwad min.wad

## Run the headless render gate

The render gate boots the full engine against that IWAD and checks a real 3D
frame. Build the render image, generate the IWAD into a directory, and run it
with that directory mounted read-only at `/ro`; it prints `doom: PASS`:

    scripts/cc-maize.sh --dev -D DOOMGENERIC_RESX=320 -D DOOMGENERIC_RESY=200 \
        -o /tmp/doom_render.mzx \
        --sources demos/doom/doom.sources \
        demos/doom/doom_render_selfcheck.c demos/doom/doomgeneric_maize.c

    build/<preset>/maize --mount "<wad-dir>=/ro:ro" /tmp/doom_render.mzx \
        -iwad /ro/min.wad -warp 1 1 -nomonsters

`scripts/run-ctest.sh` wires this up as `run_doom_render` on both hosts (it
compiles the generator with the system `cc`, produces the IWAD, and runs the
gate), distinct from the platform-only `run_doom_selfcheck`.

## Operator SDL demo (play a rendered first level)

Watching DOOM render and playing it in a window on your own `doom1.wad` is the
visible payoff (it is not a CI gate). The full walkthrough is "Build and run it
yourself" at the top of this file: build a display-enabled `maize`, build (or use
the committed) `doom.mzx`, and run it with `--display` and your WAD mounted
read-only. The window opens at the native 320x200, the default framebuffer
geometry the platform layer is built against.

## Status: Phase C, DOOM boots and renders a first level

The full engine boots against the minimal synthetic IWAD and renders a real 3D
first level, headless-checked in CI by `run_doom_render` and visible via the
operator SDL demo on a real `doom1.wad`. The platform layer below is unchanged
from Phase B.

## Platform layer (Phase B): real DG platform + WAD-read plumbing

The six `DG_*` platform functions are implemented for real over the frozen Maize
device / clock / libc surfaces:

- `DG_Init` registers a 320x200 XRGB8888 present buffer in guest RAM after a
  geometry guard (framebuffer must be exactly 320x200, format XRGB8888).
- `DG_DrawFrame` copies the rendered frame into that buffer and presents it.
- `DG_GetTicksMs` / `DG_SleepMs` read and spin on the monotonic ms clock.
- `DG_GetKey` translates Set-1 make/break scancodes to DOOM key codes (arrows,
  ctrl = fire, space = use, enter, escape, tab, shift/alt modifiers, and the
  alphanumerics used by the menus).
- `DG_SetWindowTitle` stores the title (Maize has no window manager).

WAD file access needs no new hook: the engine reaches a WAD through the libc
`fopen`/`fread`/`fseek`/`ftell`/`fclose` path, which the self-check verifies on
the committed binary fixture. Each primitive is checked in isolation by
`doom_selfcheck.c` through the real `cc-maize.sh` pipeline, and the full tree
still links (`run_doom_link`).
