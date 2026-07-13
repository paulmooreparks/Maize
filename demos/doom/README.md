# demos/doom: DOOM on Maize (port bring-up)

This directory hosts the Maize DOOM port. The engine itself is the
vendored `doomgeneric` submodule; every Maize-specific glue file lives here,
OUTSIDE the submodule, which is never edited.

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

To watch DOOM render and play it in a window, build a `maize` with the SDL
display backend (`cmake -DMAIZE_DISPLAY=ON ...`), build the interactive demo
image from `doom_main.c`, and run it with `--display` and YOUR OWN `doom1.wad`
directory mounted read-only. Nothing real-game is committed; you supply the WAD.

    scripts/cc-maize.sh --dev -D DOOMGENERIC_RESX=320 -D DOOMGENERIC_RESY=200 \
        -o demos/doom/doom.mzx \
        --sources demos/doom/doom.sources \
        demos/doom/doom_main.c demos/doom/doomgeneric_maize.c

    build/<preset>/maize --display --input=keyboard \
        --mount "<your-doom-wad-dir>=/wad:ro" \
        demos/doom/doom.mzx -iwad /wad/doom1.wad -warp 1 1

The window opens at the native 320x200 (the default framebuffer geometry the
platform layer is built against). Keyboard controls follow the Set-1 to DOOM
keymap in `doomgeneric_maize.c`: arrows to move, Ctrl to fire, Space to use,
Enter / Escape for menus. This demo is the visible payoff; it is not a CI gate.

On Windows under Git Bash / MSYS2, the shell rewrites a bare POSIX argument like
`/wad/doom1.wad` (or `/ro/min.wad`) into a Windows path before the native
`maize.exe` receives it, so DOOM cannot find the WAD at its guest mount point.
Those `-iwad` values are GUEST paths (resolved by maize's hostfs), not host
paths, so exempt them from the rewrite: prefix the command with
`MSYS2_ARG_CONV_EXCL='/wad'` (or `'/ro'` for the render gate). The `--mount` host
side needs no exemption (it is already a native `C:\...` path).

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
