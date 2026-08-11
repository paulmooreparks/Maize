# Instruction Reference: Constants, Integer Arithmetic, and Compares

This chapter is normative. It gives the full reference for three families of the Maize v2
base: the constants and moves, the integer arithmetic and logic operations in all of their
register, unary, and immediate forms, and the ten compare predicates in both of their forms.
The instruction inventory summarizes these families in one line each; this chapter is where
their behavior is fixed in full, and where a conformance binary looks to find out what
result to expect from any input, including inputs a conventional machine would leave
undefined.

Nothing in this chapter reads or writes memory. Every instruction here reads zero, one, two,
or three general registers and writes one or two general registers, and control passes to
the following instruction in every case where the instruction does not trap.

## Reading an entry

Every entry carries the same five labels in the same order.

- **Syntax** gives every assembly form the mnemonic accepts, one per line, in
  source-to-destination order and without commas. A literal always carries its base marker,
  `#` for decimal, `$` for hexadecimal, and `%` for binary.
- **Encoding** names the length class from the instruction-encoding chapter and points at
  the band of the opcode-map appendix that holds the opcode bytes.
- **Operation** states what the machine computes and what it writes to every destination.
- **Traps** names every trap the instruction can raise during execution, or the single word
  None.
- **Example** shows the instruction in use, followed by one sentence saying what it did.

An entry covers one canonical mnemonic together with its width-modified and immediate forms,
because those forms differ only in operand shape and result width and share every other rule
in the family. Each of them has its own opcode byte, and the opcode-map appendix lists them
separately.

## Rules that hold for every instruction in this chapter

Six rules apply to all three families, and no entry restates them.

Register r0 reads as zero and discards writes. An instruction that names r0 as a destination
computes its result and then discards it, and an instruction that names r0 as a source reads
zero. This is what makes `move r0 rd` a clear-to-zero, `subtract r0 rs rd` a negation, and
`compare_eq rs r0 rd` a test against zero, with no dedicated opcode spent on any of them.

Every instruction in this chapter is unprivileged. The machine executes each of them
identically at user level and at supervisor level, and none of them raises the
privileged-operation trap.

Every operand slot in this chapter is a plain slot, so every operand byte carries a form
field of `%000`. An operand byte with any other form field raises the illegal-operand trap
before the instruction executes, exactly as the instruction-encoding chapter specifies. That
trap belongs to the decode stage and is therefore not repeated on the **Traps** line of any
entry, which names execution-stage traps only.

An instruction that traps writes nothing. When an entry names a trap, the destination
registers hold the values they held before the instruction, so the instruction is
restartable in the sense the trap-model chapter defines, and a handler that fixes the cause
and resumes at the faulting address gets a clean re-execution.

Arithmetic in this chapter is two's complement and wraps modulo the operation width.
Overflow is neither trapped nor recorded anywhere, because the machine has no condition
register; software that needs to detect an overflow compares the operands before the
operation or inspects the result after it.

Results are written whole. Every instruction in this chapter writes all 64 bits of its
destination register, and none of them merges into a slice of a register. Merging is the
business of the insert instructions, which the memory-and-fields reference chapter covers.

## Constants and moves

The instructions in this family write a register from another register, from a literal, or
from the program counter. None of them reads memory, none of them computes anything beyond
an extension or an addition to the program counter, and none of them can trap.

A narrow literal names its extension rule in the mnemonic, with `z` for zero-extension and
`s` for sign-extension, in the same spelling the loads use. The machine never applies a
default extension to a literal, because a reader who has to remember a default is a reader
who eventually remembers it wrong. The width letters follow the machine's vocabulary: `.b`
is the 8-bit byte, `.q` the 16-bit quarter-word, `.h` the 32-bit half-word, and `.w` the
full 64-bit word. Elsewhere in the base a bare mnemonic is the word-wide operation and no `.w`
exists, and the immediate move is the one place that spells the word width out, because a
bare immediate move would otherwise have to pick an encoding silently.

The assembler picks the encoding from the mnemonic alone. The bare `move` is the
register-to-register form and nothing else, and every immediate move names its width in a
length specifier, `move.w` for the full word and one of the six narrow forms below it. An
immediate move written without a width specifier is a syntax error. Because every literal
carries a mandatory base marker, a register operand and a literal operand are never
confusable, and neither the base marker nor the digit count of a literal ever carries width
information, so no encoding choice depends on the magnitude of the value. A literal that does
not fit the width its mnemonic names is a syntax error, never a truncation. Fit is the
assembler chapter's dual-reading test, so a field of N bits accepts any value from -2^(N-1)
through 2^N - 1 and the extension letter decides only what the machine does with the field
once it is loaded. That is why `move.sb $FF r3` is legal and leaves -1 in the destination.

### move

**Syntax:**

    move rs rd

**Encoding:** `op r r`; see the constants-and-moves band of the opcode-map appendix.

**Operation:** The machine writes all 64 bits of rs into rd and changes nothing else. When rs
and rd name the same register the machine still performs the write, and the register's value
is unchanged. The bare mnemonic is the register-to-register form only, so a literal operand
here is a syntax error rather than an implied width.

**Traps:** None.

**Example:**

    move r9 r4

r4 becomes the word held in r9.

### move.w

**Syntax:**

    move.w $imm rd

**Encoding:** `op r i8`; see the constants-and-moves band of the opcode-map appendix. This is
the longest instruction in the base, at ten bytes.

**Operation:** The machine writes the 64-bit literal, stored little-endian in the instruction,
into rd with no extension applied, since the literal is already a full word.

**Traps:** None.

**Example:**

    move.w $FFFFFFFFFFFFFFFF r7

r7 becomes the all-ones word.

### move.zb

**Syntax:**

    move.zb $imm rd

**Encoding:** `op r i1`; see the constants-and-moves band of the opcode-map appendix.

**Operation:** The machine writes the 8-bit literal into the low byte of rd and zero into
bits 63 through 8, so rd holds a value in the range 0 through 255.

**Traps:** None.

**Example:**

    move.zb $FF r3

r3 becomes `$00000000000000FF`.

### move.sb

**Syntax:**

    move.sb $imm rd

**Encoding:** `op r i1`; see the constants-and-moves band of the opcode-map appendix.

**Operation:** The machine takes bit 7 of the 8-bit literal as the sign, copies it into bits
63 through 8, and writes the literal into the low byte of rd, so rd holds a value in the
range -128 through 127.

**Traps:** None.

**Example:**

    move.sb $FF r3

r3 becomes `$FFFFFFFFFFFFFFFF`, which is -1.

### move.zq

**Syntax:**

    move.zq $imm rd

**Encoding:** `op r i2`; see the constants-and-moves band of the opcode-map appendix.

**Operation:** The machine writes the 16-bit quarter-word literal into the low quarter-word
of rd and zero into bits 63 through 16.

**Traps:** None.

**Example:**

    move.zq #1000 r4

r4 becomes 1000.

### move.sq

**Syntax:**

    move.sq $imm rd

**Encoding:** `op r i2`; see the constants-and-moves band of the opcode-map appendix.

**Operation:** The machine takes bit 15 of the 16-bit literal as the sign, copies it into
bits 63 through 16, and writes the literal into the low quarter-word of rd.

**Traps:** None.

**Example:**

    move.sq $8000 r4

r4 becomes `$FFFFFFFFFFFF8000`, which is -32768.

### move.zh

**Syntax:**

    move.zh $imm rd

**Encoding:** `op r i4`; see the constants-and-moves band of the opcode-map appendix.

**Operation:** The machine writes the 32-bit half-word literal into the low half-word of rd
and zero into bits 63 through 32. This is the form that materializes an unsigned 32-bit
constant in six bytes rather than the ten a full word would cost.

**Traps:** None.

**Example:**

    move.zh $DEADBEEF r5

r5 becomes `$00000000DEADBEEF`.

### move.sh

**Syntax:**

    move.sh $imm rd

**Encoding:** `op r i4`; see the constants-and-moves band of the opcode-map appendix.

**Operation:** The machine takes bit 31 of the 32-bit literal as the sign, copies it into
bits 63 through 32, and writes the literal into the low half-word of rd.

**Traps:** None.

**Example:**

    move.sh $FFFFFFF8 r5

r5 becomes `$FFFFFFFFFFFFFFF8`, which is -8.

### pc_add

**Syntax:**

    pc_add $imm rd

**Encoding:** `op r i4`; see the constants-and-moves band of the opcode-map appendix.

**Operation:** The machine sign-extends the 32-bit literal to 64 bits, adds it to the
address of the instruction following this one, and writes the sum into rd. The addition
wraps modulo 2^64. The machine does not translate, validate, or access the resulting
address, so a `pc_add` that names an unmapped or non-canonical address still completes and
still writes rd; a fault, if there is to be one, comes from the later load, store, or
transfer that uses the value. Because the instruction is six bytes long, `pc_add #0 rd`
writes the address of this instruction plus 6.

This is the only way software reads the program counter, since the program counter is not a
general register, and it is the base for position-independent addressing: the assembler
computes the literal from a label, and the linker adjusts it.

**Traps:** None.

**Example:**

    pc_add #0 r6

r6 becomes the address of the instruction immediately after the `pc_add`.

## Integer arithmetic and logic

Every instruction in this family reads one or two registers, or one register and a literal,
and writes one register; the carry pair also writes a second register. None of them touches
memory, and none of them produces any effect beyond the registers its entry names, because
there is no condition register for an arithmetic result to leak into.

Seven rules govern the whole family, and no entry below restates them.

**Half-word operations zero-extend.** A bare mnemonic operates on the full 64-bit word. A
`.h` mnemonic reads the low half-word of each of its sources, ignores their upper halves
entirely, computes a 32-bit result, and writes that result zero-extended into the full
64-bit destination, so bits 63 through 32 of the destination are always zero after a `.h`
operation. This matches what both translation hosts do natively, so a `.h` operation lowers
to a bare host instruction with no fixup. It also means that the value in a register after a
`.h` operation is not the sign-extended form of the 32-bit result, which matters when the
result is then compared; the compare family below states the idiom for that case.

**Width-modified forms exist for `.h` only.** No arithmetic or logic instruction has a `.b`
or `.q` form. Narrow widths live on the memory operations and on extract and insert, which
their own chapters cover, and C promotes narrower types before arithmetic anyway.

**Arithmetic wraps.** Addition, subtraction, multiplication, and negation wrap modulo the
operation width, which is 2^64 for a bare mnemonic and 2^32 for a `.h` mnemonic. No
arithmetic overflow raises a trap and none is recorded.

**Shift counts are masked.** A shift takes its count modulo the operation width, so a word
shift uses the low 6 bits of the count and a `.h` shift uses the low 5 bits. Every count
value is therefore defined, including counts of 64 and above and counts whose high bits are
set, and neither translation host needs a fixup. A shift-count literal is 8 bits, and
because the machine masks it before use, its signedness never arises.

**Division traps rather than approximating.** A `divide` or `remainder` instruction whose
divisor is zero raises the divide-error trap with the divide-by-zero subcode. A
`divide_signed` or `divide_signed.h` whose dividend is the most negative value of the
operation width and whose divisor is -1 raises the divide-error trap with the
quotient-overflow subcode, because the true quotient is not representable. Both subcodes are
reported in the trap frame's cause word as the trap-model chapter describes. No division
produces an approximated, saturated, or defaulted result, and no division that traps writes
its destination.

**The literal is always the second source.** In an immediate form the literal occupies the
second source position, so `subtract r1 $8 r3` computes r1 minus 8 and never 8 minus r1, and
`shift_left r1 #3 r3` shifts r1 left by 3 and never 3 left by r1. The reverse operation is
written with the literal in a register. In a three-operand register form the first named
source is likewise the left operand, so `subtract r1 r2 r3` computes r1 minus r2.

**Immediates are 32 bits, and shift counts are 8.** An arithmetic or logical immediate is a
32-bit literal sign-extended to 64 bits before the operation, and that includes the bitwise
operations, so `and rs $FFFFFFF0 rd` masks against `$FFFFFFFFFFFFFFF0`. In a `.h` immediate
form the 32-bit literal is the half-word operand directly and no extension is applied, since
the literal and the operation are the same width. A shift-count immediate is an 8-bit
literal, masked as described above.

### add

**Syntax:**

    add rs1 rs2 rd
    add.h rs1 rs2 rd
    add rs $imm rd
    add.h rs $imm rd

**Encoding:** `op r r r` for the register forms and `op r r i4` for the immediate forms; see
the integer arithmetic and logic band of the opcode-map appendix.

**Operation:** The machine writes the sum of the two sources into rd, wrapping modulo the
operation width. The word forms add all 64 bits and write all 64 bits. The `.h` forms add
the low half-words, discard any carry out of bit 31, and write the 32-bit sum zero-extended
into rd. In the immediate forms the second source is the sign-extended 32-bit literal for
the word form and the literal itself for the `.h` form.

**Traps:** None.

**Example:**

    add r4 $1000 r4

r4 increases by `$1000`, wrapping if it was near the top of the word.

### subtract

**Syntax:**

    subtract rs1 rs2 rd
    subtract.h rs1 rs2 rd
    subtract rs $imm rd
    subtract.h rs $imm rd

**Encoding:** `op r r r` for the register forms and `op r r i4` for the immediate forms; see
the integer arithmetic and logic band of the opcode-map appendix.

**Operation:** The machine subtracts the second source from the first and writes the
difference into rd, wrapping modulo the operation width. The `.h` forms subtract the low
half-words and write the 32-bit difference zero-extended. Subtracting a literal is the same
operation as adding its negation, and the assembler does not rewrite one into the other,
because the two spellings are different instructions with different opcodes.

**Traps:** None.

**Example:**

    subtract r30 #32 r30

The stack pointer moves down 32 bytes.

### multiply

**Syntax:**

    multiply rs1 rs2 rd
    multiply.h rs1 rs2 rd

**Encoding:** `op r r r`; see the integer arithmetic and logic band of the opcode-map
appendix.

**Operation:** The machine forms the full product of its two sources and writes the low part
into rd, discarding the high part. The word form writes the low 64 bits of the 128-bit
product, and the `.h` form writes the low 32 bits of the 64-bit product of the two low
half-words, zero-extended. The low half of a product is identical whether the operands are
read as signed or as unsigned, so the base spends no opcode on a signed and an unsigned
variant of this instruction; the distinction appears only in the high-half instructions
below.

**Traps:** None.

**Example:**

    multiply r2 r3 r4

r4 becomes the low word of r2 times r3.

### multiply_high_signed

**Syntax:**

    multiply_high_signed rs1 rs2 rd

**Encoding:** `op r r r`; see the integer arithmetic and logic band of the opcode-map
appendix.

**Operation:** The machine reads both sources as signed 64-bit values, forms the exact
128-bit product, and writes bits 127 through 64 of that product into rd. Together with
`multiply` on the same operands this yields the full 128-bit signed product in two
instructions. There is no `.h` form, because the full product of two half-words fits in a
word and `multiply` already delivers it when the operands have been extended.

**Traps:** None.

**Example:**

    multiply_high_signed r2 r3 r5

r5 becomes the high word of the signed 128-bit product of r2 and r3.

### multiply_high_unsigned

**Syntax:**

    multiply_high_unsigned rs1 rs2 rd

**Encoding:** `op r r r`; see the integer arithmetic and logic band of the opcode-map
appendix.

**Operation:** The machine reads both sources as unsigned 64-bit values, forms the exact
128-bit product, and writes bits 127 through 64 of that product into rd. This is the
instruction a bignum multiply and a reciprocal division sequence are built on.

**Traps:** None.

**Example:**

    multiply_high_unsigned r2 r3 r5

r5 becomes the high word of the unsigned 128-bit product of r2 and r3.

### divide_signed

**Syntax:**

    divide_signed rs1 rs2 rd
    divide_signed.h rs1 rs2 rd

**Encoding:** `op r r r`; see the integer arithmetic and logic band of the opcode-map
appendix.

**Operation:** The machine reads both sources as signed values of the operation width,
divides the first by the second, truncates the quotient toward zero, and writes it into rd.
Truncation toward zero means that -7 divided by 2 yields -3 and not -4, which is what C
requires. The `.h` form divides the low half-words and writes the 32-bit quotient
zero-extended, so a negative half-word quotient appears in rd with bits 63 through 32 clear.

**Traps:** The divide-error trap, with the divide-by-zero subcode when the divisor is zero
and the quotient-overflow subcode when the dividend is the most negative value of the
operation width and the divisor is -1. In either case rd is unmodified.

**Example:**

    divide_signed r2 r3 r4

r4 becomes r2 divided by r3, truncated toward zero, unless r3 is zero.

### divide_unsigned

**Syntax:**

    divide_unsigned rs1 rs2 rd
    divide_unsigned.h rs1 rs2 rd

**Encoding:** `op r r r`; see the integer arithmetic and logic band of the opcode-map
appendix.

**Operation:** The machine reads both sources as unsigned values of the operation width,
divides the first by the second, and writes the quotient into rd. An unsigned quotient is
always representable, so this instruction has no overflow case. The `.h` form divides the
low half-words and writes the 32-bit quotient zero-extended.

**Traps:** The divide-error trap with the divide-by-zero subcode when the divisor is zero,
in which case rd is unmodified.

**Example:**

    move.zb $A r3
    divide_unsigned r2 r3 r4

The divisor is materialized in r3 first, because this instruction has no immediate form, and
r4 becomes r2 divided by 10 as an unsigned value.

### remainder_signed

**Syntax:**

    remainder_signed rs1 rs2 rd
    remainder_signed.h rs1 rs2 rd

**Encoding:** `op r r r`; see the integer arithmetic and logic band of the opcode-map
appendix.

**Operation:** The machine reads both sources as signed values of the operation width and
writes into rd the remainder that pairs with the truncated-toward-zero quotient, which is
the dividend minus the divisor times that quotient. The remainder therefore takes the sign
of the dividend, so -7 remainder 2 yields -1. When the dividend is the most negative value
of the operation width and the divisor is -1 the machine writes zero into rd and raises no
trap, because that remainder is exactly zero and is representable even though the
corresponding quotient is not. The `.h` form works on the low half-words and writes the
32-bit remainder zero-extended.

**Traps:** The divide-error trap with the divide-by-zero subcode when the divisor is zero,
in which case rd is unmodified.

**Example:**

    remainder_signed r2 r3 r4

r4 becomes the remainder of r2 divided by r3, carrying the sign of r2.

### remainder_unsigned

**Syntax:**

    remainder_unsigned rs1 rs2 rd
    remainder_unsigned.h rs1 rs2 rd

**Encoding:** `op r r r`; see the integer arithmetic and logic band of the opcode-map
appendix.

**Operation:** The machine reads both sources as unsigned values of the operation width and
writes the remainder of the first divided by the second into rd. The result is always less
than the divisor. The `.h` form works on the low half-words and writes the 32-bit remainder
zero-extended.

**Traps:** The divide-error trap with the divide-by-zero subcode when the divisor is zero,
in which case rd is unmodified.

**Example:**

    move.zb $10 r3
    remainder_unsigned r2 r3 r4

The divisor is materialized in r3 first, because this instruction has no immediate form, and
r4 becomes the low four bits of r2, which is r2 modulo 16.

### and

**Syntax:**

    and rs1 rs2 rd
    and rs $imm rd

**Encoding:** `op r r r` for the register form and `op r r i4` for the immediate form; see
the integer arithmetic and logic band of the opcode-map appendix.

**Operation:** The machine writes the bitwise AND of its two sources into rd, bit by bit
across all 64 bits. The immediate form ANDs against the sign-extended 32-bit literal, so a
mask whose high bits are all ones is written as a negative-looking literal and reaches the
whole word. There is no `.h` form, because a bitwise operation on the low half alone is the
word operation with a masked literal and buys nothing.

**Traps:** None.

**Example:**

    and r2 $FF r3

r3 becomes the low byte of r2 with every higher bit cleared.

### or

**Syntax:**

    or rs1 rs2 rd
    or rs $imm rd

**Encoding:** `op r r r` for the register form and `op r r i4` for the immediate form; see
the integer arithmetic and logic band of the opcode-map appendix.

**Operation:** The machine writes the bitwise inclusive OR of its two sources into rd across
all 64 bits. The immediate form ORs against the sign-extended 32-bit literal.

**Traps:** None.

**Example:**

    or r2 %1 r2

Bit 0 of r2 is set and every other bit keeps its value.

### xor

**Syntax:**

    xor rs1 rs2 rd
    xor rs $imm rd

**Encoding:** `op r r r` for the register form and `op r r i4` for the immediate form; see
the integer arithmetic and logic band of the opcode-map appendix.

**Operation:** The machine writes the bitwise exclusive OR of its two sources into rd across
all 64 bits. The immediate form exclusive-ORs against the sign-extended 32-bit literal, so
`xor rs $-1 rd` is a complement and reaches the same result `not` does.

**Traps:** None.

**Example:**

    xor r2 r2 r2

r2 becomes zero, which is the idiomatic self-clear when the destination is also the source.

### shift_left

**Syntax:**

    shift_left rs1 rs2 rd
    shift_left.h rs1 rs2 rd
    shift_left rs #imm rd
    shift_left.h rs #imm rd

**Encoding:** `op r r r` for the register forms and `op r r i1` for the immediate forms; see
the integer arithmetic and logic band of the opcode-map appendix.

**Operation:** The machine shifts the first source left by the masked count, filling the
vacated low bits with zero, and writes the result into rd. Bits shifted out of the top are
discarded. The word forms mask the count to 6 bits and shift a 64-bit value; the `.h` forms
mask the count to 5 bits, shift the low half-word, and write the 32-bit result
zero-extended. In the register forms the count comes from the low bits of the second source
register, whose remaining bits the machine ignores.

**Traps:** None.

**Example:**

    shift_left r2 #3 r3

r3 becomes r2 times 8.

### shift_right_logical

**Syntax:**

    shift_right_logical rs1 rs2 rd
    shift_right_logical.h rs1 rs2 rd
    shift_right_logical rs #imm rd
    shift_right_logical.h rs #imm rd

**Encoding:** `op r r r` for the register forms and `op r r i1` for the immediate forms; see
the integer arithmetic and logic band of the opcode-map appendix.

**Operation:** The machine shifts the first source right by the masked count, filling the
vacated high bits with zero, and writes the result into rd. Bits shifted out of the bottom
are discarded. The word forms shift a 64-bit value with a 6-bit count; the `.h` forms shift
the low half-word with a 5-bit count, fill from bit 31 downward with zero, and write the
32-bit result zero-extended, so the upper half of rd is zero for two independent reasons.

**Traps:** None.

**Example:**

    shift_right_logical r2 #32 r3

r3 becomes the upper half-word of r2, zero-extended.

### shift_right_arithmetic

**Syntax:**

    shift_right_arithmetic rs1 rs2 rd
    shift_right_arithmetic.h rs1 rs2 rd
    shift_right_arithmetic rs #imm rd
    shift_right_arithmetic.h rs #imm rd

**Encoding:** `op r r r` for the register forms and `op r r i1` for the immediate forms; see
the integer arithmetic and logic band of the opcode-map appendix.

**Operation:** The machine shifts the first source right by the masked count, filling the
vacated high bits with copies of the source's sign bit, and writes the result into rd. The
word forms replicate bit 63 and use a 6-bit count. The `.h` forms replicate bit 31 of the
low half-word, use a 5-bit count, and write the 32-bit result zero-extended into rd, so the
sign fill appears inside the low half-word while bits 63 through 32 of rd are zero.

**Traps:** None.

**Example:**

    shift_right_arithmetic r2 #1 r3

r3 becomes r2 divided by 2 rounded toward negative infinity, which differs from
`divide_signed` by 1 for a negative odd dividend.

### not

**Syntax:**

    not rs rd
    not.h rs rd

**Encoding:** `op r r`; see the integer arithmetic and logic band of the opcode-map
appendix.

**Operation:** The machine inverts every bit of the source and writes the result into rd.
The word form inverts all 64 bits. The `.h` form inverts the 32 bits of the low half-word
and writes that result zero-extended, so bits 63 through 32 of rd are zero rather than the
inverted upper half of the source.

**Traps:** None.

**Example:**

    not r2 r3

r3 becomes the one's complement of r2.

### negate

**Syntax:**

    negate rs rd
    negate.h rs rd

**Encoding:** `op r r`; see the integer arithmetic and logic band of the opcode-map
appendix.

**Operation:** The machine subtracts the source from zero at the operation width and writes
the difference into rd. Negating the most negative value of the width yields that same value
back, since the result wraps and the positive counterpart is not representable, and the
machine raises no trap for it. The `.h` form negates the low half-word and writes the 32-bit
result zero-extended.

**Traps:** None.

**Example:**

    negate r2 r3

r3 becomes the two's complement negation of r2.

### byte_reverse

**Syntax:**

    byte_reverse rs rd
    byte_reverse.h rs rd

**Encoding:** `op r r`; see the integer arithmetic and logic band of the opcode-map
appendix.

**Operation:** The word form writes the eight bytes of the source in reverse order into rd,
so byte 0 of the source becomes byte 7 of the result and byte 7 becomes byte 0. The `.h`
form reverses the four bytes of the source's low half-word and writes that 32-bit result
zero-extended into rd, ignoring the source's upper half. Neither form reverses bits within a
byte. This is the byte-order conversion the networking path needs on every packet header,
and reversing a quarter-word is `byte_reverse.h` followed by a right shift of 16, or an
extract, since the arithmetic family carries no `.q` form.

**Traps:** None.

**Example:**

    byte_reverse.h r2 r3

r3 becomes the low half-word of r2 with its four bytes in the opposite order, zero-extended.

### Carry and borrow

Flagless arithmetic still has to add and subtract numbers wider than a word, so the base
carries an explicit carry-chain pair. Both instructions name a **carry register** that the
machine reads as the carry or borrow coming in and writes as the carry or borrow going out.
That register is an ordinary general register, and it is the register-file equivalent of the
carry flag this machine does not have.

Four rules fix the contract, and both entries below rely on them.

- Only bit 0 of the carry register is read. Bits 63 through 1 are ignored on the way in, so
  a carry register holding any value at all behaves predictably.
- The carry-out written on the way out is 0 or 1 in the whole 64-bit register, with bits 63
  through 1 cleared. A carry register is therefore always in canonical form after the first
  instruction of a chain writes it.
- The machine writes rd first and rc second. When rd and rc name the same register the
  carry-out is the value that survives, and the sum is lost.
- Naming r0 as the carry register reads a carry-in of zero and discards the carry-out, which
  degenerates `add_carry` into a plain add and `subtract_borrow` into a plain subtract.

Neither instruction has a `.h` form, because a multi-precision chain is built from
full-width limbs.

A chain opens by putting a zero carry-in in place. Naming r0 as the carry register does that
for the first limb but throws away the carry-out that the second limb needs, so the usual
opening is to clear a real carry register with `move r0 rc` and then run every limb through
the same register. A three-limb unsigned addition of the limbs in r2 through r4 by the limbs
in r5 through r7, into r8 through r10, reads:

    move r0 r11
    add_carry r2 r5 r11 r8
    add_carry r3 r6 r11 r9
    add_carry r4 r7 r11 r10

After the last instruction r11 holds the carry out of the whole 192-bit addition, which is
the overflow indicator for the wide operation.

### add_carry

**Syntax:**

    add_carry rs1 rs2 rc rd

**Encoding:** `op r r r r`; see the integer arithmetic and logic band of the opcode-map
appendix.

**Operation:** The machine computes the 65-bit unsigned sum of rs1, rs2, and bit 0 of rc. It
writes the low 64 bits of that sum into rd, then writes bit 64 of the sum, which is 0 or 1,
into rc. Both writes happen, in that order, and the ordering is what makes an rd that names
the same register as rc yield the carry-out. The carry-out is the unsigned carry, so it is 1
exactly when the true sum exceeds `$FFFFFFFFFFFFFFFF`, and it says nothing about signed
overflow.

**Traps:** None.

**Example:**

    add_carry r3 r6 r11 r9

r9 becomes the middle limb of the sum and r11 becomes the carry into the limb above it.

### subtract_borrow

**Syntax:**

    subtract_borrow rs1 rs2 rc rd

**Encoding:** `op r r r r`; see the integer arithmetic and logic band of the opcode-map
appendix.

**Operation:** The machine computes rs1 minus rs2 minus bit 0 of rc. It writes the low 64
bits of that difference into rd, then writes the borrow-out into rc, where the borrow-out is
1 when the true difference is negative as an unsigned computation, meaning that rs2 plus the
incoming borrow exceeds rs1, and 0 otherwise. Both writes happen, in that order.

**Traps:** None.

**Example:**

    subtract_borrow r3 r6 r11 r9

r9 becomes the middle limb of the difference and r11 becomes the borrow into the limb above
it.

## Compares

A compare evaluates a relation between two values and writes 1 or 0 into a general register.
There is no condition register to read, so a materialized condition is an ordinary value
that any instruction can consume: the select pair tests it, a branch can test it against r0,
and arithmetic can add it.

Five rules govern the family, and no entry below restates them.

The destination is written whole. The machine writes 1 or 0 into all 64 bits of rd, so bits
63 through 1 are always zero after a compare. A compare result is therefore already a
canonical boolean, safe to use as a mask after negation and safe to add into a counter.

The base defines ten predicates and every one of them exists in both forms. Completeness
here is structural rather than incidental: the same ten predicates serve the compares and
the branches, tested on the same two operands in the same order, so a compare and the branch
that tests the same relation can never disagree, and no relation is reachable from one
family but not the other.

The literal is always the second source. In an immediate form the machine sign-extends the
32-bit literal to 64 bits and compares the register against it in that position, so
`compare_lt_signed r4 $10 r5` asks whether r4 is less than 16 and never the reverse. The
reversed relation is available by name, since the family carries both directions of every
ordering.

An unsigned comparison against a literal treats the sign-extended literal as an unsigned
64-bit value. The literal is sign-extended first and then read as unsigned, so
`compare_lt_unsigned r4 $-1 r5` compares against `$FFFFFFFFFFFFFFFF` and sets r5 for every
value of r4 except that one. This gives access to the top of the unsigned range from a
32-bit literal, and a small unsigned constant is written plainly because sign extension
leaves it unchanged.

Compares operate on the full word only, with no `.h` forms. Comparing values that are
already zero-extended half-words works directly for the unsigned predicates, because a
zero-extended half-word is its own unsigned word value. A signed comparison of half-word
values needs the operands sign-extended into the word first, with `extract.sh` or
`bitfield_extract_signed`, because `.h` arithmetic zero-extends its result and a negative
32-bit value therefore appears in the register as a large positive word. This is the
width-consistency requirement the width-modifier decision names, and it is a compiler
obligation rather than a machine behavior. The cost is real and worth naming. A signed
comparison of two half-word values is three instructions here, one extend per operand plus the
compare, where a host with half-word compares spends one. The base pays that on the operations
that are narrow rather than carrying a second comparison width, and the widening stays visible
in the instruction stream where a reader can see what it cost.

The ten entries below share one shape, so their Operation lines differ only in the relation
they name.

### compare_eq

**Syntax:**

    compare_eq rs1 rs2 rd
    compare_eq rs $imm rd

**Encoding:** `op r r r` for the register form and `op r r i4` for the immediate form; see
the compares band of the opcode-map appendix.

**Operation:** The machine writes 1 into rd when the two 64-bit source values are equal and
0 when they differ. Equality does not depend on signedness, so this predicate has no signed
and unsigned pair.

**Traps:** None.

**Example:**

    compare_eq r4 r0 r5

r5 becomes 1 when r4 is zero, which is the idiomatic test against zero.

### compare_ne

**Syntax:**

    compare_ne rs1 rs2 rd
    compare_ne rs $imm rd

**Encoding:** `op r r r` for the register form and `op r r i4` for the immediate form; see
the compares band of the opcode-map appendix.

**Operation:** The machine writes 1 into rd when the two 64-bit source values differ and 0
when they are equal.

**Traps:** None.

**Example:**

    compare_ne r4 $2A r5

r5 becomes 1 unless r4 holds 42.

### compare_lt_signed

**Syntax:**

    compare_lt_signed rs1 rs2 rd
    compare_lt_signed rs $imm rd

**Encoding:** `op r r r` for the register form and `op r r i4` for the immediate form; see
the compares band of the opcode-map appendix.

**Operation:** The machine reads both sources as signed 64-bit two's complement values and
writes 1 into rd when the first is less than the second, otherwise 0.

**Traps:** None.

**Example:**

    compare_lt_signed r4 r0 r5

r5 becomes 1 when r4 is negative.

### compare_le_signed

**Syntax:**

    compare_le_signed rs1 rs2 rd
    compare_le_signed rs $imm rd

**Encoding:** `op r r r` for the register form and `op r r i4` for the immediate form; see
the compares band of the opcode-map appendix.

**Operation:** The machine reads both sources as signed 64-bit two's complement values and
writes 1 into rd when the first is less than or equal to the second, otherwise 0.

**Traps:** None.

**Example:**

    compare_le_signed r4 r0 r5

r5 becomes 1 when r4 is negative or zero.

### compare_gt_signed

**Syntax:**

    compare_gt_signed rs1 rs2 rd
    compare_gt_signed rs $imm rd

**Encoding:** `op r r r` for the register form and `op r r i4` for the immediate form; see
the compares band of the opcode-map appendix.

**Operation:** The machine reads both sources as signed 64-bit two's complement values and
writes 1 into rd when the first is greater than the second, otherwise 0. The predicate
exists in its own right rather than as an operand swap, so that an immediate form can put
the literal on the right where the family requires it.

**Traps:** None.

**Example:**

    compare_gt_signed r4 #100 r5

r5 becomes 1 when r4 exceeds 100 as a signed value.

### compare_ge_signed

**Syntax:**

    compare_ge_signed rs1 rs2 rd
    compare_ge_signed rs $imm rd

**Encoding:** `op r r r` for the register form and `op r r i4` for the immediate form; see
the compares band of the opcode-map appendix.

**Operation:** The machine reads both sources as signed 64-bit two's complement values and
writes 1 into rd when the first is greater than or equal to the second, otherwise 0.

**Traps:** None.

**Example:**

    compare_ge_signed r4 r0 r5

r5 becomes 1 when r4 is zero or positive.

### compare_lt_unsigned

**Syntax:**

    compare_lt_unsigned rs1 rs2 rd
    compare_lt_unsigned rs $imm rd

**Encoding:** `op r r r` for the register form and `op r r i4` for the immediate form; see
the compares band of the opcode-map appendix.

**Operation:** The machine reads both sources as unsigned 64-bit values and writes 1 into rd
when the first is less than the second, otherwise 0.

**Traps:** None.

**Example:**

    compare_lt_unsigned r4 r7 r5

r5 becomes 1 when r4 is below r7 as an unsigned value, which is the bounds check an index
against a length wants.

### compare_le_unsigned

**Syntax:**

    compare_le_unsigned rs1 rs2 rd
    compare_le_unsigned rs $imm rd

**Encoding:** `op r r r` for the register form and `op r r i4` for the immediate form; see
the compares band of the opcode-map appendix.

**Operation:** The machine reads both sources as unsigned 64-bit values and writes 1 into rd
when the first is less than or equal to the second, otherwise 0.

**Traps:** None.

**Example:**

    compare_le_unsigned r4 $FF r5

r5 becomes 1 when r4 fits in a byte.

### compare_gt_unsigned

**Syntax:**

    compare_gt_unsigned rs1 rs2 rd
    compare_gt_unsigned rs $imm rd

**Encoding:** `op r r r` for the register form and `op r r i4` for the immediate form; see
the compares band of the opcode-map appendix.

**Operation:** The machine reads both sources as unsigned 64-bit values and writes 1 into rd
when the first is greater than the second, otherwise 0.

**Traps:** None.

**Example:**

    compare_gt_unsigned r4 r0 r5

r5 becomes 1 when r4 is nonzero.

### compare_ge_unsigned

**Syntax:**

    compare_ge_unsigned rs1 rs2 rd
    compare_ge_unsigned rs $imm rd

**Encoding:** `op r r r` for the register form and `op r r i4` for the immediate form; see
the compares band of the opcode-map appendix.

**Operation:** The machine reads both sources as unsigned 64-bit values and writes 1 into rd
when the first is greater than or equal to the second, otherwise 0. Comparing any value
against zero with this predicate always yields 1, which is a true statement rather than a
special case.

**Traps:** None.

**Example:**

    compare_ge_unsigned r4 $1000 r5

r5 becomes 1 when r4 is at least `$1000` as an unsigned value.

## Conformance notes

The properties below are directly testable by a binary, and a conforming machine exhibits
all of them.

- Every `.h` instruction in this chapter leaves bits 63 through 32 of its destination zero,
  for every input, including inputs whose 32-bit result has bit 31 set.
- A shift by a count of 64 through 255 produces the same result as a shift by that count
  modulo 64 in the word forms, and the same relation holds modulo 32 in the `.h` forms.
- Every divide and remainder instruction with a zero divisor raises the divide-error trap
  with the divide-by-zero subcode, and leaves its destination register holding its prior
  value.
- `divide_signed` of the most negative word by -1 raises the divide-error trap with the
  quotient-overflow subcode, while `remainder_signed` on the same operands writes zero and
  raises nothing.
- A carry chain of any length built from `add_carry` over a single carry register produces
  the same limbs as the arbitrary-precision sum of the operands, and the final carry
  register holds 1 exactly when the wide sum overflowed.
- An instruction in this chapter naming r0 as its destination changes no architectural
  state, and an `add_carry` naming r0 as both destination and carry register changes none
  either.
- Every compare writes exactly 1 or exactly 0, never any other value, for every pair of
  inputs.
