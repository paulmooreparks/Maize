# demos/doom: DOOM on Maize (port bring-up)

This directory hosts the Maize DOOM port (maize-85). The engine itself is the
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

    scripts/cc-maize.sh --dev -o demos/doom/doom.mzx \
        --sources demos/doom/doom.sources demos/doom/doom_main.c

## Status: Phase A BLOCKED on toolchain gaps (dormant)

As of maize-145's first bring-up pass the tree does NOT yet compile or link. The
vendoring and build infra above are committed DORMANT: nothing in CI references
them (no `run_doom_link` is wired into `scripts/run-ctest.sh`), so master stays
green. The blocking gaps are recorded in the maize-145 card comments for the
orchestrator to file as spawned fix cards; the dominant ones are:

1. `cproc` rejects `__attribute__((packed))` on DOOM's on-disk WAD structs
   (47 core TUs). Front-end / driver gap.
2. `mazm` object mode does not support label operands in `ST` / data initializers
   (maize-12), which DOOM's action-function pointer tables (info.c states[], menus,
   etc.) require (~23 core TUs).
3. Missing freestanding headers (`strings.h`, `math.h`, `assert.h`, `unistd.h`,
   `sys/types.h`, `sys/stat.h`) and libc entry points (`strcasecmp`, `strncasecmp`,
   `sscanf`, `system`, `remove`, `mkdir`, `usleep`, `fabs`) plus header macros
   (`SEEK_SET`/`SEEK_CUR`/`SEEK_END`, `EISDIR`).

Once those land, re-run the build command above; on a produced `.mzx`, wire the
`run_doom_link` gate into `scripts/run-ctest.sh` per maize-145 section 7.
