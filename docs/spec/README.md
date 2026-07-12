# The Maize Instruction Set Architecture, Version 1.0

This directory is the normative specification of the Maize virtual machine's
instruction set architecture (ISA). Version 1.0 is the **behavioral freeze point**:
the register model, the instruction encoding, every instruction's operation and flag
effects, the memory model, the trap model, the device-facing surface, and the
floating-point contract are fixed here and do not change within the v1.x series except
by the additive, reserved-space rules of Chapter 12 and Chapter 15.

## Freeze status

v1.0 freezes **behavior**, not timing. Every observable result of every instruction is
pinned. The cycle-cost / performance model is specified separately (Chapter 14 is a
pointer to it) and is explicitly out of the behavioral freeze. Conformance (Chapter 13)
is defined against behavior alone.

## How to read this specification

Each chapter is a single Markdown file. This index assigns the chapter numbers and the
reading order; the chapter files themselves are the normative text. The three chapters
marked **EXISTS** are incorporated **by reference**: their files are canonical and are not
restated here.

**Normative vs informative.** Unless a passage is explicitly marked *informative* (or
sits under a heading named "Rationale", "Note", or "Positioning"), it is normative: a
conforming implementation must behave as it says. Informative passages explain intent
and never add a requirement.

**The reference implementation is normative ground truth.** Maize is spec-as-product:
the reference VM (`src/cpu.cpp`, `src/maize_cpu.h`) is the definition of correct
behavior, and every normative claim in these chapters is grounded in the shipped
dispatch, with the repository `README.md` opcode and flag tables as the human-readable
cross-check. Where a chapter states what the machine does, it states what the code does.
Each newly authored chapter carries a **Sourcing** section naming the code and tables its
claims are keyed to.

**No undefined behavior.** Every condition that is undefined behavior on a conventional
machine is, on Maize, either a named trap (Chapter 10) or an explicitly enumerated
defined, non-trapping result. There is no third category. This is the single property
that makes the machine conformance-testable and two conforming VMs bit-for-bit
equivalent on every input.

## Notation

Numeric radix prefixes, used throughout Maize source and documentation:

- `%` precedes a **binary** value: `%0100_0001`.
- `#` precedes a **decimal** value: `#123`.
- `$` precedes a **hexadecimal** value: `$FEDC`.

The separators `` ` `` (back-tick), `_` (underscore), and `,` (comma) are all legal
inside any numeric literal and carry no value; group digits however reads best
(``$FEDC`BA98``, `%0100_0001`, `#1,000,000`). A bare token with no prefix is decimal.
An operand written with a leading `@` is a **memory address** that gets dereferenced;
without `@` it is a plain value (Chapter 5).

## Table of contents

| Ch | Title | File | Status |
|---:|-------|------|--------|
| 1 | Overview and positioning | [overview.md](overview.md) | NEW |
| 2 | Register model | [register-model.md](register-model.md) | NEW |
| 3 | Subregister model | [subregister-model.md](subregister-model.md) | NEW |
| 4 | Memory model | [memory-model.md](memory-model.md) | NEW |
| 5 | Addressing modes and operand encoding | [addressing-modes.md](addressing-modes.md) | NEW |
| 6 | Instruction encoding | [instruction-encoding.md](instruction-encoding.md) | NEW |
| 7 | Instruction reference | [instruction-reference.md](instruction-reference.md) | NEW |
| 8 | Floating-point | [floating-point.md](floating-point.md) | NEW |
| 9 | Execution model | [execution-model.md](execution-model.md) | NEW |
| 10 | Trap model | [trap-model.md](trap-model.md) | EXISTS |
| 11 | Device-facing surface | [device-surface.md](device-surface.md) | EXISTS |
| 12 | Forward compatibility and reserved space | [reservations.md](reservations.md) | EXISTS |
| 13 | Conformance | [conformance.md](conformance.md) | NEW (thin) |
| 14 | Cycle-cost model | [cycle-cost.md](cycle-cost.md) | NEW (deferral pointer) |
| 15 | Versioning and freeze policy | [versioning.md](versioning.md) | NEW (thin) |
| A | Opcode map (numeric) | [appendix-a-opcode-map.md](appendix-a-opcode-map.md) | NEW |
| B | Encoding quick reference | [appendix-b-encoding-quickref.md](appendix-b-encoding-quickref.md) | NEW |
| C | Syscall surface (informative) | [appendix-c-syscall-surface.md](appendix-c-syscall-surface.md) | NEW |
| D | Glossary | [appendix-d-glossary.md](appendix-d-glossary.md) | NEW |

## What v1.0 is, in one paragraph

Maize is a flat 64-bit CISC byte-code machine with sixteen operand-addressable 64-bit
registers, each sliced into byte / quarter-word / half-word / word subregisters;
variable-length instructions built from a one-byte opcode (two mode bits plus a six-bit
base slot) and one operand byte per operand; a sparse, no-fault, little-endian memory;
five arithmetic/logic flags (C, N, V, P, Z) plus four privileged status bits; a precise,
fully-defined trap model; a Zfinx floating-point extension (IEEE-754 binary32 and
binary64 in the integer registers with a separate FCSR); a port-mapped device surface
with no MMIO; and a reserved-space schedule that lets paging, segments, atomics, and SMP
arrive later without breaking a single v1.0 binary.
