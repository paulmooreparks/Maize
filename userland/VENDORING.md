# Userland vendoring (maize-94)

Borrowed userland ported to Maize by recompilation, not reimplementation (the maize-90
"steal, don't build" doctrine). Each project is a git submodule pinned at an exact
upstream commit, kept pristine; every Maize-local source change lives in a numbered
`patches/<project>/` overlay applied to a scratch checkout at build time (never edited
inline in the submodule), mirroring `toolchain/qbe-maize`'s overlay-onto-a-pristine-
submodule pattern. This keeps the submodule diff empty and re-pinnable and the Maize
deltas reviewable in isolation.

## Pin table

| Tool  | Path             | Submodule URL                             | Pinned commit | Upstream tag | License |
|-------|------------------|-------------------------------------------|---------------|--------------|---------|
| oksh  | `userland/oksh`  | https://github.com/ibara/oksh             | `15f69e4`     | oksh-7.9     | ISC / public-domain (OpenBSD ksh lineage) |
| sbase | `userland/sbase` | https://github.com/michaelforney/sbase    | `b30fb56`     | (master)     | MIT/X (suckless) |

sbase is michaelforney's fork, matching the same author's cproc/qbe mirrors already
trusted in this repo (`toolchain/VENDORING.md`).

## Layout

- `userland/oksh/`, `userland/sbase/` -- pristine pinned submodules.
- `userland/patches/oksh/`, `userland/patches/sbase/` -- numbered overlay patches, one
  concern per file; each patch's header states which upstream file/function it touches and
  why (kilo's "Maize-local patch" marker discipline, at patch-file granularity).
- `userland/include/` -- Maize-local headers the borrowed source needs that the
  freestanding libc slice does not yet ship (e.g. a minimal `regex.h` shim: sbase's
  `util.h` includes it unconditionally though only grep-family utils actually use regex,
  out of scope for wave 1).
- `userland/<project>/.../sources.list` -- `--sources` listfiles for
  `scripts/cc-maize.sh`'s multi-source mode, enumerating the post-patch `.c` set per
  program. No new pipeline code: `cc-maize.sh` already drives the full cpp -> cproc-qbe ->
  qbe -t maize -> mazm -c -> mzld chain against `toolchain/rt`.
- `userland/oksh/config.h` (overlaid) -- a hand-authored replacement for oksh's autoconf
  output, with the `HAVE_*` feature macros set to match what Maize's libc provides today
  (no job control, no wide chars, no locale, no vfork, no mmap), so the compiled feature
  set matches the wave-1 floor rather than silently drifting.

## Wave-1 scope

sbase utilities (operator-confirmed, OQ 8949): `ls` (plain names), `cat`, `cp`, `mv`,
`rm`, `echo`, `printf`, `pwd`, `ed`, `true`, `false`. oksh: interactive raw-mode prompt,
external commands via PATH, pipelines, redirection (`<`, `>`, `>>`), builtins
(`cd`, `exit`, `export`, `pwd`), `$?`. Binaries keep the `.mzx` extension (OQ 8950) and
live at `/bin/<name>.mzx` inside the `--mount` grant (decision 8939).

## Building

`userland/build-userland.sh` stages a scratch checkout of each submodule, applies the
`patches/<project>/` series in order, then compiles each program through
`scripts/cc-maize.sh` using its `sources.list`. The output `.mzx` binaries are the
wave-1 `/bin` set. Aggregate (struct) by-value argument/return sites the qbe-maize
backend cannot lower are patched to pass/return via pointer in the overlay (decision
8946), never by extending the backend.
