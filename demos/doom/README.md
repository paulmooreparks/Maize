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
- `testdata/doomread.bin`: a committed 512-byte binary fixture the self-check reads
  through the libc `FILE*` path (the same path DOOM uses to open a WAD).
- `doom_main.c`: the entry TU; `main` calls `doomgeneric_Create` then loops
  `doomgeneric_Tick`.

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

## Status: Phase B, real DG platform + WAD-read plumbing, unit-verified

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

The full DOOM boot, the first-level render from a real IWAD, operator IWAD
provisioning, and the windowed SDL demo are the next phase.
