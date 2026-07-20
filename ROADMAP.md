# Maize Roadmap

## Positioning

Maize is a complete, comprehensible computer: a 64-bit CISC with a fully
specified ISA, an assembler, a C compiler target, and an operating
system, small enough to understand end to end and honest enough to teach
you why real machines look the way they do. It occupies the systems seat
between nand2tetris (hardware up) and MMIX (algorithm analysis): flags,
segments, privilege, interrupts, and port I/O, the conceptual toolkit of
the x86 lineage, cleaned up and made regular. "The machine x86 should
have been."

The specification is the product. The C++ VM is the reference
implementation. Portability means anyone can implement a Maize VM in any
language in a weekend and know they got it right, not that every language
compiles to Maize.

Speed is no longer the asterisk. A C-compiled DOOM runs on Maize at
around 65fps, peaking above 70, with the bytecode still fully
interpreted, before an optimizing compiler backend or a JIT. This matters
for positioning because speed costs Maize none of its defining
properties: a semantics-preserving JIT changes how fast the machine runs,
not what it computes, so Maize is fast and still bit-identical and
deterministic today. The cost model, when it lands (Milestone 2's open
half), will be defined in cycles by the ISA rather than in wall-clock,
which means cycle-metering joins that list without trading any of it
away, a combination most fast VMs give up. The honest ceiling grows: not
only a specification, teaching, and evaluation artifact, but a small,
fully-specified, deterministic computer that is also fast enough to run
real software. Determinism, comprehensibility, a small trust base, and
cost-metering once it is specified, stop being interesting properties of
a slow toy and become production-relevant properties of a runtime you
could deploy, wherever those properties matter more than ecosystem
gravity.

Success criteria: the right few hundred people, independent third-party VM
implementations, someone teaching with it, possibly a paper using it as an
LLM evaluation environment. Not industry adoption.

Still out of scope: competing with WASM on ecosystem and library gravity
as a general-purpose compile target, and chasing industry adoption as a
goal in itself. Maize wins only where its specific properties
(determinism, comprehensibility, cost-metering, contamination-free
evaluation) are what you actually need, not on breadth. No longer
excluded, and now sequenced as post-freeze campaigns rather than out of
scope: optimizing compiler backends (LLVM first, then GCC) and a JIT,
pursued precisely for the speed and language reach the positioning above
now calls for.

# Phase 1: the teaching computer

Milestones 0 through 5. The foundation, and the public on-ramp. All but
the conformance suite and the launch itself are complete.

## Milestone 0: ISA repairs

**Status: complete.**

Fix the known unsoundness before anything else builds on it. These were
cheap to do early and would have been expensive once binaries and
third-party VMs existed.

- Flags model: C (unsigned carry/borrow) and V (signed overflow) are
  separate and defined per operand width, so the documented JLT/JGT
  semantics (N != V) are coherent and JB is live after arithmetic.
- The MUL width-case bug is fixed and covered by tests.
- Signed and unsigned DIV/MOD variants.
- ADC and SBB for wide arithmetic.
- A wide multiply exposing the high half of the 128-bit product.
- The missing branch complements: JGE, JLE, JBE, JAE.
- Flags-on-load/copy: kept, as a deliberate and documented CISC choice.
- The immediate-math operand field: implemented rather than deleted. No
  ghost features in the spec.
- Pointer width and the segment model: a flat 64-bit space, with
  segments as a privilege boundary only.

Exit: met. Every decision is recorded in the spec, the VM implements
them, and hello.mazm still runs.

## Milestone 0.5: Stabilization

**Status: complete.**

Added July 2026 after the first two cards through the pipeline showed
that environment friction, not the work itself, dominated cost.

- Portable C++ toolchain with no MSVC dependency: CMake, Ninja, and
  Clang or GCC on every platform (llvm-mingw or MSYS2 UCRT64 on
  Windows). VS Code (CMake Tools and clangd) is the first-class editor,
  but everything is CLI-first via presets, so Zed, Vim, or anything else
  works anywhere.
- Assembler portability and diagnostics: mazm builds and runs cleanly on
  every supported platform, and wrong input produces useful errors.
- One-command test runner: assemble, run, and diff expected output for
  every test program, green on a fresh clone.
- Opcode implementation audit: a full matrix of spec against assembler
  against VM dispatch, so gaps are found deliberately rather than by
  accident.
- A string-output syscall and a shared test library, so test programs
  stop hand-counting string lengths.
- CI on every push: Linux and Windows build and test matrix.
- The Release-build undefined behavior in the register union is fixed.

Exit: met. A fresh clone builds and passes the full test suite with one
command on Linux and Windows, locally and in CI, with no scratch
patches.

## Milestone 1: C toolchain

**Status: complete.**

The compiler backend was the forcing function that surfaced the
remaining ISA ambiguities, so it ran before the spec freeze.

- Every documented instruction is implemented in the C++ VM.
- The cproc and qbe backend compiles, assembles, and runs nontrivial C,
  including string handling, signed and unsigned division, and the
  documented calling convention. It has since built DOOM, the kilo
  editor, the oksh shell, and the sbase utilities.
- mzld links independently assembled objects into executables.
- Assembler cleanup: proper error reporting. Unicode source files are
  still open and carried into Phase 2.
- mzdis disassembles.
- Every ISA ambiguity the backend exposed was resolved and recorded.

Exit: met, and overshot. The bar was a C hello world plus one real
program. The toolchain now builds a 52-program C conformance corpus and
a complete C-compiled userland.

## Milestone 2: Specification v1.0

**Status: the specification is complete and v1.0 is the behavioral
freeze point. The cost model is deliberately NOT part of it and remains
open.**

The flagship artifact. MMIX-grade rigor is the differentiator.

- Per-instruction specification: encoding, operation, flag effects,
  trap behavior (divide by zero, shift past width, privilege
  violations), sub-register merge semantics, the memory model, and the
  interrupt model. Published as a 21-chapter reference under
  `docs/spec/`.
- Spec versioning policy. v1.0 is a freeze: binaries assembled against
  v1.0 run forever on v1.x VMs.
- Floating point ships **in** v1.0 rather than being deferred to v1.1.
  The ISA carries IEEE-754 arithmetic in the integer register file, an
  FCSR with sticky exception flags, and a compare instruction with the
  float branch idioms built on it. The C toolchain uses hardware float,
  not soft-float.

Exit: met for the behavioral specification. Someone who has never seen
the C++ source can answer behavioral questions from the spec alone.

### The cost model is a separate deliverable, still open

v1.0 freezes *behavior*, not timing, and that separation is deliberate:
a behavioral conformance claim must never depend on a timing model, and
the performance model has to be free to evolve without touching the
behavioral contract. So there is no architectural cycle counter in the
v1.0 instruction set, the VM does not currently report cycles executed,
and no program's result depends on cycle cost. Any such facility arrives
later as a reserved-space extension with its own contract. The details
are in `docs/spec/cycle-cost.md`.

This is the piece that unlocks algorithm analysis and fewest-cycles
play, and it is the remaining half of this milestone's original title.

## Milestone 3: Conformance suite

**Status: open. Along with Milestone 2's cost model, one of the two
Phase 1 engineering deliverables still outstanding.**

The portability story, and the uxn lesson: independent reimplementation
is the strongest signal an ISA project can earn, and it only happens
when the spec plus the tests make it a bounded weekend exercise.

- The normative chapter is written (`docs/spec/conformance.md`). The
  suite itself is the remaining deliverable.
- A suite of binaries plus expected outputs and final states, runnable
  against any VM implementation, covering every instruction, flag
  effect, and trap in the spec.
- Reference VM passes 100% in CI.
- A short "implement your own Maize VM" guide: spec plus suite, done in
  a weekend.

Exit: the suite exists, the reference VM passes, and the guide has been
validated by building a second minimal VM in another language against
it.

## Milestone 4: The computer

**Status: complete, and substantially overshot. Phase 2 is where the
overshoot went.**

The systems curriculum that justifies the CISC design choices.

- Devices: console, keyboard, framebuffer, timer, and a host-filesystem
  mount layer, with device interrupts.
- The OS is **quesOS**, and it went well past this milestone's original
  sketch of a BIOS layer plus a small syscall subset. quesOS is a
  multi-process kernel running on the Sv48 paging MMU with per-process
  address spaces, fork, exec, pipes, signals, poll, and termios, and a
  preemptive scheduler. Guest syscalls arrive through a dedicated trap,
  not the originally sketched software interrupt.
- The shell and the utilities are borrowed rather than built: the oksh
  shell and the sbase coreutils, recompiled for the Maize ISA.
- The curriculum arc as documentation is still partly open: assembly,
  then syscalls, then devices, then OS internals, then write your own
  VM.

Exit: substantially met. quesOS boots to an interactive shell, and
programs can be written and run from inside the machine. Assembling
from inside the machine waits on the self-hosted toolchain, which is
Phase 2 work.

## Milestone 5: Launch

**Status: open, and partly overtaken by events.** DOOM shipped publicly
ahead of any formal launch, and the README has been rewritten more than
once since this milestone was written.

- README rewritten around the positioning statement above, with the ISA
  reference delegated to `docs/spec/` rather than duplicated in it.
- Show HN, r/EmuDev, Handmade Network, Ben Eater orbit.
- LLM evaluation harness as a separate release and second post: spec,
  task suite, and a deterministic grader. Maize has zero training-data
  presence, so it tests reasoning-from-specification rather than
  recall.

Exit: shipped. Measure against the success criteria in Positioning.

# Phase 2: the platform

Phase 1 built a small, comprehensible, fully specified teaching
computer, and that stands on its own both as a deliverable and as the
public on-ramp. Phase 2 grows the same machine, on the same frozen ISA,
into something you can live inside.

**Version 1.0 is quesOS built out as a Unix-like operating system on
Maize**: the kernel, a borrowed userland, and a graphical session, on a
machine whose behavior is specified to the instruction. Running a Linux
distribution remains the long-term destination, but it is the
completeness proof that comes *after* 1.0 rather than the definition of
it.

One doctrine governs every milestone below: **borrow every solved
problem, and build only what is unique to Maize.** Shells, coreutils,
editors, filesystems, window systems, and language runtimes get
recompiled for the ISA rather than reinvented. Maize's own effort goes
to the machine itself, the compiler backends, the kernel and its
syscall ABI, and the per-port shim that lets each borrowed program
land. Every borrowed program is also a forcing function: it pulls the
next real ABI gap into view faster than any amount of speculative
design.

## Milestone 6: Storage

A block device and a real filesystem, so the machine has a disk rather
than a mounted host directory.

- A block device in the VM, documented in the spec's device surface.
- A filesystem using an existing on-disk format (ext2 or FAT). Per the
  doctrine, formats are borrowed, never invented.
- A guest driver, and a host-side tool for building and inspecting
  images.

Exit: a root filesystem lives on a disk image and the userland runs off
it.

## Milestone 7: The graphical session

quesOS gets a window system, borrowed rather than designed.

- A pointer device (queued, interrupt-driven, absolute coordinates,
  buttons, wheel): the last missing piece of input hardware.
- A queued interrupt controller.
- Port a small X server, then a window manager and a terminal emulator
  as its first clients.
- Boot to a graphical session: one command brings up the server, the
  window manager, and a terminal as quesOS children, presenting through
  the graphical host window.

Exit: boot into a desktop, and run a graphical program and a terminal
side by side.

## Milestone 8: Optimizing compiler backends

cproc and qbe compile a strict C subset, which is not enough to build a
distribution. The backends lift that ceiling. **LLVM goes first, then
GCC.**

- LLVM leads because it is one campaign instead of two: its machine-code
  layer is an integrated assembler, disassembler, and object writer, and
  lld links, so a single target description yields the whole toolchain
  rather than requiring a separate binutils port. Its target
  descriptions are largely declarative, which maps cleanly from a frozen
  spec, and its test tooling gives a much tighter iteration loop. It
  also opens the languages that make Maize an interesting target: Rust,
  Zig, Clang C and C++, and Swift.
- GCC follows, for what only it offers: Ada, mature Fortran, COBOL,
  Modula-2, and gccgo.
- Shared groundwork is written once and inherited by whichever campaign
  runs second: the ELF and ABI supplement, the compiler-runtime function
  set, the libc decision, and the end-to-end test harness.

Exit: a C and C++ program of real size builds through the new backend
and runs on the VM.

## Milestone 9: Performance

The interpreter already runs DOOM smoothly, which is the evidence that
what remains is optimization rather than rescue.

- A JIT that tiers up hot blocks from the interpreter while preserving
  semantics exactly. It changes how fast the machine runs, not what it
  computes.
- A fast path for address translation. Every process under quesOS runs
  under paging, and the translation walk is the gap that none of the
  smaller levers address.
- Keep determinism intact: a JIT must stay bit-identical and
  deterministic. And when the cost model lands (Milestone 2's open
  half), it must be defined in cycles by the ISA rather than in
  wall-clock, precisely so that a JITted Maize can be fast and still
  cycle-metered.

Exit: borrowed software feels native, and the cost model still holds.

## Milestone 10: Networking

- A network device in the VM.
- A socket ABI in quesOS, shaped to the same Linux subset as the rest
  of the syscall surface.

Exit: a borrowed network program runs unmodified.

## Milestone 11: quesOS 1.0

The release: everything above, assembled into something a person can
install and use.

- The kernel, the borrowed userland, and the graphical session, on a
  disk image.
- Documentation covering the machine, the OS, and how to build software
  for it.
- The spec still frozen, and the conformance suite still green.

Exit: someone who is not us boots quesOS on Maize and gets work done.

# Beyond 1.0: Linux on Maize

The completeness proof, and the reason the milestones above are ordered
the way they are. Every capability Linux demands of the machine (block
storage, complete MMU behavior, a network device, real signals, timers)
is built and proven first against a kernel we fully control and can
debug, which is exactly what Milestones 6 through 10 do. Porting Linux
onto never-exercised devices would be the worst available diagnostic
position. Doing quesOS first means Linux arrives on hardware that
already works.

Exit: a Linux distribution, its userland recompiled for the Maize ISA,
boots and runs.
