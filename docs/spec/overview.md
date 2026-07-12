# Chapter 1: Overview and Positioning

*This chapter is informative framing. The normative content of the specification begins
in Chapter 2. Nothing here adds a requirement; it states the design goals a reader needs
in order to read the rest of the document correctly.*

## 1.1 What Maize is

Maize is a 64-bit virtual machine that executes a custom byte code. It is a "fantasy
computer": a machine designed for study and enjoyment rather than to model any physical
part, but designed to the same standard of rigor as a real ISA. The reference VM is
implemented in C++ and runs on Windows and Linux; a complete toolchain (the `mazm`
assembler, the `mzld` linker, the `mzdis` disassembler, and the `mzcc` C compiler)
targets it.

Maize is a **flat 64-bit** machine. There is one linear address space of 2^64 bytes.
Pointers, the program counter, and the stack pointer are all full 64-bit values; there is
no segmentation of the address space in v1.0 (segments, where the reservations chapter
mentions them, are a future privilege/protection concept, not an addressing concept).

Maize is intentionally **CISC**. Arithmetic and logic instructions may take a memory
operand directly (for example `ADD @R1 R0`). The machine does *not* go load-store. The
three data-movement instructions CP / LD / ST, plus the zero-extending copy CPZ, are the
enforced memory boundary: CP/CPZ never touch memory, LD only reads memory, ST only writes
memory, and the assembler rejects any spelling that crosses those lines. This keeps the
mnemonic honest about whether an instruction touches memory while leaving the ALU free to
fold a load into an operation.

## 1.2 Design goals

- **Spec-as-product.** The specification is the deliverable. The reference VM is the
  authority for what every instruction does, and this document exists so that a second
  implementation can be written from the prose alone.
- **No undefined behavior, ever.** Every condition that would be undefined behavior on a
  conventional machine is a defined outcome on Maize: either a named trap with a stable
  numeric cause (Chapter 10) or an explicitly enumerated defined, non-trapping result.
  Out-of-range shifts, sparse-memory reads, misaligned accesses, decoded-but-undefined
  operand fields, and floating-point arithmetic exceptions all have defined results. This
  is what makes a Maize binary behave identically under analysis and in production and
  makes two conforming VMs indistinguishable.
- **Determinism.** A run is a pure function of the image and its declared inputs (argv,
  envp, mounts). The VM never inherits ambient host state.
- **Forward compatibility by reservation.** A paging MMU, base-and-bounds segments,
  atomics and SMP, and a future nommu-Linux port are all planned. v1.0 holds the encoding
  space and states the contracts (Chapter 12) so that each arrives as a v1.x extension
  from reserved space, with no v1.0 binary affected.

## 1.3 The reference-implementation principle

Where this document and the reference VM could in principle disagree, the VM is
normative and the document has a defect. Every normative claim in Chapters 2 through 9 is
grounded in the shipped dispatch (`src/cpu.cpp`) and the opcode/register model
(`src/maize_cpu.h`), with the repository `README.md` tables as the human cross-check.
Each chapter names its sources in a **Sourcing** section.

Three narrow points where the shipped code does not yet match the frozen *contract* are
called out explicitly in Chapters 10 and 11 (the reference VM's throw-and-exit trap
delivery, the `$94` OUT form, the unpopulated-port dereference, and the not-yet-enforced
privilege gate). Those are known deferred code fixes against a frozen contract, delivered
by later cards; they are not spec defects, and the conformance suite tests the contract,
not the current code in those spots.

## 1.4 Scope of v1.0

v1.0 freezes **behavior**. In scope: the register and subregister model; the addressing
modes and instruction encoding; every instruction's operation, operand forms, flag
effects, and trap behavior; the memory model; the execution and reset model; the trap and
interrupt taxonomy; the device-facing port-I/O surface; and the IEEE-754 floating-point
contract.

Out of scope for the behavioral freeze: the **cycle-cost / performance model** (Chapter
14, specified separately), and every reserved future extension enumerated in Chapter 12
(paging, segments, SMP/atomics ordering primitives beyond CMPXCHG, the control-register
mechanism, the escape-prefix second opcode plane). Those occupy reserved encoding and
contract space but define no v1.0 behavior.

## 1.5 Document conventions

Radix prefixes (`%` binary, `#` decimal, `$` hex) and the numeric separators
(`` ` `` `_` `,`) are described in the index. Opcodes are written as `$xx` hex bytes.
A `reg` operand is any of the sixteen registers (Chapter 2), optionally suffixed with a
subregister (Chapter 3); an operand written `@X` is a memory dereference (Chapter 5).
"Word" means 64 bits, "half-word" 32, "quarter-word" 16, "byte" 8.
