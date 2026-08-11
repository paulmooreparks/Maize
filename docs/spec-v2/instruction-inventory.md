# Instruction Inventory

This chapter is normative. It lists every instruction in the Maize v2 base instruction set,
grouped by family, and for each one gives the canonical mnemonic, the operand list in
source-to-destination order, the semantics, and the encoding form. The opcode byte for each
instruction is in the opcode-map appendix, and the byte-level rules the encoding forms refer
to are in the instruction-encoding chapter.

The base is frozen once. Every operation a program can reach without an extension is here,
and the last section of this chapter says what is deliberately not here and which decision
removed it.

## How to read an entry

Every family opens with prose stating the rules shared by the whole family, then lists its
instructions in a table. The table columns are the same throughout.

- **Mnemonic** gives the one canonical spelling. Mnemonics are lowercase, compound names
  spell their words with underscores, and a dot appears only before a length or format
  specifier.
- **Operands** gives the assembly form in source-to-destination order, comma-free. A
  register is written `rN`, an immediate is written with its mandatory base marker (`#` for
  decimal, `$` for hexadecimal, `%` for binary), and the `@` sigil marks every operand
  through which the instruction touches memory.
- **Semantics** states what the machine does in one sentence.
- **Form** names the length class from the instruction-encoding chapter, where `op` is the
  opcode byte, `r` is one operand byte, and `iN` is an immediate of N bytes.

Widths follow the machine's own vocabulary throughout: a word is 64 bits, a half-word is 32,
a quarter-word is 16, and a byte is 8. The letters `.h`, `.q`, and `.b` name half-word,
quarter-word, and byte, and a bare mnemonic operates on the full word. The immediate move is
the one exception, since it spells the word width out as `move.w`, and the family paragraph
below says why.

Two rules apply to every instruction in the chapter and are not repeated per entry. Register
r0 reads as zero and discards writes, so an instruction that names r0 as its destination
still performs all of its other effects, including every memory access and every fault those
accesses can raise. Every input has a defined outcome: where an entry does not name a trap,
the instruction raises none for any operand encoding and any operand value.

## Constants and moves

The instructions in this family write a register from another register, from a literal, or
from the program counter, and none of them touches memory. The narrow immediate forms spell
their extension rule with `z` and `s` the way loads do, so a reader never has to remember a
default. Every form writes the full 64-bit destination; none of them merges into a slice.

| Mnemonic | Operands | Semantics | Form |
|:---------|:---------|:----------|:-----|
| `move` | `move rs rd` | Copies the whole word in rs into rd. | `op r r` |
| `move.w` | `move.w $imm rd` | Writes the 64-bit literal into rd. | `op r i8` |
| `move.zb` | `move.zb $imm rd` | Writes the 8-bit literal zero-extended into rd. | `op r i1` |
| `move.sb` | `move.sb $imm rd` | Writes the 8-bit literal sign-extended into rd. | `op r i1` |
| `move.zq` | `move.zq $imm rd` | Writes the 16-bit literal zero-extended into rd. | `op r i2` |
| `move.sq` | `move.sq $imm rd` | Writes the 16-bit literal sign-extended into rd. | `op r i2` |
| `move.zh` | `move.zh $imm rd` | Writes the 32-bit literal zero-extended into rd. | `op r i4` |
| `move.sh` | `move.sh $imm rd` | Writes the 32-bit literal sign-extended into rd. | `op r i4` |
| `pc_add` | `pc_add $imm rd` | Writes the address of the next instruction plus the sign-extended 32-bit literal into rd. | `op r i4` |

The move family covers eight encodings, and the spelling of the mnemonic names the encoding
outright. The bare `move` is the register-to-register form and nothing else. Every immediate
move states its width in a length specifier, which is `move.w` for the 64-bit literal and one
of the six narrow forms for a byte, quarter-word, or half-word literal. An immediate move
written without a width specifier is a syntax error. Because every literal carries a mandatory
base marker, a register operand and an immediate operand are never confusable, and neither the
base marker nor the digit count of a literal ever carries width information. The `.w`
specifier exists nowhere else in the base, and it is here because a bare immediate move would
otherwise have to pick an encoding that the source never named.

`pc_add` is how position-independent code names an address. Adding `#0` yields the address
of the following instruction, which is the only way software reads the program counter,
since the program counter is not a general register.

## Integer arithmetic and logic

Every instruction in this family reads registers and writes a register, and none of them
touches memory. There is no condition register, so no instruction in this family produces a
side effect other than its destination register; multi-precision arithmetic uses the
explicit carry forms below.

A bare mnemonic operates on the full 64-bit word. A `.h` mnemonic operates on the low 32
bits of its sources, ignoring their upper halves, and writes its 32-bit result
**zero-extended** into the full 64-bit destination. Width-modified forms exist for `.h` only.

Three shared rules cover the operand cases that a conventional machine leaves undefined.

- Arithmetic wraps modulo the operation width, and overflow is neither trapped nor recorded;
  software that needs to detect it compares operands before the operation or inspects the
  result afterward.
- A shift count is taken modulo the operation width, so a word shift uses the low 6 bits of
  the count and a `.h` shift uses the low 5 bits. Every count value is therefore defined,
  and neither host architecture the translator targets needs a fixup.
- A division or remainder by zero raises the divide-error trap, and the signed division of
  the most negative value by -1 raises the same trap with the quotient-overflow subcode. No
  division produces an approximated result.

In the three-operand forms the first named source is the left operand, so `subtract r1 r2 r3`
computes r1 minus r2 into r3, and `shift_left r1 r2 r3` shifts r1 left by r2 into r3. In an
immediate form the literal always occupies the second source position, so `subtract r1 $8 r3`
computes r1 minus 8 and `shift_left r1 #3 r3` shifts left by 3. An ALU immediate is 32 bits
sign-extended to the operation width, and a shift-count immediate is 8 bits taken modulo the
operation width.

### Register-to-register forms

| Mnemonic | Operands | Semantics | Form |
|:---------|:---------|:----------|:-----|
| `add` | `add rs1 rs2 rd` | Writes rs1 plus rs2 into rd. | `op r r r` |
| `add.h` | `add.h rs1 rs2 rd` | Writes the low half-words of rs1 and rs2 added, zero-extended, into rd. | `op r r r` |
| `subtract` | `subtract rs1 rs2 rd` | Writes rs1 minus rs2 into rd. | `op r r r` |
| `subtract.h` | `subtract.h rs1 rs2 rd` | Writes the half-word difference, zero-extended, into rd. | `op r r r` |
| `multiply` | `multiply rs1 rs2 rd` | Writes the low word of the product of rs1 and rs2 into rd. | `op r r r` |
| `multiply.h` | `multiply.h rs1 rs2 rd` | Writes the low half-word of the half-word product, zero-extended, into rd. | `op r r r` |
| `multiply_high_signed` | `multiply_high_signed rs1 rs2 rd` | Writes the high word of the signed 128-bit product into rd. | `op r r r` |
| `multiply_high_unsigned` | `multiply_high_unsigned rs1 rs2 rd` | Writes the high word of the unsigned 128-bit product into rd. | `op r r r` |
| `divide_signed` | `divide_signed rs1 rs2 rd` | Writes the signed quotient of rs1 by rs2, truncated toward zero, into rd. | `op r r r` |
| `divide_signed.h` | `divide_signed.h rs1 rs2 rd` | Writes the signed half-word quotient, zero-extended, into rd. | `op r r r` |
| `divide_unsigned` | `divide_unsigned rs1 rs2 rd` | Writes the unsigned quotient of rs1 by rs2 into rd. | `op r r r` |
| `divide_unsigned.h` | `divide_unsigned.h rs1 rs2 rd` | Writes the unsigned half-word quotient, zero-extended, into rd. | `op r r r` |
| `remainder_signed` | `remainder_signed rs1 rs2 rd` | Writes the signed remainder, taking the sign of rs1, into rd. | `op r r r` |
| `remainder_signed.h` | `remainder_signed.h rs1 rs2 rd` | Writes the signed half-word remainder, zero-extended, into rd. | `op r r r` |
| `remainder_unsigned` | `remainder_unsigned rs1 rs2 rd` | Writes the unsigned remainder into rd. | `op r r r` |
| `remainder_unsigned.h` | `remainder_unsigned.h rs1 rs2 rd` | Writes the unsigned half-word remainder, zero-extended, into rd. | `op r r r` |
| `and` | `and rs1 rs2 rd` | Writes the bitwise AND of rs1 and rs2 into rd. | `op r r r` |
| `or` | `or rs1 rs2 rd` | Writes the bitwise OR of rs1 and rs2 into rd. | `op r r r` |
| `xor` | `xor rs1 rs2 rd` | Writes the bitwise exclusive OR of rs1 and rs2 into rd. | `op r r r` |
| `shift_left` | `shift_left rs1 rs2 rd` | Writes rs1 shifted left by rs2 modulo 64 into rd. | `op r r r` |
| `shift_left.h` | `shift_left.h rs1 rs2 rd` | Writes the half-word left shift, zero-extended, into rd. | `op r r r` |
| `shift_right_logical` | `shift_right_logical rs1 rs2 rd` | Writes rs1 shifted right by rs2 modulo 64 with zero fill into rd. | `op r r r` |
| `shift_right_logical.h` | `shift_right_logical.h rs1 rs2 rd` | Writes the half-word logical right shift, zero-extended, into rd. | `op r r r` |
| `shift_right_arithmetic` | `shift_right_arithmetic rs1 rs2 rd` | Writes rs1 shifted right by rs2 modulo 64 with sign fill into rd. | `op r r r` |
| `shift_right_arithmetic.h` | `shift_right_arithmetic.h rs1 rs2 rd` | Writes the half-word arithmetic right shift, zero-extended, into rd. | `op r r r` |

### Unary forms

| Mnemonic | Operands | Semantics | Form |
|:---------|:---------|:----------|:-----|
| `not` | `not rs rd` | Writes the bitwise complement of rs into rd. | `op r r` |
| `not.h` | `not.h rs rd` | Writes the complement of the low half-word of rs, zero-extended, into rd. | `op r r` |
| `negate` | `negate rs rd` | Writes zero minus rs into rd. | `op r r` |
| `negate.h` | `negate.h rs rd` | Writes the half-word negation, zero-extended, into rd. | `op r r` |
| `byte_reverse` | `byte_reverse rs rd` | Writes the eight bytes of rs in reverse order into rd. | `op r r` |
| `byte_reverse.h` | `byte_reverse.h rs rd` | Writes the four bytes of the low half-word of rs reversed, zero-extended, into rd. | `op r r` |

`byte_reverse` and `byte_reverse.h` are in the base on the strength of the networking
milestone, where byte-order conversion is on the hot path of every packet. Reversing a
quarter-word is `byte_reverse.h` followed by a shift of 16, or an extract, because the
width-modifier decision keeps `.b` and `.q` off the arithmetic instructions.

### Carry and borrow

Flagless arithmetic still has to add numbers wider than a word, so the base carries an
explicit carry-chain pair. Both instructions name a **carry register** that is read as the
carry or borrow in and written as the carry or borrow out, which is the register-file
equivalent of the carry flag the machine does not have.

| Mnemonic | Operands | Semantics | Form |
|:---------|:---------|:----------|:-----|
| `add_carry` | `add_carry rs1 rs2 rc rd` | Writes rs1 plus rs2 plus bit 0 of rc into rd, then writes the unsigned carry-out (0 or 1) into rc. | `op r r r r` |
| `subtract_borrow` | `subtract_borrow rs1 rs2 rc rd` | Writes rs1 minus rs2 minus bit 0 of rc into rd, then writes the unsigned borrow-out (0 or 1) into rc. | `op r r r r` |

Three details make the pair usable and testable. Only bit 0 of the carry register is read,
and the remaining bits are ignored, so a carry register holding any value behaves
predictably. The sum is written to rd first and the carry-out to rc second, so when rd and
rc name the same register the carry-out is the value that survives. Naming r0 as the carry
register reads a carry-in of zero and discards the carry-out, which turns `add_carry` into a
plain add and gives a chain its natural first step; the more usual opening is to zero a real
carry register with `move r0 rc` so that the carry-out of the first limb is captured.

A three-limb unsigned addition of the limbs in r2 through r4 by the limbs in r5 through r7,
into r8 through r10, reads:

    move r0 r11
    add_carry r2 r5 r11 r8
    add_carry r3 r6 r11 r9
    add_carry r4 r7 r11 r10

### Immediate forms

| Mnemonic | Operands | Semantics | Form |
|:---------|:---------|:----------|:-----|
| `add` | `add rs $imm rd` | Writes rs plus the sign-extended 32-bit literal into rd. | `op r r i4` |
| `add.h` | `add.h rs $imm rd` | Writes the half-word sum with the literal, zero-extended, into rd. | `op r r i4` |
| `subtract` | `subtract rs $imm rd` | Writes rs minus the sign-extended 32-bit literal into rd. | `op r r i4` |
| `subtract.h` | `subtract.h rs $imm rd` | Writes the half-word difference with the literal, zero-extended, into rd. | `op r r i4` |
| `and` | `and rs $imm rd` | Writes the bitwise AND of rs with the sign-extended literal into rd. | `op r r i4` |
| `or` | `or rs $imm rd` | Writes the bitwise OR of rs with the sign-extended literal into rd. | `op r r i4` |
| `xor` | `xor rs $imm rd` | Writes the bitwise exclusive OR of rs with the sign-extended literal into rd. | `op r r i4` |
| `shift_left` | `shift_left rs #imm rd` | Writes rs shifted left by the 8-bit literal modulo 64 into rd. | `op r r i1` |
| `shift_left.h` | `shift_left.h rs #imm rd` | Writes the half-word left shift by the literal modulo 32, zero-extended, into rd. | `op r r i1` |
| `shift_right_logical` | `shift_right_logical rs #imm rd` | Writes rs shifted right by the literal modulo 64 with zero fill into rd. | `op r r i1` |
| `shift_right_logical.h` | `shift_right_logical.h rs #imm rd` | Writes the half-word logical right shift, zero-extended, into rd. | `op r r i1` |
| `shift_right_arithmetic` | `shift_right_arithmetic rs #imm rd` | Writes rs shifted right by the literal modulo 64 with sign fill into rd. | `op r r i1` |
| `shift_right_arithmetic.h` | `shift_right_arithmetic.h rs #imm rd` | Writes the half-word arithmetic right shift, zero-extended, into rd. | `op r r i1` |

## Compares

A compare writes 1 or 0 into a destination register. There is no condition register to read,
so a materialized condition is an ordinary value that any instruction can consume, and the
`select` pair and the branches consume it directly.

The base defines ten predicates, and every one of them exists in both the register-register
form and the immediate form. Completeness here is structural rather than incidental: the
same ten predicates serve the compares and the branches, so a compare and the branch that
tests the same relation can never disagree, and no relation is reachable from one family but
not the other. The immediate form compares against a 32-bit literal sign-extended to 64
bits, and the literal is always the second source, so `compare_lt_signed r4 $10 r5` sets r5
when r4 is signed-less-than 16.

| Mnemonic | Operands | Semantics | Form |
|:---------|:---------|:----------|:-----|
| `compare_eq` | `compare_eq rs1 rs2 rd` | Writes 1 into rd when rs1 equals rs2, otherwise 0. | `op r r r` |
| `compare_ne` | `compare_ne rs1 rs2 rd` | Writes 1 into rd when rs1 differs from rs2, otherwise 0. | `op r r r` |
| `compare_lt_signed` | `compare_lt_signed rs1 rs2 rd` | Writes 1 into rd when rs1 is signed-less-than rs2, otherwise 0. | `op r r r` |
| `compare_le_signed` | `compare_le_signed rs1 rs2 rd` | Writes 1 into rd when rs1 is signed-less-than-or-equal to rs2, otherwise 0. | `op r r r` |
| `compare_gt_signed` | `compare_gt_signed rs1 rs2 rd` | Writes 1 into rd when rs1 is signed-greater-than rs2, otherwise 0. | `op r r r` |
| `compare_ge_signed` | `compare_ge_signed rs1 rs2 rd` | Writes 1 into rd when rs1 is signed-greater-than-or-equal to rs2, otherwise 0. | `op r r r` |
| `compare_lt_unsigned` | `compare_lt_unsigned rs1 rs2 rd` | Writes 1 into rd when rs1 is unsigned-less-than rs2, otherwise 0. | `op r r r` |
| `compare_le_unsigned` | `compare_le_unsigned rs1 rs2 rd` | Writes 1 into rd when rs1 is unsigned-less-than-or-equal to rs2, otherwise 0. | `op r r r` |
| `compare_gt_unsigned` | `compare_gt_unsigned rs1 rs2 rd` | Writes 1 into rd when rs1 is unsigned-greater-than rs2, otherwise 0. | `op r r r` |
| `compare_ge_unsigned` | `compare_ge_unsigned rs1 rs2 rd` | Writes 1 into rd when rs1 is unsigned-greater-than-or-equal to rs2, otherwise 0. | `op r r r` |
| `compare_eq` | `compare_eq rs $imm rd` | Writes 1 into rd when rs equals the sign-extended literal, otherwise 0. | `op r r i4` |
| `compare_ne` | `compare_ne rs $imm rd` | Writes 1 into rd when rs differs from the literal, otherwise 0. | `op r r i4` |
| `compare_lt_signed` | `compare_lt_signed rs $imm rd` | Writes 1 into rd when rs is signed-less-than the literal, otherwise 0. | `op r r i4` |
| `compare_le_signed` | `compare_le_signed rs $imm rd` | Writes 1 into rd when rs is signed-less-than-or-equal to the literal, otherwise 0. | `op r r i4` |
| `compare_gt_signed` | `compare_gt_signed rs $imm rd` | Writes 1 into rd when rs is signed-greater-than the literal, otherwise 0. | `op r r i4` |
| `compare_ge_signed` | `compare_ge_signed rs $imm rd` | Writes 1 into rd when rs is signed-greater-than-or-equal to the literal, otherwise 0. | `op r r i4` |
| `compare_lt_unsigned` | `compare_lt_unsigned rs $imm rd` | Writes 1 into rd when rs is unsigned-less-than the literal, otherwise 0. | `op r r i4` |
| `compare_le_unsigned` | `compare_le_unsigned rs $imm rd` | Writes 1 into rd when rs is unsigned-less-than-or-equal to the literal, otherwise 0. | `op r r i4` |
| `compare_gt_unsigned` | `compare_gt_unsigned rs $imm rd` | Writes 1 into rd when rs is unsigned-greater-than the literal, otherwise 0. | `op r r i4` |
| `compare_ge_unsigned` | `compare_ge_unsigned rs $imm rd` | Writes 1 into rd when rs is unsigned-greater-than-or-equal to the literal, otherwise 0. | `op r r i4` |

The unsigned comparison of a value against the literal treats the sign-extended literal as
an unsigned 64-bit value, so `compare_lt_unsigned r4 $-1 r5` compares against
`$FFFFFFFFFFFFFFFF`. Comparing against zero is the common case and needs no literal at all,
because r0 supplies it: `compare_eq r4 r0 r5` sets r5 when r4 is zero.

## Branches and control transfer

Conditional control flow is a fused compare-and-branch, so nothing is carried between the
comparison and the transfer. The ten branch predicates are exactly the ten compare
predicates, tested on the same two register operands in the same order.

A branch, a jump, and a call all take a signed 32-bit displacement in bytes, measured from
the address of the instruction following the transfer. Targets are byte-granular and carry
no alignment requirement. The register forms of jump and call take an absolute address
rather than a displacement.

| Mnemonic | Operands | Semantics | Form |
|:---------|:---------|:----------|:-----|
| `branch_eq` | `branch_eq rs1 rs2 target` | Transfers to the target when rs1 equals rs2, otherwise falls through. | `op r r i4` |
| `branch_ne` | `branch_ne rs1 rs2 target` | Transfers when rs1 differs from rs2. | `op r r i4` |
| `branch_lt_signed` | `branch_lt_signed rs1 rs2 target` | Transfers when rs1 is signed-less-than rs2. | `op r r i4` |
| `branch_le_signed` | `branch_le_signed rs1 rs2 target` | Transfers when rs1 is signed-less-than-or-equal to rs2. | `op r r i4` |
| `branch_gt_signed` | `branch_gt_signed rs1 rs2 target` | Transfers when rs1 is signed-greater-than rs2. | `op r r i4` |
| `branch_ge_signed` | `branch_ge_signed rs1 rs2 target` | Transfers when rs1 is signed-greater-than-or-equal to rs2. | `op r r i4` |
| `branch_lt_unsigned` | `branch_lt_unsigned rs1 rs2 target` | Transfers when rs1 is unsigned-less-than rs2. | `op r r i4` |
| `branch_le_unsigned` | `branch_le_unsigned rs1 rs2 target` | Transfers when rs1 is unsigned-less-than-or-equal to rs2. | `op r r i4` |
| `branch_gt_unsigned` | `branch_gt_unsigned rs1 rs2 target` | Transfers when rs1 is unsigned-greater-than rs2. | `op r r i4` |
| `branch_ge_unsigned` | `branch_ge_unsigned rs1 rs2 target` | Transfers when rs1 is unsigned-greater-than-or-equal to rs2. | `op r r i4` |
| `jump` | `jump target` | Transfers to the target unconditionally. | `op i4` |
| `jump` | `jump rs` | Transfers to the absolute address in rs. | `op r` |
| `call` | `call target` | Writes the address of the following instruction into r31 and transfers to the target. | `op i4` |
| `call` | `call rs` | Reads the absolute target from rs, then writes the address of the following instruction into r31 and transfers. | `op r` |
| `return` | `return` | Transfers to the address in r31. | `op` |

The register form of `call` reads its target before it writes the link register, so
`call r31` is a well-defined call through the current link register. Branching against zero
needs no special instruction, since r0 supplies the zero: `branch_ne r4 r0 body` is the
nonzero test.

## Select

The conditional-move pair takes its condition from a register and is destructive in the
destination, which is exactly the host conditional-move contract on both translation
targets. The condition is a whole 64-bit value tested against zero, not a single bit, so a
compare result and any other nonzero value behave the same way.

| Mnemonic | Operands | Semantics | Form |
|:---------|:---------|:----------|:-----|
| `select_nz` | `select_nz rs rc rd` | Writes rs into rd when rc is nonzero, otherwise leaves rd unchanged. | `op r r r` |
| `select_z` | `select_z rs rc rd` | Writes rs into rd when rc is zero, otherwise leaves rd unchanged. | `op r r r` |

The general three-way select composes from the pair in two instructions: `move rf rd`
followed by `select_nz rt rc rd` yields the value of rt when rc is nonzero and of rf
otherwise.

## Loads and stores

Only the instructions in this family and the block-memory family read or write memory. Every
memory operand carries the `@` sigil, so a reader finds every memory access by scanning for
one character.

Two addressing forms exist. The bare form `@rN` uses the register's value as the address.
The displaced form `@rN+$disp` adds a signed 16-bit displacement to the register's value,
wrapping modulo 2^64, and the assembler also accepts `@rN-$disp` for a negative
displacement. There is no indexed form and no scaling in the base.

Access width and the extension rule live entirely in the mnemonic. A load names its access
width with `.b`, `.q`, or `.h`, or takes the full word with no suffix, and a narrow load
names its extension with `z` or `s` before the width letter. Every load writes the full
64-bit destination register; no load merges into a slice, and placing a loaded byte at an
interior position is deliberately a load followed by an `insert`. A store writes the low
bytes of its source register at the named width and reads nothing back.

Misaligned accesses are permitted at every width and raise no trap. A load or store may
raise a page fault, and when it does the destination register and memory are unmodified, so
the instruction re-executes cleanly after the fault is serviced.

| Mnemonic | Operands | Semantics | Form |
|:---------|:---------|:----------|:-----|
| `load` | `load @rb rd` | Reads a word from the address in rb into rd. | `op r r` |
| `load.zb` | `load.zb @rb rd` | Reads a byte and writes it zero-extended into rd. | `op r r` |
| `load.sb` | `load.sb @rb rd` | Reads a byte and writes it sign-extended into rd. | `op r r` |
| `load.zq` | `load.zq @rb rd` | Reads a quarter-word and writes it zero-extended into rd. | `op r r` |
| `load.sq` | `load.sq @rb rd` | Reads a quarter-word and writes it sign-extended into rd. | `op r r` |
| `load.zh` | `load.zh @rb rd` | Reads a half-word and writes it zero-extended into rd. | `op r r` |
| `load.sh` | `load.sh @rb rd` | Reads a half-word and writes it sign-extended into rd. | `op r r` |
| `load` | `load @rb+$disp rd` | Reads a word from rb plus the displacement into rd. | `op r r i2` |
| `load.zb` | `load.zb @rb+$disp rd` | Reads a byte from rb plus the displacement, zero-extended, into rd. | `op r r i2` |
| `load.sb` | `load.sb @rb+$disp rd` | Reads a byte from rb plus the displacement, sign-extended, into rd. | `op r r i2` |
| `load.zq` | `load.zq @rb+$disp rd` | Reads a quarter-word, zero-extended, into rd. | `op r r i2` |
| `load.sq` | `load.sq @rb+$disp rd` | Reads a quarter-word, sign-extended, into rd. | `op r r i2` |
| `load.zh` | `load.zh @rb+$disp rd` | Reads a half-word, zero-extended, into rd. | `op r r i2` |
| `load.sh` | `load.sh @rb+$disp rd` | Reads a half-word, sign-extended, into rd. | `op r r i2` |
| `store` | `store rs @rb` | Writes the word in rs to the address in rb. | `op r r` |
| `store.b` | `store.b rs @rb` | Writes the low byte of rs to the address in rb. | `op r r` |
| `store.q` | `store.q rs @rb` | Writes the low quarter-word of rs to the address in rb. | `op r r` |
| `store.h` | `store.h rs @rb` | Writes the low half-word of rs to the address in rb. | `op r r` |
| `store` | `store rs @rb+$disp` | Writes the word in rs to rb plus the displacement. | `op r r i2` |
| `store.b` | `store.b rs @rb+$disp` | Writes the low byte of rs to rb plus the displacement. | `op r r i2` |
| `store.q` | `store.q rs @rb+$disp` | Writes the low quarter-word of rs to rb plus the displacement. | `op r r i2` |
| `store.h` | `store.h rs @rb+$disp` | Writes the low half-word of rs to rb plus the displacement. | `op r r i2` |

Instruction fetch is coherent with stores. A program that writes bytes to memory and then
transfers to them executes what it wrote, with no cache-maintenance instruction and no
synchronization sequence, and the base spends no opcode on instruction-cache maintenance
because the architecture defines none to be needed.

## Extract and insert

The dotted positional forms are how software names a byte, a quarter-word, or a half-word
inside a register. A dotted **source** extracts, producing a fresh full-width value and
creating no dependency on the destination's previous contents. A dotted **destination**
inserts, and that read-modify-write is the only merge site in the instruction set.

All fourteen positional forms are reachable: eight bytes `rN.b0` through `rN.b7`, four
quarter-words `rN.q0` through `rN.q3`, and two half-words `rN.h0` and `rN.h1`. The element
width comes from the opcode and the element index from the operand byte's form field, as the
instruction-encoding chapter fixes. The general bitfield instructions sit behind the
positional forms with an immediate bit position and bit width, and the positional forms are
their aligned shorthands.

The boundary between the two notations is the invariant the register-model chapter states, and
the entries below depend on it rather than restating it.

| Mnemonic | Operands | Semantics | Form |
|:---------|:---------|:----------|:-----|
| `extract.zb` | `extract.zb rs.bN rd` | Writes byte N of rs zero-extended into rd. | `op r r` |
| `extract.sb` | `extract.sb rs.bN rd` | Writes byte N of rs sign-extended into rd. | `op r r` |
| `extract.zq` | `extract.zq rs.qN rd` | Writes quarter-word N of rs zero-extended into rd. | `op r r` |
| `extract.sq` | `extract.sq rs.qN rd` | Writes quarter-word N of rs sign-extended into rd. | `op r r` |
| `extract.zh` | `extract.zh rs.hN rd` | Writes half-word N of rs zero-extended into rd. | `op r r` |
| `extract.sh` | `extract.sh rs.hN rd` | Writes half-word N of rs sign-extended into rd. | `op r r` |
| `insert.b` | `insert.b rs rd.bN` | Writes the low byte of rs into byte N of rd, preserving the other bytes. | `op r r` |
| `insert.q` | `insert.q rs rd.qN` | Writes the low quarter-word of rs into quarter-word N of rd, preserving the rest. | `op r r` |
| `insert.h` | `insert.h rs rd.hN` | Writes the low half-word of rs into half-word N of rd, preserving the other half. | `op r r` |
| `bitfield_extract` | `bitfield_extract rs #pos #width rd` | Writes the width-bit field of rs starting at bit pos, zero-extended, into rd. | `op r r i1 i1` |
| `bitfield_extract_signed` | `bitfield_extract_signed rs #pos #width rd` | Writes that field sign-extended from its top bit into rd. | `op r r i1 i1` |
| `bitfield_insert` | `bitfield_insert rs #pos #width rd` | Writes the low width bits of rs into rd at bit pos, preserving every other bit of rd. | `op r r i1 i1` |

A bitfield instruction whose width is zero, or whose position plus width exceeds 64, raises
the illegal-operand trap. Neither condition has a defaulted interpretation, so a wrong
encoding stops rather than quietly producing a truncated field.

## Block memory

The block-memory instructions copy and fill regions of memory, and they exist as
instructions rather than syscalls because they are machine operations that a translator can
lower to a host memory routine. They replace the v1 syscall-provided copy and fill outright.

All three instructions name three registers, and all three are restartable under paging.
The restartability contract is normative and testable: at every point at which the machine
can be interrupted, the named registers hold the state of the operation as it stands, with
the pointer registers advanced past the bytes already transferred and the count register
holding the number of bytes not yet transferred. A page fault therefore leaves the registers
describing exactly the remaining work, the fault reports the address of the block
instruction itself, and re-executing that instruction after the fault is serviced completes
the operation with no byte copied twice and none skipped. On normal completion the count
register is zero and each pointer register points just past the last byte it touched.

| Mnemonic | Operands | Semantics | Form |
|:---------|:---------|:----------|:-----|
| `block_copy` | `block_copy @rs @rd rn` | Copies rn bytes from the address in rs to the address in rd, with the result defined for any overlap as though the source were read entirely before any write. | `op r r r` |
| `block_copy_forward` | `block_copy_forward @rs @rd rn` | Copies rn bytes ascending, byte by byte, so that an overlapping copy observes bytes already written. | `op r r r` |
| `block_set` | `block_set rv @rd rn` | Writes the low byte of rv to rn consecutive bytes starting at the address in rd. | `op r r r` |

Overlap is stated rather than implied. `block_copy` is the memmove-safe operation and gives
the same result for every overlap, including the fully overlapping case. `block_copy_forward`
is the cheaper operation a compiler emits for a non-overlapping copy, and its behavior under
overlap is still fully defined as the ascending byte-by-byte result rather than left
undefined. Because `block_copy` may transfer bytes in an implementation-chosen order, its
mid-operation register state is defined as the remaining-work description above and not as a
particular direction of travel; software that needs a specific direction uses
`block_copy_forward`.

A count of zero is valid, performs no access, and raises no fault. `block_set` leaves its
value register unmodified throughout.

The three register operands name three distinct registers. An encoding of `block_copy`,
`block_copy_forward`, or `block_set` that names the same register in more than one of its
three operand slots raises the illegal-operand trap, and the rule applies to r0 like any
other register because it constrains encodings rather than values. Aliased slots would make
the mid-operation restart state unrepresentable, since a register that has to be two of
pointer, counter, and fill value at once cannot describe the work that remains.

## Floating point

Floating-point values live in the ordinary registers, and the base defines no separate
floating-point register file, no floating-point load or store, and no floating-point move.
A binary64 value occupies the full word, and a binary32 value occupies the low half-word.
The dot on a floating-point mnemonic names the format: a bare mnemonic is binary64 and a
`.h` mnemonic is binary32.

A binary32 result is written zero-extended into the full 64-bit destination, which follows
the same half-word rule the integer instructions use. There is no NaN-boxing, and the upper
half of a binary32 result is zero rather than all ones.

The machine implements IEEE 754-2019 binary32 and binary64 with all five rounding modes, all
five sticky exception flags, subnormals with no flush-to-zero, and a single-rounded fused
multiply-add. The rounding mode and the sticky flags live in a control and status register
rather than in dedicated instructions, so software reads and writes them with `csr_read` and
`csr_write`. Every rounding operation consults the current rounding mode; a rounding
operation executed while the mode field holds a reserved encoding raises the illegal-operand
trap rather than silently rounding to nearest.

Arithmetic exceptions are sticky and never trap. A divide by zero yields the correctly
signed infinity and sets the divide-by-zero flag, an invalid operation yields the canonical
quiet NaN and sets the invalid flag, and the operation always produces its IEEE 754 defined
result. Only illegal encodings trap.

| Mnemonic | Operands | Semantics | Form |
|:---------|:---------|:----------|:-----|
| `float_add` | `float_add rs1 rs2 rd` | Writes the correctly rounded binary64 sum into rd. | `op r r r` |
| `float_add.h` | `float_add.h rs1 rs2 rd` | Writes the correctly rounded binary32 sum, zero-extended, into rd. | `op r r r` |
| `float_subtract` | `float_subtract rs1 rs2 rd` | Writes rs1 minus rs2 in binary64 into rd. | `op r r r` |
| `float_subtract.h` | `float_subtract.h rs1 rs2 rd` | Writes the binary32 difference, zero-extended, into rd. | `op r r r` |
| `float_multiply` | `float_multiply rs1 rs2 rd` | Writes the correctly rounded binary64 product into rd. | `op r r r` |
| `float_multiply.h` | `float_multiply.h rs1 rs2 rd` | Writes the correctly rounded binary32 product, zero-extended, into rd. | `op r r r` |
| `float_divide` | `float_divide rs1 rs2 rd` | Writes rs1 divided by rs2 in binary64 into rd. | `op r r r` |
| `float_divide.h` | `float_divide.h rs1 rs2 rd` | Writes the binary32 quotient, zero-extended, into rd. | `op r r r` |
| `float_square_root` | `float_square_root rs rd` | Writes the correctly rounded binary64 square root into rd. | `op r r` |
| `float_square_root.h` | `float_square_root.h rs rd` | Writes the binary32 square root, zero-extended, into rd. | `op r r` |
| `float_negate` | `float_negate rs rd` | Writes rs with its sign bit inverted into rd, exactly and without rounding. | `op r r` |
| `float_negate.h` | `float_negate.h rs rd` | Writes the binary32 value with its sign inverted, zero-extended, into rd. | `op r r` |
| `float_absolute` | `float_absolute rs rd` | Writes rs with its sign bit cleared into rd, exactly and without rounding. | `op r r` |
| `float_absolute.h` | `float_absolute.h rs rd` | Writes the binary32 value with its sign cleared, zero-extended, into rd. | `op r r` |
| `float_multiply_add` | `float_multiply_add rs1 rs2 rs3 rd` | Writes the single-rounded binary64 value of rs1 times rs2 plus rs3 into rd. | `op r r r r` |
| `float_multiply_add.h` | `float_multiply_add.h rs1 rs2 rs3 rd` | Writes the single-rounded binary32 result, zero-extended, into rd. | `op r r r r` |
| `float_multiply_subtract` | `float_multiply_subtract rs1 rs2 rs3 rd` | Writes the single-rounded binary64 value of rs1 times rs2 minus rs3 into rd. | `op r r r r` |
| `float_multiply_subtract.h` | `float_multiply_subtract.h rs1 rs2 rs3 rd` | Writes the single-rounded binary32 result, zero-extended, into rd. | `op r r r r` |
| `float_minimum` | `float_minimum rs1 rs2 rd` | Writes the smaller binary64 operand into rd, treating -0 as smaller than +0. | `op r r r` |
| `float_minimum.h` | `float_minimum.h rs1 rs2 rd` | Writes the smaller binary32 operand, zero-extended, into rd. | `op r r r` |
| `float_maximum` | `float_maximum rs1 rs2 rd` | Writes the larger binary64 operand into rd. | `op r r r` |
| `float_maximum.h` | `float_maximum.h rs1 rs2 rd` | Writes the larger binary32 operand, zero-extended, into rd. | `op r r r` |
| `float_compare_eq` | `float_compare_eq rs1 rs2 rd` | Writes 1 into rd when the operands are ordered and equal, otherwise 0. | `op r r r` |
| `float_compare_eq.h` | `float_compare_eq.h rs1 rs2 rd` | The binary32 ordered-equal compare. | `op r r r` |
| `float_compare_ne` | `float_compare_ne rs1 rs2 rd` | Writes 1 into rd when the operands are unordered or unequal, otherwise 0. | `op r r r` |
| `float_compare_ne.h` | `float_compare_ne.h rs1 rs2 rd` | The binary32 not-equal compare. | `op r r r` |
| `float_compare_lt` | `float_compare_lt rs1 rs2 rd` | Writes 1 into rd when rs1 is ordered and less than rs2, otherwise 0. | `op r r r` |
| `float_compare_lt.h` | `float_compare_lt.h rs1 rs2 rd` | The binary32 ordered less-than compare. | `op r r r` |
| `float_compare_le` | `float_compare_le rs1 rs2 rd` | Writes 1 into rd when rs1 is ordered and less than or equal to rs2, otherwise 0. | `op r r r` |
| `float_compare_le.h` | `float_compare_le.h rs1 rs2 rd` | The binary32 ordered less-or-equal compare. | `op r r r` |
| `float_compare_ordered` | `float_compare_ordered rs1 rs2 rd` | Writes 1 into rd when neither operand is a NaN, otherwise 0. | `op r r r` |
| `float_compare_ordered.h` | `float_compare_ordered.h rs1 rs2 rd` | The binary32 ordered test. | `op r r r` |
| `float_compare_unordered` | `float_compare_unordered rs1 rs2 rd` | Writes 1 into rd when either operand is a NaN, otherwise 0. | `op r r r` |
| `float_compare_unordered.h` | `float_compare_unordered.h rs1 rs2 rd` | The binary32 unordered test. | `op r r r` |
| `float_narrow` | `float_narrow rs rd` | Writes the binary64 value in rs converted to binary32, zero-extended, into rd. | `op r r` |
| `float_widen` | `float_widen rs rd` | Writes the binary32 value in the low half-word of rs converted to binary64 into rd. | `op r r` |
| `float_to_signed` | `float_to_signed rs rd` | Writes the binary64 value converted to a signed 64-bit integer into rd. | `op r r` |
| `float_to_signed.h` | `float_to_signed.h rs rd` | Writes the binary32 value converted to a signed 64-bit integer into rd. | `op r r` |
| `float_to_unsigned` | `float_to_unsigned rs rd` | Writes the binary64 value converted to an unsigned 64-bit integer into rd. | `op r r` |
| `float_to_unsigned.h` | `float_to_unsigned.h rs rd` | Writes the binary32 value converted to an unsigned 64-bit integer into rd. | `op r r` |
| `signed_to_float` | `signed_to_float rs rd` | Writes the signed 64-bit integer in rs converted to binary64 into rd. | `op r r` |
| `signed_to_float.h` | `signed_to_float.h rs rd` | Writes that integer converted to binary32, zero-extended, into rd. | `op r r` |
| `unsigned_to_float` | `unsigned_to_float rs rd` | Writes the unsigned 64-bit integer in rs converted to binary64 into rd. | `op r r` |
| `unsigned_to_float.h` | `unsigned_to_float.h rs rd` | Writes that integer converted to binary32, zero-extended, into rd. | `op r r` |

Four points fix the corners of this family.

- The compare family is complete by construction rather than by inspection. Six predicates
  cover the four IEEE relations plus the two NaN tests, and the greater-than and
  greater-or-equal relations are the less-than and less-or-equal instructions with the
  operands written in the other order, which is exact for every input including NaNs. There
  is no predicate reachable in one direction and missing in the other.
- Every compare is a quiet compare. A quiet NaN operand yields the unordered answer and
  raises no flag; only a signaling NaN raises the invalid flag. This includes the ordering
  predicates, which is a deliberate divergence from the RISC-V behavior where an ordering
  compare signals on a quiet NaN.
- Every NaN-producing arithmetic operation returns the canonical quiet NaN rather than
  propagating a payload. Negation and absolute value are the exceptions, since they touch
  only the sign bit, raise no flag, and pass payloads through unchanged.
- A float-to-integer conversion saturates: a value above the destination range yields the
  maximum, a value below yields the minimum, and both set the invalid flag. A NaN input
  yields zero and sets the invalid flag, matching the common C cast convention.

The negated fused operations are the fused operations followed by `float_negate`, which is
exact and therefore keeps the result single-rounded. Sign injection and classification are
integer bit operations on the register that already holds the value, so the base spends no
opcode on them.

## Control and status registers

The control and status registers hold every piece of architectural state that is not a
general register, including the trap-model state, the paging root, the floating-point
rounding mode and sticky flags, the feature bitmap, and the syscall-provider selection bit.
Two instructions reach the whole space, and the privileged-architecture chapter owns the
numbering and the meaning of each register.

| Mnemonic | Operands | Semantics | Form |
|:---------|:---------|:----------|:-----|
| `csr_read` | `csr_read $csr rd` | Writes the current value of the named control and status register into rd. | `op r i2` |
| `csr_write` | `csr_write rs $csr` | Writes the value in rs into the named control and status register. | `op r i2` |

The 16-bit register number carries its own access rules, so the machine can enforce them
without a lookup table. The layout of the number and the access rules it implies are owned
by the privileged-architecture chapter, which states them once for the whole specification.

## System and traps

The system family is small by design, because the trap model pushes a four-word frame in
hardware and leaves register saving to the kernel, and because state toggles that v1 spent
opcodes on are control and status register bits in v2.

| Mnemonic | Operands | Semantics | Form |
|:---------|:---------|:----------|:-----|
| `sys` | `sys #imm` | Enters the kernel through the syscall trap with the 8-bit literal as the syscall number. | `op i1` |
| `sys` | `sys rs` | Enters the kernel through the syscall trap with the low byte of rs as the syscall number. | `op r` |
| `trap_return` | `trap_return` | Pops the four-word trap frame and resumes the interrupted context. | `op` |
| `halt` | `halt` | Stops the machine. | `op` |
| `wait_for_interrupt` | `wait_for_interrupt` | Suspends execution until some cause is both pending and enabled, then continues at the following instruction. | `op` |
| `nop` | `nop` | Does nothing. | `op` |
| `breakpoint` | `breakpoint` | Raises the breakpoint trap. | `op` |

Five properties of this family are normative. `sys` is not privileged, because it is the
deliberate user-to-supervisor entry, and its arguments and result travel in the registers the
calling convention names, live across the trap boundary in both directions. `trap_return`,
`halt`, and `wait_for_interrupt` are privileged, and executing any of them at user level
raises the privileged-operation trap. `breakpoint` is trap-class, capturing the address of
the following instruction, so a debugger that resumes lands after it. `nop` is a real
instruction with a defined encoding rather than a reserved byte that happens to do nothing,
which is what lets a linker or a patcher pad safely. `halt` stops the machine outright,
while `wait_for_interrupt` idles a machine that expects an interrupt to arrive.

## TLB maintenance

Two operations maintain the translation cache, and both are privileged. A write to the
paging-root control and status register flushes the whole cache implicitly, so these
instructions exist for the case where a kernel edits a live page table without changing the
root.

| Mnemonic | Operands | Semantics | Form |
|:---------|:---------|:----------|:-----|
| `tlb_invalidate_all` | `tlb_invalidate_all` | Discards every cached translation. | `op` |
| `tlb_invalidate_address` | `tlb_invalidate_address rs` | Discards any cached translation for the page containing the virtual address in rs. | `op r` |

Both instructions are architecturally observable only through the translations that survive
them, and a machine that keeps no translation cache satisfies them by doing nothing. Neither
raises a fault for an address that has no cached translation.

## Port input and output

Devices live in a port space that is disjoint from the memory space, so no device register
is reachable through a load or a store. Both port instructions are privileged, and the port
identifier is the low quarter-word of the port register.

| Mnemonic | Operands | Semantics | Form |
|:---------|:---------|:----------|:-----|
| `port_in` | `port_in rp rd` | Reads a word from the port named by rp into rd. | `op r r` |
| `port_out` | `port_out rs rp` | Writes the word in rs to the port named by rp. | `op r r` |

A read from an unpopulated port yields zero and a write to an unpopulated port is discarded,
which keeps device probing defined on every machine regardless of which devices it carries.

## What the base deliberately leaves out

Each omission below is the consequence of a ratified decision, and none of them is an
oversight to be corrected by a later base revision, because the base freezes once.

- Condition flags, and with them the flag-reading conditional-set and conditional-jump
  families, are gone under the flagless decision; compares write registers and branches fuse
  their compare.
- The carry-flag controls that seeded a multi-precision chain are gone with the flags; the
  carry register of `add_carry` and `subtract_borrow` replaces them.
- Push, pop, and the fixed thirteen-register save and restore pair are gone under the
  register-file and trap-model decisions; the stack is an ABI convention, and the trap frame
  is four words pushed to the trap stack the trap model names in a control and status
  register.
- The sub-register file, and every narrow write that merged into a register, are gone under
  the register-file and extract-insert decisions; the positional forms name slices only on
  extract and insert.
- Memory operands on arithmetic and logic instructions are gone under the load-store
  requirement; only the load, store, and block-memory families touch memory.
- Byte and quarter-word arithmetic forms are gone under the width-modifier decision, which
  keeps `.h` as the only width-modified arithmetic because C promotes narrower types before
  arithmetic anyway.
- Register-driven bitfield position and width are excluded from the base under the
  extract-insert decision, pending evidence that a backend synthesizes them.
- A compressed 16-bit instruction form is excluded from the base under the encoding
  decision, and remains available as a future extension page if measurement ever justifies
  it.
- The syscall-provider select instructions are gone under the syscall-ABI decision, which
  makes the selection a control and status register bit.
- The interrupt-enable and interrupt-disable instructions are gone for the same reason, since
  a mode toggle does not earn an opcode when a control and status register bit expresses it.
- The software-interrupt instruction of v1, which never had a dispatch, is gone; `sys` plus
  the trap model cover deliberate entry into the kernel.
- The dedicated floating-point control-register instructions are gone under the privileged
  architecture decision, which generalizes that state into the numbered control and status
  register space.
- Rotates, count-leading-zeros, count-trailing-zeros, population count, integer minimum,
  maximum and absolute value, and bit test, set and clear stay out of the base because the
  codegen audit found no synthesized demand; each is an extension candidate the moment a
  workload produces evidence.
- Indexed addressing, in which a second register supplies an offset, stays out for the same
  reason.
- Atomic read-modify-write operations, compare-and-swap, and memory-ordering fences belong to
  the atomic extension under the extension-governance decision, since the base machine is
  single-hart and an ordering instruction with no observable effect would be a no-operation
  in disguise.
- A register-exchange instruction is absent, because three moves express it and the atomic
  extension owns the version that needs to be indivisible.
- An address-computation instruction in the v1 style is absent, because `add` with an
  immediate and `pc_add` cover both the frame-relative and the position-independent case.
- Instruction-cache maintenance is absent, because instruction fetch is defined coherent
  with stores.
