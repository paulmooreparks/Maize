# Maize Carryover

Working state for picking up development. Positioning and milestone sequencing live in [ROADMAP.md](ROADMAP.md); the instruction set lives in [README.md](README.md).

## Where things stand

The M0.5 stabilization foundation is complete and on `origin/master`. Maize builds natively on Windows with no Visual Studio, is clang-clean and optimization-safe, has a hardened assembler, produces redirectable/pipeable output, ships a one-command test runner, and runs CI on every push.

Landed:
- Flag model with separate carry and overflow, per operand width; signed/unsigned branches wired.
- MUL corrected (was subtracting).
- Register/flag storage redesigned: single-word backing store with shift/mask sub-register proxies (no type-punning union). Compiles under clang and GCC; correct under optimization.
- Assembler (mazm): file:line diagnostics with nonzero exit and no stale binary, ctype UB fixed, UTF-8/BOM safe, duplicate-label corruption fixed.
- VM syscall I/O uses WriteFile/ReadFile, so stdout/stdin survive redirection and pipes.
- Portable toolchain: `scripts/bootstrap-toolchain.{ps1,sh}` fetches a pinned llvm-mingw (checksum-verified); CMake presets per platform; no MSVC dependency.
- Test runner: `scripts/run-tests.{ps1,sh}` builds both binaries and runs the suite, exit 0/1/2.
- CI: `.github/workflows/ci.yml`, Linux and Windows jobs, green, red-on-failure verified.

## Build and test

Prereqs: CMake 3.21+ and Ninja. On Windows the compiler is fetched by the bootstrap script; on Linux use system GCC/Clang.

    # Windows (from repo root, no Visual Studio needed)
    scripts\bootstrap-toolchain.ps1
    scripts\run-tests.ps1

    # Linux / WSL (needs ninja on PATH)
    scripts/run-tests.sh

Manual smoke test: `mazm asm/hello.asm` then `maize asm/hello.bin` prints "Hello, world!".

## Environment notes and gotchas

- This dev host has cmake 4.3.4 (installed at `C:\Program Files\CMake\bin`, not always on the shell PATH) and ninja 1.13.2. The runner locates cmake robustly.
- WSL Ubuntu-22.04 has cmake and g++ but no ninja and no passwordless sudo, so the `linux-debug` CMake preset is not runnable there; build Linux directly with `g++ -std=c++20 -o /tmp/maize src/maize.cpp src/cpu.cpp src/sys.cpp` (mazm links the same three plus mazm.cpp). CI covers the real Linux preset run.
- WSL exit-code artifact: chaining `cmd; echo $?` inside a single `wsl.exe bash -lc '...'` from Windows misreports the exit code as 0. Capture exit codes in the outer shell or a script file.
- `asm/hello.bin` is committed and is the byte-identical baseline (md5 `ad818f96bde3c15769f8350fc24d247c`). Any ISA-visible change must keep it identical unless the change is meant to alter output.
- No LICENSE file exists yet. A public repo with no license defaults to all-rights-reserved, which works against the "reimplement your own VM" goal; worth adding one (Apache-2.0 matches the sibling projects).

## What is next

The M0 semantic ISA cards are the next roadmap block, now cheap to build and verify via the runner and CI:
- Pointer width and segment model decision.
- Flags-on-load decision.
- Signed division and modulo.
- Add-with-carry / subtract-with-borrow.
- Wide multiply with high-half result.
- Branch complements (JGE, JLE, JBE, JAE).
- Immediate-math operand field: implement or delete.

Remaining stabilization ergonomics (non-blocking): string-output syscall, then a Maize assembly test library. Follow-ups: further assembler hardening (register validation, numeric-literal diagnostics, circular-INCLUDE, the mazm `-Wswitch` cleanup) and optimized/Release CMake presets.

Task-level detail, priorities, and dependencies live on the `maize` Andoneer workbench.
