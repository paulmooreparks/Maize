# Maize Carryover

Working state for picking up development. Positioning and milestone sequencing live in [ROADMAP.md](ROADMAP.md); the instruction set lives in [README.md](README.md).

## Where things stand

Milestone 0 (ISA repairs), Milestone 0.5 (stabilization), and the bulk of Milestone 1 (C toolchain) are complete and on `origin/master`. The toolchain runs end to end: C11 source compiles through mzcc, links against the freestanding runtime, and runs on the VM with a working heap, variadic printf, and real errno reporting. CI runs the asm corpus and the C corpus on every push, Linux and Windows, plus a sanitizer leg.

Landed:
- ISA: separate carry/overflow flags per operand width; signed and unsigned div/mod; ADC/SBB; MULW/UMULW; the full branch-complement set; the SETcc family with C-friendly synonyms; SAR; NEG; flat-64 pointer model; guaranteed process-start register/stack contract with a System V-style argc/argv/envp block.
- Formats and tools: flat `.mzb` images, relocatable `.mzo` objects (SECTION/GLOBAL/PUBLIC/EXTERN/ZERO/DREF/ALIGN, maize-71/maize-89), linked `.mzx` executables (segmented model, maize-77). mazm (file:line diagnostics, `--check`/`--stdin` editor modes, `--help`), mzld (hygiene pass: W^X, overlap, fit checks), mzdis (byte-exact flat round-trip with synthesized fn_/loc_ labels, maize-70). maize loads both image formats and registers as an OS-level handler (binfmt_misc on Linux, file associations on Windows).
- C toolchain: vendored cproc/qbe submodules with a Maize QBE target. `mzcc` is now a compiled binary (maize-278 chain) that drives the whole cpp -> cproc-qbe -> qbe -> mazm -> mzld pipeline itself, with a content-addressed object cache and a parallel TU scheduler (maize-274); build-world stages 3-5 call `mzcc build-quesos`/`build-userland`/`build-demos` directly (maize-291). The legacy `scripts/cc-maize.sh` shell driver still exists as the parity baseline and is what CI's run-ctest.sh defaults to (MAIZE_CC selector); maize-281 flips CI to the compiled mzcc and deletes cc-maize.sh. mzcc has a gcc-like CLI (compile-to-`.mzx` default, `-r` run, `--emit`, `-o`, `--build`; maize-111). C ABI: six argument registers R0..R5 plus stack overflow and varargs per maize-98 ([toolchain/qbe-maize/CALLING-CONVENTION.md](toolchain/qbe-maize/CALLING-CONVENTION.md)). Runtime: crt0, string/ctype/stdio/stdlib slice with a brk-backed heap (maize-76), syscalls read/write/exit/brk with `-errno` results (maize-75, [toolchain/rt/SYSCALL-ABI.md](toolchain/rt/SYSCALL-ABI.md)).
- Infrastructure: pinned llvm-mingw bootstrap (no MSVC), CMake presets per platform, `scripts/install-mazm.{ps1,sh}` builds and installs maize/maizeg/mazm/mzld/mzdis/mzcc to `~/bin`. mzcc and the whole guest C toolchain build and run natively on Windows, no WSL and no MSYS2 required (maize-257); the only build step that still needs a POSIX shell (Git Bash) is compiling the vendored cproc/QBE via build-toolchain.sh (`mzcc --build`).

## Build and test

Prereqs: CMake 3.21+ and Ninja. On Windows the compiler is fetched by the bootstrap script; on Linux use system GCC/Clang. The C toolchain additionally needs the submodules (`git submodule update --init --recursive`).

    # Windows (from repo root, no Visual Studio needed)
    scripts\bootstrap-toolchain.ps1
    scripts\run-tests.ps1

    # Linux / WSL (needs ninja on PATH)
    scripts/run-tests.sh

`run-tests.{ps1,sh}` builds the four tools (maize, mazm, mzld, mzdis) and runs the asm/ corpus, exit 0/1/2. `scripts/run-ctest.sh` compiles and runs the ctest/ C corpus through the full mzcc pipeline and diffs each program's output (and exit status, where asserted) against its committed fixture. Manual smoke test: `mazm asm/hello.mazm` then `maize asm/hello.mzb` prints "Hello, world!".

## Environment notes and gotchas

- This dev host has cmake (installed at `C:\Program Files\CMake\bin`, not always on the shell PATH) and ninja. The runners locate cmake robustly.
- The default WSL distro is Ubuntu-24.04 with cmake, ninja, and g++ preinstalled, so the `linux-debug` preset loop runs unmodified: `wsl.exe bash -lc 'cd /mnt/c/Users/paul/source/repos/Maize && bash scripts/run-tests.sh'`.
- WSL exit-code artifact: chaining `cmd; echo $?` inside a single `wsl.exe bash -lc '...'` from Windows misreports the exit code as 0. Capture exit codes in the outer shell or a script file.
- `asm/hello.mzb` is committed and is the byte-identical baseline (md5 `067d225eb695b8efcbb752190a657fdc`). Any ISA-visible change must keep it identical unless the change is meant to alter output. (Renamed from hello.bin by maize-65, byte-neutral. Rebaselined for maize-64, the opcode-map consolidation: JZ/CLR/POP re-encoded so hello's bytes change; program output is unchanged. Prior baselines: `9633f915dc75786f693b53d1a228f4c6` (maize-41 flat-64), `04e09a107df2577cbeee3e53ce8b64a5` (maize-4), `ad818f96bde3c15769f8350fc24d247c` (original).)

## What is next

- The Milestone 1 tail: Unicode source files in the assembler.
- Milestone 2: the per-instruction specification v1.0 and the cycle cost model, the flagship artifact.
- Non-blocking ergonomics: the mazm `-Wswitch` cleanup and optimized/Release CMake presets.

Task-level detail, priorities, and dependencies live on the `maize` Andoneer workbench.
