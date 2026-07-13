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
- `doomgeneric_maize.c`: the Maize platform layer, Phase A STUB bodies for the
  six `DG_*` functions plus the two residual sound-module descriptor structs.
- `doom_main.c`: the entry TU; `main` calls `doomgeneric_Create` then loops
  `doomgeneric_Tick`.

## Build command (Phase A target: compile + link only)

The entry TU and the stub platform layer are passed positionally alongside the
entry-free `doom.sources` core set (which names no `main`, so Phase C reuses it
verbatim):

    scripts/cc-maize.sh --dev -o demos/doom/doom.mzx \
        --sources demos/doom/doom.sources \
        demos/doom/doom_main.c demos/doom/doomgeneric_maize.c

## Status: Phase A links

All 83 translation units (the `doom.sources` core set plus `doomgeneric_maize.c`
and `doom_main.c`) compile through the real `cc-maize.sh` pipeline, and the image
LINKS to a ~674 KB `.mzx` with zero unresolved symbols. The `run_doom_link` gate
in `scripts/run-ctest.sh` builds this `.mzx` on both hosts and asserts it is
produced (build-only; it does NOT run maize, that is Phase C). The gate skips
with a notice when the `demos/doom/doomgeneric` submodule is not initialized, so
a partial checkout does not hard-fail the ctest suite.

The build relies on `cproc` stripping GNU attributes in the driver (DOOM's
packed WAD structs), `mazm` object mode carrying label operands in `ST` and data
initializers (the action-function pointer tables), and the RT header/libc set
covering the `strings/math/assert/unistd/sys` headers plus
`strcasecmp`/`strncasecmp`/`fabs`/`sscanf`/`system`/`remove`/`mkdir`/`usleep`/`rename`
and the `SEEK_*`/`EISDIR` macros.

Real `DG_*` platform behaviour (framebuffer present, ms clock, the Set-1 to DOOM
key table, WAD loading) is Phase B; the headless render self-check and the
operator SDL demo are Phase C.
