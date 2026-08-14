# Maize Carryover

Working state for picking up development. Positioning and milestone sequencing live in [ROADMAP.md](ROADMAP.md); the instruction set lives in [README.md](README.md).

## Where things stand

Maize moved to a second instruction set architecture, v2, on 2026-08-12; ROADMAP.md's Phase 3 is the current campaign, and this file's "Landed" section below still describes v1, which is frozen and preserved on the `v1` branch. v2's specification is ratified and frozen at base `2.0` (`docs/spec-v2/`), and the VM, the assembler, and the core machine (privilege levels, the trap model, Sv48 paging, external interrupts) are on `dev`, with the rest of the toolchain, quesOS, and the conformance suite still queued (ROADMAP.md Milestones V3 through V5). The paragraph and bullets below predate that move and describe v1's toolchain as it stood before the freeze; a full pass bringing this section itself up to v2 is filed as maize-472.

Milestone 0 (ISA repairs), Milestone 0.5 (stabilization), and the bulk of Milestone 1 (C toolchain) are complete on the `v1` branch. That toolchain runs end to end there: C11 source compiles through mzcc, links against the freestanding runtime, and runs on the v1 VM with a working heap, variadic printf, and real errno reporting. CI ran the asm corpus and the C corpus on Linux and Windows, plus a sanitizer leg, as a nightly batch and on demand rather than on every push (maize-318), while v1 was still under development.

Landed:
- ISA: separate carry/overflow flags per operand width; signed and unsigned div/mod; ADC/SBB; MULW/UMULW; the full branch-complement set; the SETcc family with C-friendly synonyms; SAR; NEG; flat-64 pointer model; guaranteed process-start register/stack contract with a System V-style argc/argv/envp block.
- Formats and tools: flat `.mzb` images, relocatable `.mzo` objects (SECTION/GLOBAL/PUBLIC/EXTERN/ZERO/DREF/ALIGN, maize-71/maize-89), linked `.mzx` executables (segmented model, maize-77). mazm (file:line diagnostics, `--check`/`--stdin` editor modes, `--help`), mzld (hygiene pass: W^X, overlap, fit checks), mzdis (byte-exact flat round-trip with synthesized fn_/loc_ labels, maize-70). maize loads both image formats and registers as an OS-level handler (binfmt_misc on Linux, file associations on Windows).
- C toolchain: vendored cproc/qbe submodules with a Maize QBE target. `mzcc` is now a compiled binary (maize-278 chain) that drives the whole cpp -> cproc-qbe -> qbe -> mazm -> mzld pipeline itself, with a content-addressed object cache and a parallel TU scheduler (maize-274); build-world stages 3-5 call `mzcc build-quesos`/`build-userland`/`build-demos` directly (maize-291). The legacy `scripts/cc-maize.sh` shell driver still exists as the parity baseline and is what CI's run-ctest.sh defaults to (MAIZE_CC selector); maize-281 flips CI to the compiled mzcc and deletes cc-maize.sh. mzcc has a gcc-like CLI (compile-to-`.mzx` default, `-r` run, `--emit`, `-o`, `--build`; maize-111). C ABI: six argument registers R0..R5 plus stack overflow and varargs per maize-98 ([toolchain/qbe-maize/CALLING-CONVENTION.md](toolchain/qbe-maize/CALLING-CONVENTION.md)). Runtime: crt0, string/ctype/stdio/stdlib slice with a brk-backed heap (maize-76), syscalls read/write/exit/brk with `-errno` results (maize-75, [toolchain/rt/SYSCALL-ABI.md](toolchain/rt/SYSCALL-ABI.md)).
- Infrastructure: pinned llvm-mingw bootstrap (no MSVC), CMake presets per platform, `scripts/install-mzasm.{ps1,sh}` builds and installs mzvm/mzvmg/mzasm to `~/bin`, and the default Ctrl+Shift+B task runs it on every press (maize-454). The v1 binaries are no longer built or installed; the v1 C pipeline (mzcc plus the cproc/qbe cross-toolchain) is opt-in behind `-WithCToolchain` / `--with-c-toolchain`, which build-world passes. mzcc and the whole guest C toolchain build and run natively on Windows, no WSL and no MSYS2 required (maize-257); the only build step that still needs a POSIX shell (Git Bash) is compiling the vendored cproc/QBE via build-toolchain.sh (`mzcc --build`).

## Build and test

Prereqs: CMake 3.21+ and Ninja. On Windows the compiler is fetched by the bootstrap script; on Linux use system GCC/Clang.

    # Windows (from repo root, no Visual Studio needed)
    scripts\bootstrap-toolchain.ps1
    scripts\install-mzasm.ps1
    ctest --test-dir build\windows-llvm-mingw-release

    # Linux / WSL (needs ninja on PATH)
    cmake --preset linux-release
    cmake --build --preset linux-release
    ctest --test-dir build/linux-release

`ctest` runs the whole v2 suite (`cmake/MaizeV2Fixtures.cmake`): the interpreter's own fixtures and the mzasm assembler's, one CTest entry each, all labeled `v2`. `scripts/run-tests.{ps1,sh}` still build and drive `maize.exe`, a v1 binary this branch no longer builds, and `scripts/run-ctest.sh` still drives the v1 mzcc pipeline; both are stale until repointed or retired (maize-473). Manual smoke test: `mzasm asm/v2/hello.mzasm` then `mzvm asm/v2/hello.mzi` prints "hello, maize".

## Environment notes and gotchas

- This dev host has cmake (installed at `C:\Program Files\CMake\bin`, not always on the shell PATH) and ninja. The runners locate cmake robustly.
- The default WSL distro is Ubuntu-24.04 with cmake, ninja, and g++ preinstalled, so the `linux-debug` preset loop runs unmodified: `wsl.exe bash -lc 'cd /mnt/c/Users/paul/source/repos/Maize && bash scripts/run-tests.sh'`.
- WSL exit-code artifact: chaining `cmd; echo $?` inside a single `wsl.exe bash -lc '...'` from Windows misreports the exit code as 0. Capture exit codes in the outer shell or a script file.
- `asm/hello.mzb` is committed and is the byte-identical baseline (md5 `067d225eb695b8efcbb752190a657fdc`). Any ISA-visible change must keep it identical unless the change is meant to alter output. (Renamed from hello.bin by maize-65, byte-neutral. Rebaselined for maize-64, the opcode-map consolidation: JZ/CLR/POP re-encoded so hello's bytes change; program output is unchanged. Prior baselines: `9633f915dc75786f693b53d1a228f4c6` (maize-41 flat-64), `04e09a107df2577cbeee3e53ce8b64a5` (maize-4), `ad818f96bde3c15769f8350fc24d247c` (original).)

## What is next

Development continues on v2, not on the items below, which were v1's own backlog before the freeze and are not being pursued.

- The v1 Milestone 1 tail: Unicode source files in the assembler.
- The v1 cycle cost model.
- Non-blocking v1 ergonomics: the mazm `-Wswitch` cleanup and optimized/Release CMake presets.

For what is actually next, see ROADMAP.md's Phase 3 (Milestones V3 through V5) and the `maize` Andoneer workbench, which carries task-level detail, priorities, and dependencies.
