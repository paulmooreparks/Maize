# Userland patch overlays (maize-94)

Numbered, per-project overlay patches applied to a scratch checkout of the pristine
submodule at build time by `userland/build-userland.sh` (never edited inline in the
submodule). One concern per patch; each patch's header comment states which upstream
file/function it touches and why (the kilo "Maize-local patch" marker discipline, at
patch-file granularity). Applied in filename order (`0001-*.patch`, `0002-*.patch`, ...)
with `patch -p1 --forward`.

- `oksh/` -- oksh overlay (config.h replacement for autoconf, cproc-C11 fixes, aggregate
  by-value argument/return rewrites per decision 8946).
- `sbase/` -- sbase overlay (util.h / libutil cproc-C11 fixes as they surface).

Maize-local shim headers the borrowed source needs but the freestanding libc slice does
not yet ship live in `userland/include/` and are copied into the scratch checkout so
angle-bracket includes resolve (e.g. the minimal `regex.h` sbase's `util.h` pulls in).
