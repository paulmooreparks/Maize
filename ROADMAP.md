# Maize Roadmap

## Positioning

Maize is a complete, comprehensible computer: a 64-bit CISC with a fully
specified ISA, an assembler, a BIOS, an OS, and a C compiler target, small
enough to understand end to end and honest enough to teach you why real
machines look the way they do. It occupies the systems seat between
nand2tetris (hardware up) and MMIX (algorithm analysis): flags, segments,
privilege, interrupts, and port I/O, the conceptual toolkit of the x86
lineage, cleaned up and made regular. "The machine x86 should have been."

The specification is the product. The C++ VM is the reference
implementation. Portability means anyone can implement a Maize VM in any
language in a weekend and know they got it right, not that every language
compiles to Maize.

Success criteria: the right few hundred people, independent third-party VM
implementations, someone teaching with it, possibly a paper using it as an
LLM evaluation environment. Not industry adoption.

Explicitly out of scope: an LLVM backend (revisit only if a language worth
its carrying cost demands it), competing with WASM as a universal compile
target, GCC ports.

## Milestone 0: ISA repairs

Fix the known unsoundness before anything else builds on it. These are
cheap now and expensive after binaries and third-party VMs exist.

- Fix the flags model. Separate C (unsigned carry/borrow) and V (signed
  overflow), defined per operand width. Today ADD/SUB compute an unsigned
  carry test and store it in the overflow flag (cpu.cpp), which makes the
  documented JLT/JGT semantics (N != V) incoherent and leaves JB dead
  after arithmetic.
- Fix the MUL copy-paste bug: the width cases compute subtraction
  (cpu.cpp, mul_opcode handler).
- Add signed DIV/MOD variants (or define DIV as signed and add unsigned).
- Add ADC and SBB for wide arithmetic.
- Add a wide multiply (high half of the 128-bit product; MMIX-style
  dedicated register or a two-register form).
- Add missing branch complements: JGE, JLE, JBE, JAE.
- Decide flags-on-load/copy: keep as a deliberate documented CISC choice,
  or restrict flag effects to ALU/CMP/TEST. Decide once, document why.
- Decide the immediate-math operand field: implement it or delete it. No
  ghost features in the spec.
- Decide pointer width and the segment model: is Maize ILP32-on-64 within
  segments (x32-style) or a flat 64-bit space with segments as a
  privilege boundary only? One answer, written down.

Exit: all decisions recorded here or in the spec draft; VM implements
them; hello.mazm still runs (core.mazm was retired to the M4 BIOS work,
see maize-23; it is archived under asm/old_tortilla_files/).

## Milestone 0.5: Stabilization

Added July 2026 after the first two cards through the pipeline showed that
environment friction, not the work itself, dominated cost. Runs before the
remaining M0 semantic cards.

- Portable C++ toolchain with no MSVC dependency: CMake + Ninja + Clang or
  GCC on every platform (llvm-mingw or MSYS2 UCRT64 on Windows). VS Code
  (CMake Tools + clangd) is the first-class editor, but everything is
  CLI-first via presets and a bootstrap script, so Zed, Vim, or anything
  else works on any platform.
- Assembler portability and diagnostics: mazm builds and runs cleanly on
  Linux (today it needs a local patch for Windows-only I/O), and wrong
  input produces useful errors.
- One-command test runner: assemble, run, and diff expected output for
  every test program; green on a fresh clone with one command.
- Opcode implementation audit: a full matrix of README vs assembler vs VM
  dispatch, so gaps like the never-wired branch opcodes are found
  deliberately instead of by accident.
- String-output syscall (write a zero-terminated string) so test programs
  stop hand-counting string lengths.
- Maize test library: shared assert and report helpers for test programs.
- CI on every push: Linux and Windows build + test matrix. Working-repo
  and CI-host strategy (self-hosted Gitea vs GitHub) is decided on the CI
  card.
- Fix the Release-build undefined behavior in the register union design.

Exit: a fresh clone builds and passes the full test suite with one command
on Linux and Windows, locally and in CI, with no scratch patches.

## Milestone 1: C toolchain

The compiler backend is the forcing function that surfaces remaining ISA
ambiguities, so it precedes the spec freeze.

- Finish porting the remaining documented instructions to the C++ VM.
- Complete the CProc (or QBE) backend far enough to compile, assemble,
  and run a nontrivial C program (string handling, arithmetic including
  signed/unsigned division, function calls per the OS ABI).
- Implement the linker so binaries can be built and linked independently.
- Assembler cleanup: proper error reporting, Unicode source files.
- Disassembler.
- Every ISA ambiguity the backend exposes gets resolved and recorded.

Exit: a C hello world plus at least one real program (e.g. a small
interpreter or sort benchmark) compiles from C and runs on the VM.

## Milestone 2: Specification v1.0 and cost model

The flagship artifact. MMIX-grade rigor is the differentiator.

- Per-instruction specification: encoding, operation, flag effects table,
  trap behavior (divide by zero, shift past width, privilege violations),
  sub-register merge semantics, memory model, interrupt model.
- Cost model: defined cycle costs per instruction; the VM reports cycles
  executed. This enables algorithm analysis and optimization play
  (fewest-cycles leaderboards).
- Spec versioning policy. v1.0 is a freeze: binaries assembled against
  v1.0 run forever on v1.x VMs.
- Floating point is deferred to spec v1.1 and marked reserved in v1.0.
  The C toolchain uses soft-float until then.

Exit: a person who has never seen the C++ source can answer any
behavioral question from the spec alone.

## Milestone 3: Conformance suite

The portability story, and the uxn lesson.

- A test suite of binaries plus expected outputs/final states, runnable
  against any VM implementation, covering every instruction, flag effect,
  and trap in the spec.
- Reference VM passes 100% in CI.
- A short "implement your own Maize VM" guide: spec + suite = done in a
  weekend.

Exit: the suite exists, the reference VM passes, and the guide has been
validated by building a second minimal VM (any language) against it.

## Milestone 4: The computer

The systems curriculum that justifies the CISC design choices.

- Devices: console (in progress), filesystem, timer; device interrupts.
- BIOS layer (core.mazm or its C successor) over the devices.
- OS with the documented syscall surface (INT $80 subset) and the CLI.
- The curriculum arc written as documentation: assembly -> syscalls ->
  devices -> OS internals -> write your own VM.

Exit: boot to CLI, write/assemble/run a program from inside the machine.

## Milestone 5: Launch

- README rewritten around the positioning statement above; the curriculum
  arc is the spine, the "long story" moves to a history section.
- Show HN, r/EmuDev, Handmade Network, Ben Eater orbit.
- LLM evaluation harness as a separate release and second post: spec +
  task suite + deterministic grader. Maize has zero training-data
  presence, so it tests reasoning-from-specification rather than recall.

Exit: shipped. Measure against the success criteria in Positioning.
