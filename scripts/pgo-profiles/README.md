# scripts/pgo-profiles: committed Clang PGO profiles (maize-259)

Each subdirectory here is named after the CMake preset it profiles and holds one
`default.profdata`: the merged, IR-based Clang profile `install-mazm.ps1` feeds to
`-DMAIZE_PGO=use` by default. This closes the ~26-28% Windows clang-vs-gcc
interpreter gap (see CMakeLists.txt's `MAIZE_PGO` option): a fresh clone gets the
profile-guided binary with no training step, because the profile ships in the repo
instead of being regenerated on every install.

This is a build input, not an interface: it is internally replaceable (regenerate
it any time with `scripts/build-pgo.ps1`) and nothing outside the build depends on
its exact bytes, so it carries none of the staleness risk a checked-in generated
*source* file would (DIRT: debt is what escapes into interfaces, not an
internally-replaceable implementation choice).

## windows-llvm-mingw-release/default.profdata

Trained 2026-07-19 against `demos/doom/doom_bench.c` (the headless DOOM
benchmark; BENCH_FRAMES=120, `-warp 1 1 -nomonsters` against the synthetic
`min.wad`), 5 back-to-back runs merged, on llvm-mingw 20260616 (clang 22.1.8).
Result: clang baseline ~18700-19300 us/frame -> clang+PGO 14333 us/frame
(matches the gcc/Linux reference of 14330 us/frame from the same workload;
see maize-259's A/B table).

## When to retrain

A stale profile does not break the build: Clang tolerates a profile whose
function hashes no longer match the current source (`-Wno-profile-instr-out-
of-date` / `-Wno-profile-instr-unprofiled` on the `MAIZE_PGO=use` path silence
the warning) and simply skips profile-guided optimization for any function
whose hash no longer matches, falling back to ordinary -O2/-O3 codegen for
just that function. Retrain (regenerate this file with `scripts/build-pgo.ps1`
and commit the result) when either:

- the interpreter hot path changes meaningfully (`src/cpu.cpp` and anything it
  calls into: `src/sys.cpp`, `src/devices.cpp`), or
- the vendored llvm-mingw pin bumps (`scripts/bootstrap-toolchain.ps1`'s
  `$Version`), since profile format / function hashing can shift across major
  Clang versions.

See `scripts/build-pgo.ps1` for the regeneration recipe.
