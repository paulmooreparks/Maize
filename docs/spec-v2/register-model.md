# Register Model

This chapter is normative. It fixes the register file, meaning how many registers exist, how
wide they are, which of them the architecture treats specially, how the assembler names them,
and what the dotted positional names mean. It also states what is deliberately not a register,
because
several things a reader arriving from another machine expects to find in the register file
live elsewhere in Maize v2 or do not exist at all.

## The register file

Maize v2 has thirty-two general registers, named r0 through r31, and every one of them is 64
bits wide. There is no second register file. Integer values, addresses, and floating-point
values all live in these registers, and no instruction in the base reads or writes any other
register file, because none exists. The operand byte's register field is five bits, so all thirty-two registers are
reachable from every operand slot of every instruction and no register number is unassigned.

Two registers carry an architectural role, meaning a role the machine itself implements rather
than one a calling convention agrees to.

| Register | Role |
|:---------|:-----|
| r0 | Hardwired zero: reads as zero and discards writes. |
| r31 | Link register: `call` writes the return address here and `return` reads it. |

Every other register, r1 through r30, is an ordinary register with no architectural meaning.
The machine never reads one implicitly, never writes one implicitly, and treats them all
identically in every instruction.

Register widths follow the machine's own vocabulary, in which a word is 64 bits, a half-word
is 32, a quarter-word is 16, and a byte is 8. A register holds exactly one word.

### Every write is a full-width write

An instruction that names a register as its destination writes all 64 bits of it. A half-word
operation writes its 32-bit result zero-extended into the full word, a narrow load writes its
loaded value zero-extended or sign-extended into the full word as its mnemonic states, and a
binary32 floating-point operation writes its result zero-extended into the full word. No
instruction leaves the upper bits of a destination holding what they held before.

The two exceptions are the merge instructions, and they are exceptions by design rather than by
oversight. The `insert` family and `bitfield_insert` read their destination, replace the named
field, and write the rest back unchanged. Those instructions are the only read-modify-write
sites in the instruction set, which is what lets a reader find every place a register's old
value can survive an assignment by searching for two mnemonic stems.

### Names and aliases

The canonical name of a register is `rN`, written in lowercase with a decimal number and no
base marker, for N from 0 through 31. The assembler accepts three architectural aliases in
every position a register name may appear, and the disassembler emits the calling convention's
names rather than the raw numbers.

- `zero` names r0.
- `ra` names r31.
- `sp` names r30.

These aliases are register names, not mnemonics, so they do not violate the rule that an
operation has one canonical spelling. The calling-convention chapter defines the remaining ABI
names, the argument and result registers, and which registers a callee preserves; this chapter
fixes only the names the architecture itself justifies.

## r0, the hardwired zero

Register r0 reads as zero in every operand position of every instruction, and a write to r0 is
discarded. Both halves of that rule are unconditional. No instruction, no privilege level, and
no control and status register setting makes r0 hold a value.

Discarding the write does not cancel the instruction. An instruction that names r0 as its
destination performs every other effect it would otherwise have, including every memory access
it makes, every fault those accesses can raise, every other register it writes, and every
control and status register it writes. The load `load @r9 r0` reads memory at the address in r9
and raises a page fault if that address is inaccessible, and then discards the value. The
division `divide_signed r4 r5 r0` raises the divide-error trap when r5 is zero. The instruction
`csr_read $0100 r0` performs the privilege check the register number demands and traps on
failure. A machine that skips the work because the destination is r0 is not a conforming
machine, and a conformance binary detects it by the faults that fail to arrive.

The zero register is what lets the base leave out a long list of instructions without leaving
out the operations. Four idioms carry most of the weight.

    move r0 r7                   ; clear a register
    subtract r0 r4 r5            ; negate, though negate spells it directly
    branch_ne r4 r0 body         ; branch when r4 is nonzero
    compare_eq r4 r0 r5          ; materialize the is-zero test

Naming r0 in a source position is always legal and never a special case for the decoder,
because r0 is register number zero in an operand byte like any other. One instruction gives r0
a role worth naming: `add_carry` and `subtract_borrow` read a carry-in of zero and discard the
carry-out when the carry register is r0, which turns either instruction into a plain add or
subtract and gives a multi-precision chain its natural first step.

## r31, the link register

Register r31 holds the return address of the innermost call. Both forms of `call` write the
address of the instruction following the call into r31 and then transfer, and `return`
transfers to the address in r31. Those three instructions are the whole of the architectural
behavior; in every other instruction r31 is an ordinary register that software may read,
write, save, and reuse freely.

Ordering matters in one case and the inventory fixes it. The register form of `call` reads its
target from the source register before it writes r31, so `call r31` is a well-defined call
through the current link register rather than a call to the address it is about to overwrite.

Nesting is software's responsibility. The machine maintains no return-address stack and no
shadow copy of r31, so a function that calls another function saves r31 before doing so and
restores it before returning. That is a deliberate consequence of the load-store shape: a call
that pushed its return address to memory would put an implicit memory write inside control
flow, and v2 has no implicit memory writes.

Trap entry does not touch r31. The trap model saves the interrupted program counter in its own
frame and saves no general register at all, so a trap taken between a `call` and its `return`
leaves the link register intact for the interrupted program.

## The stack pointer

The architecture has no stack pointer. Register r30 is the stack pointer by the calling
convention's agreement, the assembler spells it `sp` because software finds that name easier to
read, and the machine attaches no meaning to it whatsoever. No instruction reads r30 unless a
program names it, no instruction writes r30 unless a program names it, and r30 has no reset
value the architecture guarantees.

Three consequences follow, and each is a thing a reader arriving from v1 or from x86 must
unlearn.

- There is no push and no pop. A stack push is a `subtract` on the stack pointer and a `store`,
  a pop is a `load` and an `add`, and both are visible in the instruction stream as the memory
  operations they are.
- The stack's direction, its alignment, its red zone or absence of one, and its frame layout are
  all calling-convention rules rather than machine rules, and the calling-convention chapter
  states them. A program that violates them is wrong about its ABI, not wrong about the machine,
  and the machine raises no trap for it.
- No trap, no call, and no interrupt writes anything to the user stack. The trap model pushes
  its four-word frame to the trap stack whose address comes from a control and status register,
  and that register is the only place in the entire architecture where the concept of a stack
  appears.

The privileged-architecture chapter owns the trap-stack control and status register. It is
not r30, it is not a general register at all, and a kernel that wants r30 to point at its stack
after trap entry loads it there itself.

## Positional register names

The fourteen dotted names, `rN.b0` through `rN.b7`, `rN.q0` through `rN.q3`, and `rN.h0` and
`rN.h1`, name a byte, a quarter-word, or a half-word at a fixed position inside a register.
They are operand notation, and they exist only in the operand slots of `extract` and `insert`.

There is no sub-register file. A dotted name is not a separate piece of state, it is not
independently addressable, it has no existence outside the instruction that spells it, and
writing rN through any instruction changes every dotted name over rN in the obvious way. The
semantics of a dotted operand live entirely on the two instruction families that accept one: a
dotted source on an `extract` produces a fresh full-width value and reads nothing back, and a
dotted destination on an `insert` merges the named field and preserves the rest. The
instruction inventory states both, and the instruction-encoding chapter fixes how the element
index rides in the operand byte's form field.

Indices count from the least significant end, because the machine is little-endian and a
positional name that disagreed with the byte order a store produces would be a second
convention to carry.

| Name | Bits | Width |
|:-----|:-----|:------|
| `rN.b0` | 7 through 0 | byte |
| `rN.b1` | 15 through 8 | byte |
| `rN.b2` | 23 through 16 | byte |
| `rN.b3` | 31 through 24 | byte |
| `rN.b4` | 39 through 32 | byte |
| `rN.b5` | 47 through 40 | byte |
| `rN.b6` | 55 through 48 | byte |
| `rN.b7` | 63 through 56 | byte |
| `rN.q0` | 15 through 0 | quarter-word |
| `rN.q1` | 31 through 16 | quarter-word |
| `rN.q2` | 47 through 32 | quarter-word |
| `rN.q3` | 63 through 48 | quarter-word |
| `rN.h0` | 31 through 0 | half-word |
| `rN.h1` | 63 through 32 | half-word |

All fourteen are reachable, and each is reachable by exactly one encoding. The names overlap by
construction, since `r3.h0` covers the same bits as `r3.q0` and `r3.q1` together, and that
overlap is what makes the notation useful, since software names the field at the width it
means.

A dotted name over r0 obeys the zero register rather than the notation. Extracting any field of
r0 yields zero, and inserting into any field of r0 discards the result, because r0 is hardwired
before the positional machinery applies.

Two boundaries hold everywhere in the specification, and both are stated here as invariants of
the register model rather than as properties of particular instructions.

- Width modifiers ride memory operations and positional dots ride the register operands of
  extract and insert, so `load.zb` names an access width while `r3.b5` names a register
  position, and the two notations never mean the same thing.
- No memory operation targets a register slice. A load writes a whole register and a store reads
  the low bytes of a whole register, so placing a loaded byte at an interior position is a load
  followed by an `insert`, written out where a reader can see it.

## Control and status registers

The control and status registers are architectural state, and they are not part of the register
file. They live in a separate numbered space, no operand byte reaches them, and the only
instructions that touch them are `csr_read` and `csr_write`, which name a register by a 16-bit
number carried as an immediate rather than by an operand byte. Their numbering, their access
rules, and the meaning of each one belong to the privileged-architecture chapter.

Every piece of architectural state that is neither a general register nor the program counter
nor memory is a control and status register. That includes the trap-model state, the kernel
stack address, the paging root, the floating-point rounding mode and sticky exception flags,
the feature bitmap, and the syscall-provider selection bit. Placing all of it in one numbered
space is what lets an extension add state without adding an instruction, and what keeps the
general register file free of anything a compiler cannot allocate.

## What is not a register

Four things a reader may expect to find in a register model are absent from this one, and each
absence is the consequence of a ratified decision rather than a gap.

- The program counter is architectural state but is not a register. No number names it, and
  software reads it only through `pc_add` and writes it only by transferring control, as the
  execution-model chapter states.
- There is no condition register and there are no condition flags. Comparisons write ordinary
  registers, branches fuse their comparison, and multi-precision arithmetic carries its carry in
  a register named by `add_carry` and `subtract_borrow`.
- There is no separate floating-point register file, no floating-point status register in the
  register file, and no floating-point move. Floating-point values occupy ordinary registers,
  and the rounding mode and sticky flags are control and status registers.
- There is no instruction register and no other decoder-visible state in the architecture. What
  a machine holds between fetching a byte and executing an instruction is an implementation
  matter that no program can observe, because an instruction commits all at once.

## Conformance notes

The following properties are directly testable by a binary, and a conforming machine exhibits
all of them.

- Every one of the thirty-two register numbers is accepted in every operand slot of every base
  instruction, with one family of exceptions: the block-memory instructions require their three
  operand slots to name three different registers and exclude r0 from their pointer and count
  slots, which the memory reference chapter states and which constrains encodings rather than
  values. Outside that family, no register number raises an illegal-operand trap on account of
  its value.
- A read of r0 yields zero in every operand position of every instruction, including after an
  instruction that named r0 as its destination.
- An instruction whose destination is r0 still raises the fault its operands demand, tested at
  minimum with a load from an unmapped page, a store to an unmapped page, a division by zero,
  and a control and status register access that fails its privilege check.
- Both forms of `call` write the address of the following instruction into r31, and `call r31`
  transfers to the address r31 held before the call.
- Taking a trap and returning from it leaves all thirty-two general registers with the values
  they held at the instruction boundary, since no general register is saved or restored by the
  machine.
- Each of the fourteen positional names selects the bit range this chapter tabulates, tested by
  an `extract` of a known pattern at each of the fourteen positions and an `insert` of a known
  pattern at each of the same fourteen positions.
- Writing a register through any instruction changes the value every overlapping positional name
  extracts, confirming that no dotted name holds state of its own.
