# Maize Roadmap

## Positioning

Maize is a 64-bit CISC machine with a fully specified instruction set, an
assembler, a C compiler target, and an operating system. It is small
enough that one person can hold all of it in their head at once, and it
does not hide the parts of a real machine that are awkward to explain.

nand2tetris comes at this subject from the hardware side and MMIX from
the algorithm side, and neither one covers the middle, which is where
flags, segments, privilege, interrupts, and port I/O live. That is the
x86 toolkit, cleaned up and made regular. The machine x86 should have
been.

What ships is the specification. The C++ VM in this repo is one
implementation of it. When I say Maize is portable I mean somebody can
write their own VM in whatever language they like over a weekend and
check it against the conformance suite. I do not mean that every
language on earth compiles to Maize.

Speed used to be the asterisk on all of this, and it isn't anymore.
DOOM, compiled from C, runs at about 65fps with the bytecode still fully
interpreted, before any optimizing backend or JIT.

That raises the ceiling without costing anything. A JIT that preserves
semantics changes how fast the machine runs and nothing else, so results
stay bit-identical and deterministic. Once the cost model lands (see
Milestone 2) it will be measured in ISA-defined cycles rather than
wall-clock time, so a fast Maize can still be a metered one. Most fast
VMs cannot say that. A small, fully specified, deterministic machine
that also runs real software at speed is worth deploying anywhere those
properties matter more than library ecosystem.

Success would look like a few hundred of the right people finding it, a
couple of independent VM implementations, somebody teaching with it, and
maybe a paper using it as an LLM evaluation environment. Industry
adoption is not on the list.

Two things stay out of scope. Maize is not going to compete with WASM on
ecosystem and library gravity as a general-purpose compile target, and
chasing adoption for its own sake is not a goal. Maize wins where
determinism, comprehensibility, cost-metering, and contamination-free
evaluation are what you actually need. It loses on breadth, and that is
fine.

Optimizing compiler backends (LLVM first, then GCC) and a JIT used to be
excluded. They are now post-freeze campaigns instead, for the speed and
the language reach described above.

# Phase 1: the teaching computer

Milestones 0 through 5. The foundation, and the public on-ramp.
Everything here is done except the conformance suite, the cost model,
and the launch itself.

## Milestone 0: ISA repairs

**Status: complete.**

Fixing the known unsoundness before anything else got built on top of
it. Cheap to do early, and expensive once binaries and third-party VMs
exist.

- C (unsigned carry/borrow) and V (signed overflow) are separate now,
  defined per operand width, which makes the documented JLT/JGT
  semantics coherent and leaves JB usable after arithmetic.
- The MUL width-case bug is fixed and covered by tests.
- Signed and unsigned DIV/MOD variants.
- ADC and SBB for wide arithmetic.
- A wide multiply that exposes the high half of the 128-bit product.
- The missing branch complements JGE, JLE, JBE, and JAE.
- Flags on load and copy stayed, as a documented CISC choice rather than
  an accident.
- The immediate-math operand field got implemented rather than deleted,
  because the spec should not describe features that do not exist.
- Pointer width and the segment model settled on a flat 64-bit space,
  with segments acting only as a privilege boundary.

Exit: met. The decisions are in the spec, the VM implements them, and
hello.mazm still runs.

## Milestone 0.5: Stabilization

**Status: complete.**

Added in July 2026 after the first two cards through the pipeline made
it obvious that environment friction, not the actual work, was eating
the time.

- A portable C++ toolchain with no MSVC dependency, using CMake, Ninja,
  and either Clang or GCC everywhere (llvm-mingw or MSYS2 UCRT64 on
  Windows). VS Code with CMake Tools and clangd is the first-class
  editor, but everything runs from presets on the command line, so Zed
  or Vim or anything else works too.
- mazm builds and runs cleanly on every supported platform, and bad
  input produces useful errors.
- A one-command test runner that assembles, runs, and diffs expected
  output for every test program, green on a fresh clone.
- An opcode implementation audit covering spec against assembler against
  VM dispatch, so gaps turn up on purpose instead of by surprise.
- A string-output syscall and a shared test library, so test programs
  stopped hand-counting string lengths.
- CI on every push across Linux and Windows.
- The Release-build undefined behavior in the register union is gone.

Exit: met. A fresh clone builds and passes the full suite with one
command on Linux and Windows, locally and in CI, with no scratch
patches.

## Milestone 1: C toolchain

**Status: complete.**

The compiler backend was what forced the remaining ISA ambiguities into
the open, so it ran before the spec freeze rather than after.

- Every documented instruction is implemented in the C++ VM.
- The cproc and qbe backend compiles, assembles, and runs nontrivial C,
  including string handling, signed and unsigned division, and the
  documented calling convention. It has since built DOOM, the kilo
  editor, the oksh shell, and the sbase utilities.
- mzld links independently assembled objects into executables.
- Assembler cleanup, including proper error reporting. Unicode source
  files are still open and carry into Phase 2.
- mzdis disassembles.
- Every ambiguity the backend exposed got resolved and written down.

Exit: met, and then some. The bar was a C hello world plus one real
program. The toolchain now builds a 52-program C conformance corpus and
a complete C-compiled userland.

## Milestone 2: Specification v1.0

**Status: the specification is done and v1.0 is the behavioral freeze
point. The cost model was never part of it and is still open.**

This is the flagship artifact, and MMIX-grade rigor is what makes it
worth anything.

- A per-instruction specification covering encoding, operation, flag
  effects, trap behavior (divide by zero, shift past width, privilege
  violations), sub-register merge semantics, the memory model, and the
  interrupt model. It runs to 21 chapters under `docs/spec/`.
- A versioning policy. v1.0 is a freeze, so binaries assembled against
  it run forever on any v1.x VM.
- Floating point shipped in v1.0 instead of being pushed to v1.1. The
  ISA carries IEEE-754 arithmetic in the integer register file, an FCSR
  with sticky exception flags, and a compare instruction with the float
  branch idioms built on top of it. The C toolchain uses hardware float.

Exit: met for the behavioral specification. Somebody who has never seen
the C++ source can answer behavioral questions from the spec alone.

### The cost model, still open

v1.0 freezes behavior and says nothing about timing, which was a
deliberate split. A conformance claim should not depend on a timing
model, and the performance model needs room to change without touching
the behavioral contract. So there is no architectural cycle counter in
the v1.0 instruction set, the VM does not report cycles executed today,
and no program's result depends on cycle cost. Anything along those
lines arrives later as a reserved-space extension with its own contract.
`docs/spec/cycle-cost.md` has the details.

This is the half of the milestone that unlocks algorithm analysis and
fewest-cycles play, and it is still to do.

## Milestone 3: Conformance suite

**Status: open. One of the two Phase 1 engineering deliverables left,
alongside the cost model.**

This is the portability story, and uxn is where I learned to take it
seriously. An independent reimplementation is the strongest signal a
project like this can earn, and it only happens if the spec and the
tests together make it a bounded weekend exercise.

- The normative chapter is written (`docs/spec/conformance.md`). The
  suite itself is what remains.
- A set of binaries with expected outputs and final states, runnable
  against any implementation, covering every instruction, flag effect,
  and trap in the spec.
- The reference VM passing all of it in CI.
- A short guide to writing your own Maize VM.

Exit: the suite exists, the reference VM passes, and the guide has been
validated by somebody building a second minimal VM against it in another
language.

## Milestone 4: The computer

**Status: complete, and overshot by a lot. Phase 2 is where the
overshoot went.**

The systems curriculum that justifies the CISC design choices.

- Devices, covering console, keyboard, framebuffer, timer, and a
  host-filesystem mount layer, with interrupts.
- The OS is quesOS, and it went well past the BIOS layer plus small
  syscall subset this milestone originally sketched. quesOS is a
  multi-process kernel on the Sv48 paging MMU, with per-process address
  spaces, fork, exec, pipes, signals, poll, termios, and a preemptive
  scheduler. Guest syscalls come in through a dedicated trap rather than
  the software interrupt originally planned.
- The shell and utilities are borrowed rather than written: oksh and the
  sbase coreutils, recompiled for Maize.
- The curriculum arc as documentation is still partly unwritten.

Exit: mostly met. quesOS boots to an interactive shell and programs can
be written and run inside the machine. Assembling inside the machine
waits on the self-hosted toolchain, which is Phase 2 work.

## Milestone 5: Launch

**Status: open, and partly overtaken by events.** DOOM went public well
ahead of any formal launch, and the README has been rewritten more than
once since this was written.

- A README built around the positioning above, with the ISA reference
  delegated to `docs/spec/` instead of duplicated.
- Show HN, r/EmuDev, Handmade Network, the Ben Eater orbit.
- An LLM evaluation harness as a separate release and a second post,
  with the spec, a task suite, and a deterministic grader. Maize has
  zero training-data presence, so it tests reasoning from a
  specification rather than recall.

Exit: shipped. Measured against the success criteria above.

# Phase 2: the platform

Phase 1 produced a small, comprehensible, fully specified teaching
computer, and that stands on its own as both a deliverable and the way
in for anyone new. Phase 2 grows the same machine, on the same frozen
ISA, into something you can live inside.

Version 1.0 is quesOS built out as a Unix-like operating system on
Maize, with the kernel, a borrowed userland, and a graphical session, on
a machine whose behavior is specified down to the instruction. Running a
Linux distribution is still the long-term destination, but it is the
completeness proof that comes after 1.0 rather than the definition of
it.

Everything below follows one rule. Borrow every solved problem and build
only what is peculiar to Maize. Shells, coreutils, editors, filesystems,
window systems, and language runtimes all get recompiled for the ISA
rather than reinvented. The effort goes into the machine, the compiler
backends, the kernel and its syscall ABI, and the small shim each
borrowed program needs. Borrowed software also pays for itself twice
over, because every port drags the next real ABI gap into view faster
than any amount of design work would have.

## Milestone 6: Storage

A block device and a real filesystem, so the machine has a disk instead
of a mounted host directory.

- A block device in the VM, documented in the spec's device surface.
- A filesystem using a format that already exists, either ext2 or FAT.
  Formats get borrowed, never invented.
- A guest driver, plus a host-side tool for building and inspecting
  images.

Exit: the root filesystem lives on a disk image and the userland runs
off it.

## Milestone 7: The graphical session

quesOS gets a window system, borrowed rather than designed.

- A pointer device, queued and interrupt-driven, with absolute
  coordinates, buttons, and a wheel. It is the last piece of input
  hardware still missing.
- A queued interrupt controller.
- A port of a small X server, then a window manager and a terminal
  emulator as its first clients.
- Booting straight to a graphical session, where one command brings up
  the server, the window manager, and a terminal as quesOS children,
  presenting through the graphical host window.

Exit: boot into a desktop and run a graphical program next to a
terminal.

## Milestone 8: Optimizing compiler backends

cproc and qbe handle a strict C subset, which is nowhere near enough to
build a distribution. These backends lift that ceiling. LLVM goes first,
GCC second.

- LLVM leads because it is one campaign instead of two. Its machine-code
  layer already provides an assembler, a disassembler, and an object
  writer, and lld links, so a single target description produces the
  whole toolchain rather than needing a separate binutils port. The
  target descriptions are largely declarative, which maps well onto a
  frozen spec, and the test tooling makes for a much tighter loop. It
  also opens up Rust, Zig, Clang C and C++, and Swift.
- GCC follows, for the things only it has: Ada, mature Fortran, COBOL,
  Modula-2, and gccgo.
- Some groundwork gets written once and inherited by whichever campaign
  runs second, including the ELF and ABI supplement, the
  compiler-runtime function set, the libc decision, and the end-to-end
  test harness.

Exit: a C and C++ program of real size builds through the new backend
and runs.

## Milestone 9: Performance

The interpreter already runs DOOM smoothly, which suggests what is left
is optimization rather than rescue work.

- A JIT that tiers up hot blocks from the interpreter while preserving
  semantics exactly.
- A fast path for address translation. Every process under quesOS runs
  under paging, and the translation walk is the gap none of the smaller
  levers touch.
- Determinism has to survive all of it. A JIT stays bit-identical, and
  when the cost model lands it gets defined in ISA cycles rather than
  wall-clock time so that a JITted Maize can still be metered.

Exit: borrowed software feels native and the cost model still holds.

## Milestone 10: Networking

- A network device in the VM.
- A socket ABI in quesOS, shaped to the same Linux subset as the rest of
  the syscall surface.

Exit: a borrowed network program runs unmodified.

## Milestone 11: quesOS 1.0

The release. Everything above, assembled into something a person can
install and use.

- The kernel, the borrowed userland, and the graphical session, on a
  disk image.
- Documentation covering the machine, the OS, and how to build software
  for it.
- The spec still frozen and the conformance suite still green.

Exit: somebody other than me installs quesOS on Maize and gets something
done with it.

# Beyond 1.0: Linux on Maize

Running Linux on Maize is a completeness proof and is the reason the milestones above are ordered
the way they are. Everything Linux demands of the machine, including
block storage, complete MMU behavior, a network device, real signals,
and timers, gets built and proven first against a kernel I control and
can actually debug, which is what Milestones 6 through 10 amount to.
Bringing Linux up on devices nothing has ever exercised is the worst
possible position to debug from. Doing quesOS first means Linux arrives
on hardware that already works.

Exit: a Linux distribution, its userland recompiled for Maize, boots and
runs.
