# Overview

This chapter is framing rather than requirement. It states what Maize v2 is, sketches the
machine in enough detail that the chapters after it read in the right order, names the
design stances that explain why the machine is shaped the way it is, and says plainly what
v2's arrival means for v1. Nothing here constrains an implementation on its own: every
requirement lives in a later chapter, and where this chapter and a later chapter appear to
disagree, the later chapter governs.

## What Maize v2 is

Maize v2 is the second instruction set architecture of the Maize fantasy computer, a 64-bit
machine designed for study, for enjoyment, and for the pleasure of implementing it, held to
the standard of rigor a real architecture is held to. It is a flagless, load-store machine
with thirty-two full-width general registers and a byte-granular, variable-length encoding
built for software decode and for translation to a host processor. Maize v2 is not a
revision of Maize v1 and does not execute v1 binaries. It is a clean break, taken while the
platform is still unreleased and a break is cheap.

The specification is the product. This document exists so that a competent implementer can
build a conforming machine from the prose alone, without reading the reference virtual
machine, and the conformance suite exists so that the implementer can prove it. Two claims
follow from that and are worth stating before anything else. Every input has a defined
outcome, including every invalid one: an encoding this specification does not assign raises
a named trap with a stable numeric cause, and no condition anywhere in the machine is left
undefined or implementation-defined-by-omission. A run is a pure function of the image and
its declared inputs, so two conforming machines executing the same image, with the same
device inputs delivered at the same instruction boundaries, observe the same architectural
state at every instruction boundary.

The architecture is a small frozen base plus named, independently versioned extensions. The
base is everything in this document that is not marked as belonging to an extension, and it
freezes exactly once, at v2.0, forever. Growth happens in extensions (`cap`, `vec`, `meter`,
and `atomic` are the anticipated first set), each with its own version, its own opcode page
behind an escape byte, and its own control-and-status-register range. A conformance claim
names the base version and the exact set of extensions, so a base-only machine is a
complete and certifiable Maize.

## The machine at a glance

The paragraphs that follow are a tour, not a definition; each names the chapter that owns
the subject.

Thirty-two general registers, r0 through r31, hold full 64-bit words, and no sub-register
file exists. Register r0 reads as zero and discards writes. Register r31 is the link
register the call instruction writes. The stack pointer is r30 by convention of the calling
convention and by nothing else, because the architecture itself names a stack in one place
only, the trap-stack control and status register the trap model uses. Software names a
byte, a quarter-word, or a half-word inside a register through the positional extract and
insert instructions, which are the only merge site in the machine. The register model
chapter and the calling-convention chapter own this ground.

An instruction is one opcode byte, optionally preceded by one escape byte, then one byte per
register operand, then whole little-endian immediates, in that order and never in another.
Instruction length is a pure function of the leading one or two bytes, read from a 256-entry
table, and no instruction reads machine state to determine its own length. A register
operand byte carries five bits of register number and three bits of length-neutral form
information. Instructions run from one to ten bytes in the base, and no instruction in the
base or in any future extension exceeds sixteen. The instruction-encoding chapter fixes all
of this, the instruction-inventory chapter names the operations, and the opcode-map appendix
assigns the bytes.

Memory is a flat, little-endian, byte-addressable 64-bit virtual space reached through Sv48
page translation, over a bounded and configurable guest physical memory that software
discovers from a boot-information block rather than by probing. Misaligned accesses are
permitted at every width and raise no trap, and naturally aligned accesses up to eight bytes
are single-copy atomic. Only the load, store, and block-memory families touch memory, and
every operand through which an instruction touches memory carries the `@` sigil. The
memory-model chapter owns this.

Arithmetic writes registers and nothing else, because there is no condition register
anywhere in the machine. A comparison either writes 1 or 0 into a register or fuses into a
branch, multi-precision arithmetic uses explicit carry-in and carry-out registers, and the
conditional-move pair takes its condition from a register tested against zero. The
instruction-inventory chapter has the families.

A trap switches to the trap stack named by a control and status register, pushes exactly
four words (the program counter, the status, the cause, and one auxiliary word), and vectors
by cause. Hardware saves no general-purpose register, so a kernel saves what its own calling
convention says it clobbers and no more. A faulting instruction is restartable, and the
multi-step block-memory instructions define the register state visible at every point at
which they can be interrupted. The trap-model chapter owns this.

Privilege has two levels, supervisor and user, with encoding room reserved for a third that
this version does not define. Every piece of architectural state that is not a general
register lives in a numbered control-and-status-register space that two instructions reach,
with the required privilege level and the read-only property encoded in the register number
itself. The privileged-architecture chapter owns the numbering, the page-table format, and
the translation rules.

Software enters the kernel with the `sys` instruction, whose number, arguments, and results
travel in registers and stay live across the trap boundary. The shape is deliberately the
shape v1 used, so an operating system written for v1 ports across as a recompile plus a
mechanical rewrite of its trap trampolines. Devices live in a port space disjoint from
memory, reached by two privileged instructions. The calling-convention chapter, the
syscall-surface appendix, and the device-surface chapter own these.

Here is a complete function in v2 assembly, summing an array of words, to give the syntax
before any chapter formalizes it.

    ; Sum r3 words starting at the address in r2, returning the total in r2.
    sum_words:
        move r0 r4              ; the running total starts at zero
        branch_eq r3 r0 done    ; an empty array sums to zero
    loop:
        load @r2 r5             ; read one word
        add r4 r5 r4            ; accumulate it
        add r2 #8 r2            ; advance the pointer by one word
        add r3 #-1 r3           ; one fewer word to go
        branch_ne r3 r0 loop
    done:
        move r4 r2              ; the first argument register is also the result register
        return

Operands read source to destination, so `add r4 r5 r4` adds r4 and r5 into r4 and
`load @r2 r5` loads from the address in r2 into r5. Operands are separated by spaces and
never by commas. Every numeric literal names its base, so `#8` is decimal eight and there is
no such thing as a bare number. The terminology chapter states these conventions in full.

## The stances that shape everything

Five stances explain most of the decisions a reader will meet in the chapters that follow.
Each of them cost something, and the cost is named here rather than hidden.

**The machine is translated before it is interpreted.** Every shipped Maize realization is
software, and measurement of the v1 translator showed it covering nearly all executed code
at better than ten times the interpreted speed, so v2 is designed for the translator first
and the interpreter second. That is why there are no condition flags to emulate lazily, why
half-word results zero-extend (which is free on both host architectures the translator
targets), why the conditional-move pair matches the host conditional-move contract exactly,
and why the encoding is regular enough to pattern-match cheaply. The price is that a
hypothetical hardware realization gets a variable-length encoding rather than fixed words,
which is a price x86 and Thumb-2 show is payable.

**Mistakes are made impossible rather than diagnosed.** The name for this stance is
poka-yoke, and it runs from the instruction set through the assembler to the tools. Reserved
opcodes trap, and nothing in reserved space executes as a no-operation. An undefined operand
form traps instead of defaulting to zero. A memory operation cannot target a register slice,
so a narrow store can never silently merge. A numeric literal without a base marker is a
syntax error, so no reader and no lexer ever infers a radix. Each of these costs a little
convenience, and each of them retires a class of bug that v1 shipped at least once.

**One person can implement the base in a weekend.** The budget is a real constraint that
every candidate instruction, every addressing form, and every mode bit paid rent against,
and it is the reason several familiar conveniences (byte and quarter-word arithmetic,
indexed addressing, register-driven bitfield positions, a compressed encoding) are absent
from the base. The budget is also why the base is small enough to freeze with a straight
face.

**The base freezes once, forever.** After v2.0 no instruction is added to the base, no
encoding in the base changes meaning, and no reserved byte in the base is later assigned.
Everything that arrives later arrives as a named extension a machine either implements or
traps on, which is what makes forward compatibility something a conformance binary can test
rather than something a vendor asserts.

**The whole machine is a value.** The architectural state is an enumerable vector with no
hidden components, so a snapshot is complete by construction rather than by audit, and every
nondeterministic input (the clock, entropy, the network, the console) enters only at the
device boundary where it can be recorded and replayed. Fork, snapshot, deterministic replay,
and metering all rest on that property, and the specification's job is to leave nothing out
of the vector.

## The relationship to Maize v1

Maize v1 is a complete, frozen, sixteen-register CISC machine whose arithmetic instructions
take memory operands and whose condition flags follow the x86 lineage, and it stays exactly
that. It keeps its specification, its toolchain, and its role as the machine that teaches
where the mainstream of the last forty years came from. Maize v2 does not extend it and
cannot run its binaries: the register cap lives in a nibble of v1's operand byte, the flag
model lives in every instruction's definition, and the memory operands live in the ALU, so
no extension reaches any of the three. Because v1 is unreleased, the world that has to
recompile is the operating system, the userland, the demos, and the C test corpus, all of
which are in this repository and all of which cross by recompiling. Keeping two personalities
over one translation backend remains possible if binary compatibility ever becomes a
promise, and nothing in v2 forecloses it, but nobody is building that now.

## A reader's map

The chapters are ordered so that a reader who starts at the front and continues to the back
meets nothing before its foundations. A reader who arrives from a search result and needs
one answer can enter anywhere, because each chapter names the chapters it depends on.

- The terminology chapter fixes the size vocabulary (a word is 64 bits), the width letters,
  the register names, the numeric-base markers, and the conventions this specification uses
  to state a requirement. Read it before anything else, because it deliberately breaks with
  the size vocabulary most readers arrive holding.
- The register-model chapter defines the thirty-two registers, the zero register, the link
  register, the fourteen positional slices, and the program counter.
- The instruction-encoding chapter fixes the byte-level structure of every instruction, the
  operand byte, the immediates, the length classes, and the decoding sequence.
- The memory-model chapter defines the address space, endianness, alignment, atomicity, the
  physical-memory bound, the boot-information block, and instruction-fetch coherence.
- The execution-model chapter defines the instruction cycle, the delivery of faults and
  interrupts at instruction boundaries, and the order in which an instruction's effects
  become visible.
- The instruction-inventory chapter lists every base instruction by family with its
  semantics, its operand forms, and the traps it can raise.
- The three instruction-reference chapters state the full behavior of every base
  instruction, grouped as integers and compares, memory and fields and select, and control
  and system and devices.
- The floating-point chapter states the IEEE 754 contract, the rounding modes, the sticky
  flags, and the defined result of every exceptional operation.
- The trap-model chapter defines the cause taxonomy, the four-word frame, vectoring, return
  from a trap, interrupt delivery, and the restartability contract.
- The privileged-architecture chapter defines the privilege levels, the control-and-status
  register space, Sv48 translation, the software translation cache, and page-fault delivery.
- The boot chapter defines the reset address, the reset state, and what the machine has
  already done for the guest before the first instruction runs.
- The calling-convention chapter defines the register-role map, the stack discipline, the
  argument and return rules, and the syscall register contract.
- The device-surface chapter defines the port space, the two port instructions, and the
  device contracts a conforming machine presents.
- The assembly-language chapter defines the source language, the directive set, and the
  rules by which a symbolic target becomes a displacement.
- The extensions chapter defines what an extension is, how one is named and versioned, how
  software discovers it, and what a conformance claim states.
- The versioning chapter defines what a version number promises and what it refuses to
  promise.
- The conformance chapter defines what a conforming machine is and how the suite is
  factored between the base and each extension.
- The appendices carry the opcode map, the encoding quick reference, the syscall surface,
  and the glossary.
