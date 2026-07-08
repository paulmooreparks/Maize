# Maize QBE target (overlay onto the pinned qbe submodule)

This directory holds the Maize back-end target for QBE, tracked **in the Maize
repo** rather than inside the `toolchain/qbe` submodule. The submodule stays pinned
at its exact upstream commit (`4420727667b915042050b9bfa6eb381ce7a97ba5`, maize-61
decisions 6409/6610), preserving the auditable-pristine-upstream property; the build
overlays this target onto the checkout (maize-62 decision 6637).

## Files

| File | Purpose |
|------|---------|
| `all.h` | Maize register file (QBE-internal numbering) + target decls |
| `targ.c` | `Target T_maize` struct, register save tables, symbol sanitizer |
| `abi.c` | C ABI lowering: args in R0..R9, return in RV, calls, returns |
| `isel.c` | instruction selection (hello-world slice; CISC, cons pass through) |
| `emit.c` | mazm mnemonic emission + prologue/epilogue |
| `data.c` | data emission (labelled `DATA` byte lists) |
| `qbe-registration.patch` | minimal registration patch for qbe's `main.c` / `all.h` / `Makefile` |
| `CALLING-CONVENTION.md` | the final C calling convention (Deliverable 6) |
| `BACKEND-COVERAGE.md` | supported isel/emit ops + recorded idioms (Deliverable 6) |

## Overlay mechanism

`scripts/apply-maize-qbe-target.sh` (run by `scripts/build-toolchain.sh`) performs,
idempotently:

1. copies `all.h targ.c abi.c isel.c emit.c data.c` into `toolchain/qbe/maize/`;
2. applies `qbe-registration.patch` to the submodule with `git apply` (adds the
   target table entry, the `-t maize` dispatch, an `emitdat` data-emitter hook, and
   the Makefile object list). A reverse-apply check makes re-runs a no-op.

The registration patch is a small, reviewable diff against the pinned commit, so it
applies deterministically on a fresh `git submodule update --init` checkout on both
the Linux (gcc + make) and Windows (MSYS2) build paths.

## Fallback (decision 6637)

If the overlay/patch ever proves fragile on a platform, the documented fallback is to
repoint the `toolchain/qbe` submodule at a Maize-controlled qbe fork that carries the
target directly, recording the deviation as a decision (mirrors maize-61's
submodule-vs-in-tree fallback shape).

## Pipeline

See `scripts/run-ctest.sh` for the end-to-end hello-world run
(`cproc-qbe -> qbe -t maize -> prepend runtime -> mazm -> maize -> stdout diff`).
