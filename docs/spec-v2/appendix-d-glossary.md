# Appendix D: Glossary

This appendix collects the terms of art the specification uses and defines each one once. It
is a reference rather than an authority: every entry restates what a normative chapter fixes,
and the chapter named in the entry governs where the two differ. Terms are grouped by subject
and alphabetized within each group.

## Sizes, notation, and vocabulary

**Base marker.** A base marker is the character that opens every numeric literal in Maize
assembly and names its radix, `#` for decimal, `$` for hexadecimal, and `%` for binary. There
is no default base, and a token beginning with a digit is a syntax error. The terminology
chapter and the assembly-language chapter own it.

**Byte.** A byte is 8 bits, the smallest addressable unit of memory, and the width the `.b`
letter names.

**Half-word.** A half-word is 32 bits, half of the machine's word, and the width the `.h`
letter names. It is the only width at which an arithmetic instruction carries a width
modifier.

**Memory sigil.** The memory sigil is the `@` character, which marks every operand through
which an instruction reaches memory. It is mandatory on such an operand and a syntax error
anywhere else, so a reader finds every memory access in a listing by searching for one
character.

**Quarter-word.** A quarter-word is 16 bits, a quarter of the machine's word, and the width
the `.q` letter names.

**Width letter.** A width letter is the dotted suffix on a mnemonic that names the size the
instruction operates on, `.b` for the byte, `.q` for the quarter-word, and `.h` for the
half-word. A mnemonic with no width letter operates on the full word, and the immediate move
is the single exception, since it spells the word width as `move.w` so that no immediate move
can be written without stating its width. Where a narrow operation
must also say how a value reaches full width, a `z` for zero-extension or an `s` for
sign-extension sits immediately before the width letter, as in `load.zb` and `move.sh`.

**Word.** A word on Maize is 64 bits, the machine's native operand size and the width of every
general register. The name is literal: a half-word is half of it and a quarter-word is a
quarter of it. This is deliberately neither the Intel convention, which freezes a word at 16
bits, nor the RISC-V and ARM convention, which freezes it at 32. The terminology chapter owns
the ruling.

## Encoding

**Escape byte.** An escape byte is one of the seven bytes `$F8` through `$FE` on the primary
opcode page, each of which opens one 256-entry extension opcode page. A machine that does not
implement the owning extension raises the illegal-instruction trap on the escape byte itself
and never fetches the byte after it.

**Form field.** The form field is bits 7 through 5 of an operand byte, carrying
length-neutral operand structure whose meaning the opcode fixes for that operand position. In
the base it carries exactly one thing, which is the element index of a register slice.

**Immediate.** An immediate is a constant of 1, 2, 4, or 8 bytes stored little-endian at a byte
boundary after every operand byte of the instruction. Its size comes from the opcode and never
from a field inside the instruction, and it is never split across other components.

**Length class.** A length class is the shape of an instruction written as a string in which
`op` is the opcode byte, `r` is one operand byte, and `iN` is an immediate of N bytes. The base
uses fifteen classes, and the class fixes the instruction's total length.

**Opcode byte.** The opcode byte is the flat 8-bit index that names the operation and fixes the
shape of everything that follows it. It carries no mode bits and no sub-fields.

**Operand byte.** An operand byte is the single byte that encodes exactly one register operand,
split as five bits of register number and three bits of form field. No operand shares a byte
with another operand.

**Plain slot.** A plain slot is an operand slot that names a whole register and requires a form
field of `%000`. Any other form field in a plain slot raises the illegal-operand trap. Almost
every operand slot in the base is plain.

**Reserved.** Reserved means that an encoding, a control-and-status-register number, a bit, or
a field is unassigned in this version and raises a trap when used. Reserved space never
executes as a no-operation, never reads as a defaulted value, and never behaves as an
approximation of a neighboring encoding.

**Sliced slot.** A sliced slot is an operand slot that names one element of a register, with
the element index carried in the form field and the element width carried by the opcode. The
base has byte-sliced, quarter-word-sliced, and half-word-sliced slots, and they appear only on
the extract and insert instructions.

## Registers and architectural state

**ABI name.** An ABI name is an assembler alias the calling convention assigns to a register,
such as `a0` for r2 or `s0` for r20. The disassembler emits ABI names, and an alias is a second
name for a register rather than a second name for an operation.

**Carry register.** A carry register is the ordinary general register that `add_carry` and
`subtract_borrow` read as the carry or borrow coming in and write as the carry or borrow going
out. It is the register-file equivalent of the carry flag this machine does not have.

**Control and status register.** A control and status register holds a piece of architectural
state that is not a general register, is named by an unsigned 16-bit number rather than by an
operand byte, and is reached only by `csr_read` and `csr_write`. The number itself carries the
minimum privilege level and the read-only property. The privileged-architecture chapter owns
the space.

**Feature bitmap.** The feature bitmap is the read-only control and status register in which
each allocated extension opcode page has one bit, set when the machine implements the
extension owning that page. It answers the fast presence question, and the boot-information
block's extension list answers the version question.

**Link register.** The link register is r31, into which both forms of `call` write the address
of the following instruction and from which `return` transfers. Its architectural role is
confined to those three instructions.

**Merge site.** A merge site is an instruction that reads its destination register, replaces
part of it, and writes the rest back unchanged. The `insert` family and `bitfield_insert` are
the only merge sites in the instruction set, so every place a register's old value can survive
an assignment is findable by searching for two mnemonic stems.

**Positional slice.** A positional slice is a named element inside a register, written as the
register name, a dot, a width letter, and an element index counting from the least significant
element, as in `r3.b5`, `r3.q2`, and `r3.h1`. Fourteen exist. A slice is operand notation and
not a second register file, and it appears only in the operand slots of extract and insert.

**Program counter.** The program counter holds the address of the instruction the machine is
executing. It is architectural state and not a register: no number names it, and software
reads it only through `pc_add` and writes it only by transferring control.

**Zero register.** The zero register is r0, which reads as zero in every operand position and
discards every write. Discarding the write does not cancel the instruction, so an instruction
naming r0 as its destination still performs every memory access it makes and raises every
fault those accesses can raise.

## Execution, traps, and interrupts

**Auxiliary word.** The auxiliary word is the fourth word of the trap frame, carrying the one
value a handler needs that is not derivable from the cause and the program counter: the
offending byte for a decode-class fault, the faulting virtual address for a page fault, the
syscall number for a syscall, and zero where the cause has nothing to report.

**Cause.** A cause is the stable number in the range 0 through 255 that names why the machine
entered the kernel. It is the index into the vector table and the value the frame's cause word
carries. Causes 0 through 31 are synchronous and causes 32 through 255 are external
interrupts.

**Defined.** Defined means that the specification names the outcome for every input, including
inputs a conventional architecture would leave undefined. Where a chapter says an outcome is
defined, a second implementation reaches the identical architectural state.

**Double fault.** A double fault is a page fault raised by the vector-table read or by any of
the four frame stores of a trap already being delivered. The machine does not attempt to
deliver it; it halts, recording the original cause and a halt kind of 2.

**Fault.** A fault is a condition an instruction ran into, and the frame captures the address
of the faulting instruction itself so that a handler that removes the condition can return and
let the instruction run again.

**Halt cause register.** The halt cause register is the read-only control and status register
that records why the machine stopped, carrying the cause, the subcode, and a kind that
distinguishes the halt instruction, an uninstalled handler, and a double fault.

**Interrupt.** An interrupt is a cause that arrives from outside the instruction stream, taken
between instructions and, for the block-memory family only, at the defined mid-operation
boundaries. The frame captures the address of the instruction that would have run next.

**Restartability.** Restartability is the spec-wide contract that a faulting instruction can
run again after the kernel services the fault and produce the result it would have produced
had the fault never happened. Single-step instructions satisfy it by having taken no effect;
the block-memory instructions satisfy it by defining the register state visible at every point
at which they can stop.

**Status word.** The status word holds the privilege level in bits 1 and 0 and the
external-interrupt enable in bit 2, with every other bit reserved. It lives in a control and
status register and is snapshotted into the second word of every trap frame.

**Subcode.** A subcode is the value in bits 15 through 8 of the cause word that distinguishes
conditions a single cause covers, such as division by zero from quotient overflow. Every cause
that defines no subcode writes zero there.

**Trap.** Trap has two senses this document keeps apart. As a verb, to trap means that the
machine abandons the current instruction's remaining effects and delivers a named cause
through the vectored path, always and never advisorily. As a noun in the narrow sense, a trap
is a condition the instruction asked for, whose frame captures the address of the following
instruction because there is nothing to retry; the syscall entry and the breakpoint are the
two members of that class in the base.

**Trap frame.** The trap frame is the four words the machine pushes on every trap, in order the
program counter, the status word, the cause word, and the auxiliary word. No general-purpose
register is saved with it.

**Trap stack.** The trap stack is the full-descending stack, named by a control and status
register and required to be 16-byte aligned, to which the machine pushes trap frames. It is
the only place in the entire architecture where the concept of a stack appears. Most
operating systems call this structure the kernel stack.

**Vector table.** The vector table is the 256-entry table of 8-byte handler addresses, based at
a 2 KiB-aligned virtual address held in a control and status register, from which the machine
fetches a handler by cause number. An entry of zero means no handler is installed, and the
machine halts rather than delivering.

## Memory and translation

**Bare mode.** Bare mode is the translation mode in which the physical address equals the
virtual address, no page table is consulted, and no access can page-fault. It is the reset
state.

**Boot-information block.** The boot-information block is the structure the machine writes into
memory before the first instruction executes, holding the address map, the extension list, and
the version and size fields that let a reader written against one format version read a later
one. Its address is reported in a read-only control and status register, and it is software's
only sanctioned source for how much memory exists.

**Defined-allow misalignment.** Defined-allow misalignment is the architecture's stance that
every memory access is permitted at every address and every width, with no alignment
requirement and no trap. The cost is performance and never correctness.

**Leaf.** A leaf is a page-table entry with V set and at least one of R, W, and X set, naming a
page rather than the next table down. A leaf above level 0 maps a superpage.

**Page fault.** A page fault is the cause delivered when address translation cannot satisfy an
access, numbered 8 for a fetch, 9 for a load, and 10 for a store, with subcode 0 for no valid
mapping and subcode 1 for a permission violation.

**Paging root.** The paging root is the control and status register carrying the translation
mode and the physical address of the root page table. Every write to it flushes every cached
translation, whether or not the value changes.

**Physical-memory fault.** The physical-memory fault is the cause delivered when an access
reaches a physical address that populated memory does not cover, numbered 11, with the
faulting physical address in the auxiliary word. It is raised in bare mode and under
translation alike, and the boot-information block's address map is what defines populated
memory.

**Single-copy atomic.** A naturally aligned access of 8 bytes or fewer is single-copy atomic,
meaning an observer sees it as having happened entirely or not at all rather than as a mixture
of bytes from two stores. A misaligned access carries no atomicity guarantee.

**Superpage.** A superpage is the region a leaf above level 0 maps: 2 MiB at level 1, 1 GiB at
level 2, and 512 GiB at level 3. A leaf whose physical address field has any nonzero bit below
its level's page boundary is rejected as invalid rather than aliased to an aligned address.

**Sv48.** Sv48 is the translation mode in which the machine translates the low 48 bits of a
virtual address through four levels of 512-entry page table, ignoring bits 63 through 48 with
no canonical-form check. The page-table format is unchanged from Maize v1.

**Translation cache.** The translation cache holds translations the machine has already
performed. It is architecturally invisible except for one rule: a cached translation stays
usable until something invalidates it, and writing a page-table entry is not something that
invalidates it. A write to the paging root and the two TLB maintenance instructions are the
three events that do.

## Devices

**Bulk transfer.** A bulk transfer is a bounded movement of data between a device and a region
of ordinary guest memory whose physical base address the guest registered through a port. It
happens only as the direct result of a `port_out` to a command port, and the buffer remains
ordinary memory throughout.

**Device class.** A device class is one of the seven kinds of device the base defines, each
owning a block of sixteen consecutive ports based at the class code times sixteen, one
interrupt line, and a contract a machine implements whole or not at all. The console and the
timer are required and the other five are optional.

**Port.** A port is one of the 65,536 numbered slots of the port space, reached only by
`port_in` and `port_out`, both privileged. A port identifier is not an address, and the port
space is disjoint from memory.

**Unpopulated port.** An unpopulated port is one no device answers. A read of it yields zero
and a write to it is discarded, on every conforming machine, which is what makes presence
detection work without a trap handler.

## Governance and conformance

**Base.** The base is everything in this specification not marked as belonging to an
extension. It freezes exactly once, at version `2.0`, and a machine implementing the base and
nothing else is a complete, certifiable Maize v2 machine rather than a subset or a profile.

**Conformance binary.** A conformance binary is a guest program that runs on the machine under
test, exercises one behavior, records what it observed into architectural state, and halts.
The observable channel is the architectural state at the halt, which keeps the suite
independent of any device surface.

**Conformance claim.** A conformance claim names the base version and the exact, exhaustive set
of implemented extensions with their versions. A claim listing one extension asserts that
every other ratified extension is absent.

**Erratum.** An erratum is a correction to the ratified text that changes no conformance test's
expected result. It carries a third version component, and nothing a program can observe
distinguishes one erratum level from another.

**Extension.** An extension is a named, versioned, optional unit of architecture with exactly
one allocated opcode page, its own contiguous control-and-status-register range, its own
specification chapter, and its own conformance-suite section. It is ratified as a whole and
implemented as a whole.

**Registry.** The extension registry is the document recording, for every ratified extension,
its name, its escape byte, its register range, its feature-bitmap bit, its current version, and
its declared dependencies. Allocation is part of ratification and never precedes it, and
deallocation does not exist.

## Floating point

**Binary32 and binary64.** Binary32 and binary64 are the two IEEE 754 formats the base
implements. A binary64 value occupies the whole word of a register and a binary32 value
occupies the low half-word, with a binary32 result written zero-extended into the full
destination and no NaN-boxing anywhere.

**Canonical quiet NaN.** The canonical quiet NaN is the value every NaN-producing arithmetic
operation returns instead of propagating a payload: `$7FF8000000000000` in binary64 and
`$7FC00000` in binary32. Negation and absolute value are the two exceptions, since they touch
only the sign bit.

**Correctly rounded.** An operation is correctly rounded when it returns the representable
value nearest the exact mathematical result under the current rounding direction. Addition,
subtraction, multiplication, division, square root, the fused operations, and the format
narrowing are all correctly rounded.

**fcsr.** The `fcsr` is the user-accessible control and status register holding the
floating-point state: the three-bit rounding-mode field `frm` in bits 7 through 5 and the five
sticky exception flags `fflags` in bits 4 through 0, with every higher bit reserved and
rejected on a write.

**Quiet comparison.** A quiet comparison is one that raises no flag on a quiet NaN operand and
answers the unordered case as its predicate defines. Every floating-point comparison in the
base is quiet, which is a deliberate divergence from RISC-V.

**Sticky flag.** A sticky flag is one of the five accumulated IEEE 754 exception indicators,
`nv`, `dz`, `of`, `uf`, and `nx`, each set by the machine and cleared only by software.
Arithmetic exceptions never divert control, so a flag is the whole of what an exceptional
operation reports.

## Software conventions

**Frame address.** The frame address is the value the stack pointer held when a function was
entered, which is the anchor for that function's incoming stack arguments, its saved link
register, and, where one is used, its frame pointer.

**LP64.** LP64 is the C type mapping the ABI fixes, in which `char` is a signed byte, `short` is
a quarter-word, `int` is a half-word, and `long`, `long long`, and every pointer are a full
word.

**Poka-yoke.** Poka-yoke is the name this specification gives the mistake-proofing stance
that runs through the whole design: reserved opcodes trap, undefined operand forms trap, memory
operations cannot merge into register slices, and a numeric literal without a base marker is a
syntax error. Each instance costs a little convenience and retires a class of bug.

**Red zone.** A red zone is memory below the stack pointer that a leaf function may use without
allocating it. Maize v2 has none: the bytes below `sp` belong to nobody and may be written at
any instruction boundary.

**Syscall provider.** A syscall provider is one of the numbering and semantic conventions a
machine's operating system offers for system calls, selected by a bit in a control and status
register rather than by the pair of opcodes v1 spent on the toggle.

**va_list.** A `va_list` is one word holding the address of the next unread variadic slot.
Every variadic argument travels on the stack, so `va_start` captures an address, `va_arg`
reads and advances, `va_copy` copies the word, and `va_end` generates no instruction.
