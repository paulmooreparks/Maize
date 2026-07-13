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

The vendoring and build infra above are committed DORMANT: nothing in CI
references them (no `run_doom_link` is wired into `scripts/run-ctest.sh`), so
master stays green until the tree links end to end.

The first bring-up pass's dominant blockers are all resolved: `cproc` now strips
GNU attributes in the driver (maize-149, unblocks the packed WAD structs), `mazm`
object mode carries label operands in `ST` and data initializers (maize-150,
unblocks the action-function pointer tables), and the RT header/libc set gained
`strings/math/assert/unistd/sys` headers plus `strcasecmp`/`strncasecmp`/`fabs`/
`sscanf`/`system`/`remove`/`mkdir`/`usleep` and the `SEEK_*`/`EISDIR` macros
(maize-147, maize-148).

With those in, 83 of 83 translation units (the `doom.sources` set plus
`doomgeneric_maize.c` and `doom_main.c`) compile through the real pipeline, and
the image LINKS to a ~674 KB `.mzx`. One libc gap remains before the link is
green on master: `g_game.c` calls `rename(2)` (save-game commit path) and the RT
has no `rename`, which cproc rejects as an undeclared identifier. It is a sibling
of the existing `remove`/`mkdir` link-only stubs (maize-148) and belongs to the
libc-gaps workstream, not Phase A's diff.

Once `rename` lands, re-run the build command above; on a produced `.mzx`, wire
the `run_doom_link` gate into `scripts/run-ctest.sh` per maize-145 section 7.
