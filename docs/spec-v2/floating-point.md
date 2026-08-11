# Floating Point

This chapter is normative. It fixes the floating-point model of the Maize v2 base: where
floating-point values live, which parts of IEEE 754-2019 the machine implements, how the
rounding mode and the sticky exception flags are held and reached, what a comparison answers
when an operand is a NaN, what a NaN-producing operation returns, and how a conversion
behaves at the edges of the integer range. It then gives the full reference entry for each of
the forty-four floating-point instructions.

The instruction inventory summarizes this family in one table per its own conventions. This
chapter is the authority on the detail: where the two disagree, the inventory is the summary
and this chapter states the semantics a conformance binary tests.

## Where floating-point values live

Floating-point values occupy the ordinary general registers. The base defines no separate
floating-point register file, no floating-point load, no floating-point store, and no
floating-point move, because `load`, `store`, and `move` already carry the bits and a
floating-point value is a bit pattern like any other.

A binary64 value occupies the whole 64-bit word of a register. A binary32 value occupies the
low half-word, bits 31 through 0. The format is named by the mnemonic and by nothing else: a
bare floating-point mnemonic operates on binary64 and a `.h` mnemonic operates on binary32.
No operand byte carries a format field, no register carries a format tag, and no machine
state selects a format.

A binary32 operation ignores bits 63 through 32 of each of its source registers entirely. It
reads the low half-word, computes in binary32, and writes the 32-bit result zero-extended
into the full 64-bit destination, which is the same rule the `.h` integer arithmetic follows.
There is no NaN-boxing: the upper half of a binary32 result is zero and never all ones, a
source register whose upper half holds anything at all is a valid binary32 operand, and no
encoding of the upper half raises a trap or sets a flag.

Two consequences are worth stating because software depends on them. Every binary32 operation
is total over all 2^64 source patterns, so a conformance binary can drive arbitrary garbage
into the upper halves and observe results identical to the same test with zeroed upper halves.
A binary32 value written by any instruction in this chapter compares equal, as an integer, to
its own 32-bit encoding, so integer instructions can classify, copy, and mask floating-point
values without a conversion step.

## Conformance scope

The machine implements IEEE 754-2019 binary32 and binary64 arithmetic for the operations this
chapter defines. Within that scope the implementation is complete rather than approximate.

- All five rounding directions are supported, and every rounding operation consults the
  current one.
- All five sticky exception flags are raised exactly when 754 requires them.
- Subnormal operands and subnormal results are computed with gradual underflow, and no
  operation flushes a subnormal to zero in any rounding mode.
- The fused operations round once, on the exact product-plus-addend, and never twice.
- Addition, subtraction, multiplication, division, square root, and the format narrowing are
  correctly rounded, which is to say each returns the representable value nearest the exact
  mathematical result under the current rounding direction.

Four parts of the standard are outside the base, and each is outside it because a ratified
decision put it there or because no operation in the base needs it.

- Trapping exception handling, in the sense of 754's alternate exception handling, does not
  exist. Arithmetic exceptions are sticky and never divert control, and the trap-model chapter
  lists no floating-point arithmetic cause.
- Decimal formats, extended formats, and formats other than binary32 and binary64 are absent
  from the base, and no instruction names one.
- A per-instruction rounding field does not exist. The rounding direction comes from machine
  state, which is what keeps every floating-point instruction inside the operand-byte layout
  the encoding chapter fixes.
- Round-to-integral-value, remainder, scaleB, logB, and the recommended transcendental
  operations are not base instructions. Software composes rounding to an integral value from
  the arithmetic that is here, and the idiom appears under the integer-conversion family
  below.

An implementation that claims base conformance implements every operation in this chapter at
the accuracy stated above. A machine that computes on a host floating-point unit meets that
accuracy requirement when the host unit is itself 754 conformant and the machine drives the
host's rounding direction from the architectural rounding mode. Accuracy is not the whole of
what this chapter requires, and the note below says which rules a host unit does not supply.

**Lowering note (non-normative).** Three rules in this chapter are out of reach for an
operation handed to a 754-conformant host unchanged, and each of them costs a fix-up. Both
major hosts propagate an operand's NaN payload where this chapter requires the canonical quiet
NaN of the destination format, so a translator substitutes the canonical value on every
arithmetic result that is a NaN. The minimum and maximum operations follow minNum and maxNum,
which AArch64 implements directly in FMINNM and FMAXNM and x86 does not, since MINSD and MAXSD
return their second operand whenever an operand is a NaN and for the pair of signed zeros. The
float-to-integer conversions carry the saturation and NaN rules that the integer-conversion
section's own lowering note describes.

## The floating-point control and status register

Floating-point control state lives in the control and status register space, under the name
`fcsr`, and not in dedicated instructions. Software reads it with `csr_read` and writes it
with `csr_write`, the same two instructions that reach every other control and status
register. The privileged-architecture chapter owns the register's number and its access class;
this chapter owns the meaning of its bits.

The register is a full word, and the base defines its low eight bits.

| Bits | Field | Meaning |
|:-----|:------|:--------|
| 63..8 | reserved | Reads as zero, and a write of any value other than zero raises the illegal-operand trap with subcode 6. |
| 7..5 | `frm` | The rounding-mode field, three bits. |
| 4..0 | `fflags` | The sticky exception flags, five bits. |

At reset the whole register is zero, which is round-to-nearest-ties-to-even with no exception
raised. The reserved upper bits are held for a future extension that adds trap enables, and
they are rejected rather than ignored on a write so that software written against such an
extension cannot silently do nothing on a base machine.

A write to `fcsr` replaces both fields at once, so clearing the sticky flags without
disturbing the rounding mode is a read, a mask, and a write. The flags are set by the machine
and cleared only by software.

    csr_read $0000 r5          ; fcsr: r5 holds the whole register
    and r5 $E0 r5              ; keep frm, clear every sticky flag
    csr_write r5 $0000         ; fcsr: commit

### The rounding-mode field

The three-bit `frm` field names the rounding direction that every rounding operation uses.

| Encoding | Name | Direction |
|:---------|:-----|:----------|
| `%000` | `rne` | To nearest, ties to even. |
| `%001` | `rtz` | Toward zero. |
| `%010` | `rdn` | Toward negative infinity. |
| `%011` | `rup` | Toward positive infinity. |
| `%100` | `rmm` | To nearest, ties away from zero. |
| `%101` | reserved | No direction; see below. |
| `%110` | reserved | No direction; see below. |
| `%111` | reserved | No direction; see below. |

A rounding operation executed while `frm` holds `%101`, `%110`, or `%111` raises the
illegal-operand trap, writes nothing to its destination, and sets no flag. It does not fall
back to round-to-nearest, because a program that reaches a reserved mode has a bug and the
machine's job is to stop rather than to guess. This is the same mistake-proofing stance that
makes a reserved opcode trap and an unmarked numeric literal a syntax error.

The trap is a property of the operation, not of the write that set the mode. A `csr_write`
that places a reserved encoding in `frm` succeeds, and the next rounding operation is what
traps. The non-rounding operations, which are negation, absolute value, minimum, maximum,
every comparison, the widening conversion, and both float-to-integer conversions, execute
normally under a reserved mode because they never consult it.

### The sticky exception flags

The five bits of `fflags` accumulate the 754 arithmetic exceptions. A bit set by an operation
stays set until software clears it, and no operation clears a bit.

| Bit | Name | Raised when |
|:----|:-----|:------------|
| 4 | `nv` | The operation is invalid: a signaling NaN operand, infinity times zero, infinity minus infinity, zero divided by zero, infinity divided by infinity, the square root of a negative operand other than negative zero, or a float-to-integer conversion whose result is not representable. |
| 3 | `dz` | A finite nonzero value is divided by zero. |
| 2 | `of` | The rounded result exceeds the largest finite value of the destination format in magnitude. |
| 1 | `uf` | The result is tiny and inexact. |
| 0 | `nx` | The delivered result differs from the exact mathematical result. |

Two details pin the flags for conformance. Tininess for `uf` is detected after rounding, so a
result that rounds up to the smallest normal value is not tiny and raises neither `uf` nor,
on that account, `nx`. Overflow raises `nx` alongside `of` in every rounding direction,
because an overflowed result is never the exact value.

Arithmetic exceptions never divert control. A divide by zero delivers the correctly signed
infinity and sets `dz`; an invalid operation delivers the canonical quiet NaN and sets `nv`;
the operation always delivers its 754-defined result. Only an illegal encoding or a reserved
rounding mode raises a trap.

## Comparison policy

Every comparison in this chapter is a quiet comparison. A quiet NaN operand makes the pair
unordered, produces the answer the predicate defines for the unordered case, and raises no
flag. A signaling NaN operand raises `nv` on every comparison, including the ordered and
unordered tests, and the comparison then answers as though the operand were the corresponding
quiet NaN.

This is a deliberate divergence from RISC-V, and it is worth naming plainly rather than
leaving a reader to discover it. In RISC-V the ordering compares `flt` and `fle` are
signalling compares that raise the invalid flag on a quiet NaN, while only `feq` is quiet.
Maize v2 makes all six predicates quiet. The reason is that the flagless design turned every
comparison into a value-producing instruction that a compiler emits freely, including for C's
relational operators on values a program has already checked, and a predicate whose flag
behavior differs from its neighbour's is exactly the sort of per-instruction footnote v2 is
removing. Software that needs the signalling behavior of a relational operator obtains it by
testing for a NaN first with `float_compare_unordered`, which costs one instruction and is
explicit at the point of use.

A comparison writes 1 or 0 into a whole 64-bit destination register. There is no condition
register, so the result is an ordinary value that the branches, the `select` pair, and the
integer instructions all consume directly.

## NaN results

Every arithmetic operation in this chapter that produces a NaN produces the canonical quiet
NaN of its destination format, rather than propagating an operand's payload.

- The binary64 canonical quiet NaN is `$7FF8000000000000`, which is positive, quiet, and has
  a zero payload.
- The binary32 canonical quiet NaN is `$7FC00000`, which is positive, quiet, and has a zero
  payload, and it is written zero-extended into the full destination as `$000000007FC00000`.

A signaling NaN is a NaN whose significand's most significant bit is clear, and a quiet NaN is
a NaN whose significand's most significant bit is set. A signaling NaN reaching any operation
in this chapter raises `nv`.

Two instructions are exceptions to the canonicalization rule, and they are exceptions because
they touch only the sign bit. `float_negate` and `float_absolute` copy their operand with the
sign bit inverted or cleared, pass a NaN payload through unchanged, raise no flag on any
input including a signaling NaN, and never round. That exactness is what lets the negated
fused operations be built from the fused operations without a second rounding.

Nothing in the base injects a sign from one register into another, and nothing in the base
classifies a value into a 754 class, because both are integer bit operations on the register
that already holds the value. The idioms appear in the reference entries below.

## Conversions and saturation

A float-to-integer conversion saturates rather than wrapping, and it never leaves a
destination register undefined.

- A value whose truncation exceeds the largest value of the destination integer type yields
  that largest value and sets `nv`.
- A value whose truncation falls below the smallest value of the destination integer type
  yields that smallest value and sets `nv`.
- A NaN of either sign yields zero and sets `nv`.
- Any other value yields its exact truncation, and sets `nx` when the source had a fractional
  part.

The NaN rule is the one place where the base departs from the RISC-V conversion behavior it
otherwise resembles, and the departure is deliberate. RISC-V yields the maximum-magnitude
positive value for a NaN input; Maize yields zero, which is what a C cast of a NaN yields on
the platforms C programs are actually written against, and which is far likelier to be the
value a program can recover from.

Saturation applies to the destination type rather than to the register, so
`float_to_unsigned` on a negative value yields zero and `float_to_signed` on a large positive
value yields `$7FFFFFFFFFFFFFFF`. Both set `nv`, and neither has a defaulted or
implementation-chosen alternative.

An integer-to-float conversion is exact when the integer is representable in the destination
format, and correctly rounded under the current rounding mode when it is not, setting `nx` in
that case. No integer-to-float conversion can overflow to infinity in binary64, and a 64-bit
integer of large magnitude converting to binary32 rounds normally and can reach infinity only
by rounding a value already at the format's overflow threshold, which no 64-bit integer is.

## Predicate completeness

The base spends six opcode pairs on floating-point comparison, and those six predicates reach
every relation a program can ask about a pair of floating-point values. The completeness is
structural rather than a matter of inspection, which matters because the v1 predicate family
had a gap that only an audit found.

The four 754 relations are less than, equal, greater than, and unordered, and exactly one of
them holds for any pair of values. The base names `float_compare_lt`, `float_compare_le`,
`float_compare_eq`, `float_compare_ne`, `float_compare_ordered`, and
`float_compare_unordered`. Greater-than is less-than with the operands written in the other
order, and greater-or-equal is less-or-equal with the operands written in the other order.

That swap is exact for every input, including NaNs, and the exactness is the whole argument. A
value is greater than another when and only when the other is less than it, and when either
operand is a NaN both orderings answer 0, which is the correct unordered answer for both
predicates. No input distinguishes `float_compare_gt rs1 rs2 rd` from
`float_compare_lt rs2 rs1 rd`, so the missing mnemonics carry no missing behavior and the
assembler needs no synthesized form to hide.

The negations follow the same way and cost one extra instruction where a program wants them.
The predicate "not less than", which is true when the operands are unordered, is
`float_compare_lt` followed by `xor rd #1 rd`. The predicate "not equal in the ordered sense",
true only when both operands are numbers that differ, is `float_compare_ne` combined with
`float_compare_ordered` by `and`. Every one of the fourteen predicates a C compiler can emit
for a relational or equality operator on floating-point operands is therefore reachable, most
in one instruction and the rest in two, and none of them requires a predicate the base does
not have.

## Reading the instruction entries

Each entry below gives the assembly syntax, the encoding length class, the operation, the
traps, and an example. The families share rules that appear once here rather than in every
entry.

Every operand slot of every instruction in this chapter is a plain slot, so each operand byte
holds a form field of `%000` and a nonzero form field raises the illegal-operand trap. The
entries do not repeat that trap, because it belongs to the encoding layer and applies
uniformly. The opcode byte of every instruction in this chapter is in the floating-point band
of the opcode-map appendix, `$C8` through `$F3`.

Each operation exists in a binary64 form spelled with a bare mnemonic and a binary32 form
spelled with `.h`. The binary32 form reads the low half-word of each source, ignores the upper
half, computes in binary32, and writes its result zero-extended into the whole destination
register. Where an entry for a `.h` form states its operation briefly, the full rules are the
ones its binary64 sibling states, applied to binary32.

Register r0 reads as zero and discards writes. An instruction that names r0 as its destination
still raises every flag the operation would raise, because the flags are architectural state
and not a property of the destination.

The nineteen rounding operations are `float_add`, `float_subtract`, `float_multiply`,
`float_divide`, `float_square_root`, `float_multiply_add`, `float_multiply_subtract`,
`float_narrow`, `signed_to_float`, and `unsigned_to_float`, together with the `.h` form of
each of them except `float_narrow`. Each of those raises the illegal-operand trap when `frm`
holds a reserved encoding, and each entry says so. No other instruction in this chapter
consults `frm`.

## Arithmetic

The four basic arithmetic operations are correctly rounded under the current rounding mode.
Each takes two source registers and a destination register, in source-to-destination order, so
the first named source is the left operand of a non-commutative operation.

The special-value rules are 754's and are stated once here. A sum of infinities of opposite
sign, a product of zero and infinity, a quotient of zero by zero, and a quotient of infinity
by infinity are invalid, delivering the canonical quiet NaN and raising `nv`. A finite nonzero
value divided by zero delivers the correctly signed infinity and raises `dz`. A rounded result
too large for the format delivers infinity or the largest finite value according to the
rounding direction, and raises `of` with `nx`.

### float_add

**Syntax:**

    float_add rs1 rs2 rd

**Encoding:** `op r r r`, four bytes. The opcode byte is in the floating-point band of the
opcode-map appendix.

**Operation:** The machine reads the binary64 values in rs1 and rs2, computes their exact sum,
rounds it once under the current rounding mode, and writes the whole 64-bit result into rd.
The sum of two zeros of opposite sign is positive zero in every rounding mode except round
toward negative infinity, where it is negative zero. Adding infinities of opposite sign is
invalid.

**Traps:** Illegal-operand, when the rounding-mode field holds a reserved encoding.

**Example:**

    float_add r2 r3 r4         ; r4 becomes the binary64 sum of r2 and r3

### float_add.h

**Syntax:**

    float_add.h rs1 rs2 rd

**Encoding:** `op r r r`, four bytes. The opcode byte is in the floating-point band of the
opcode-map appendix.

**Operation:** The machine reads the binary32 values in the low half-words of rs1 and rs2,
computes their exact sum, rounds it once under the current rounding mode, and writes the
32-bit result zero-extended into rd. The upper halves of both sources are ignored.

**Traps:** Illegal-operand, when the rounding-mode field holds a reserved encoding.

**Example:**

    float_add.h r2 r3 r4       ; r4 becomes the binary32 sum, upper half zero

### float_subtract

**Syntax:**

    float_subtract rs1 rs2 rd

**Encoding:** `op r r r`, four bytes. The opcode byte is in the floating-point band of the
opcode-map appendix.

**Operation:** The machine subtracts the binary64 value in rs2 from the binary64 value in rs1,
rounds the exact difference once under the current rounding mode, and writes the whole 64-bit
result into rd. The difference of two equal finite values is positive zero in every rounding
mode except round toward negative infinity, where it is negative zero. Subtracting infinities
of the same sign is invalid.

**Traps:** Illegal-operand, when the rounding-mode field holds a reserved encoding.

**Example:**

    float_subtract r2 r3 r4    ; r4 becomes r2 minus r3

### float_subtract.h

**Syntax:**

    float_subtract.h rs1 rs2 rd

**Encoding:** `op r r r`, four bytes. The opcode byte is in the floating-point band of the
opcode-map appendix.

**Operation:** The machine subtracts the binary32 value in the low half-word of rs2 from the
binary32 value in the low half-word of rs1, rounds once under the current rounding mode, and
writes the 32-bit result zero-extended into rd.

**Traps:** Illegal-operand, when the rounding-mode field holds a reserved encoding.

**Example:**

    float_subtract.h r2 r3 r4  ; r4 becomes the binary32 difference

### float_multiply

**Syntax:**

    float_multiply rs1 rs2 rd

**Encoding:** `op r r r`, four bytes. The opcode byte is in the floating-point band of the
opcode-map appendix.

**Operation:** The machine multiplies the binary64 values in rs1 and rs2, rounds the exact
product once under the current rounding mode, and writes the whole 64-bit result into rd. The
sign of the result is the exclusive or of the operand signs, including for zeros and
infinities. Multiplying zero by infinity is invalid.

**Traps:** Illegal-operand, when the rounding-mode field holds a reserved encoding.

**Example:**

    float_multiply r2 r3 r4    ; r4 becomes the binary64 product

### float_multiply.h

**Syntax:**

    float_multiply.h rs1 rs2 rd

**Encoding:** `op r r r`, four bytes. The opcode byte is in the floating-point band of the
opcode-map appendix.

**Operation:** The machine multiplies the binary32 values in the low half-words of rs1 and
rs2, rounds once under the current rounding mode, and writes the 32-bit result zero-extended
into rd.

**Traps:** Illegal-operand, when the rounding-mode field holds a reserved encoding.

**Example:**

    float_multiply.h r2 r3 r4  ; r4 becomes the binary32 product

### float_divide

**Syntax:**

    float_divide rs1 rs2 rd

**Encoding:** `op r r r`, four bytes. The opcode byte is in the floating-point band of the
opcode-map appendix.

**Operation:** The machine divides the binary64 value in rs1 by the binary64 value in rs2,
rounds the exact quotient once under the current rounding mode, and writes the whole 64-bit
result into rd. A finite nonzero numerator over a zero denominator yields the correctly signed
infinity and raises `dz`. Zero over zero and infinity over infinity are invalid.

**Traps:** Illegal-operand, when the rounding-mode field holds a reserved encoding.

**Example:**

    float_divide r2 r3 r4      ; r4 becomes r2 divided by r3

### float_divide.h

**Syntax:**

    float_divide.h rs1 rs2 rd

**Encoding:** `op r r r`, four bytes. The opcode byte is in the floating-point band of the
opcode-map appendix.

**Operation:** The machine divides the binary32 value in the low half-word of rs1 by the
binary32 value in the low half-word of rs2, rounds once under the current rounding mode, and
writes the 32-bit result zero-extended into rd.

**Traps:** Illegal-operand, when the rounding-mode field holds a reserved encoding.

**Example:**

    float_divide.h r2 r3 r4    ; r4 becomes the binary32 quotient

## Square root

The square root is correctly rounded and takes one source. Its special values are 754's: the
square root of positive zero is positive zero, the square root of negative zero is negative
zero, the square root of positive infinity is positive infinity, and the square root of any
other negative value is invalid.

### float_square_root

**Syntax:**

    float_square_root rs rd

**Encoding:** `op r r`, three bytes. The opcode byte is in the floating-point band of the
opcode-map appendix.

**Operation:** The machine computes the square root of the binary64 value in rs, rounds it
once under the current rounding mode, and writes the whole 64-bit result into rd. A negative
operand other than negative zero delivers the canonical quiet NaN and raises `nv`.

**Traps:** Illegal-operand, when the rounding-mode field holds a reserved encoding.

**Example:**

    float_square_root r2 r3    ; r3 becomes the binary64 square root of r2

### float_square_root.h

**Syntax:**

    float_square_root.h rs rd

**Encoding:** `op r r`, three bytes. The opcode byte is in the floating-point band of the
opcode-map appendix.

**Operation:** The machine computes the square root of the binary32 value in the low half-word
of rs, rounds it once under the current rounding mode, and writes the 32-bit result
zero-extended into rd.

**Traps:** Illegal-operand, when the rounding-mode field holds a reserved encoding.

**Example:**

    float_square_root.h r2 r3  ; r3 becomes the binary32 square root of r2

## Sign operations

Negation and absolute value are exact bit operations on the sign. Neither rounds, neither
consults the rounding mode, and neither raises a flag for any input, including a signaling
NaN. Both pass a NaN payload through unchanged, which makes them the only instructions in this
chapter that can produce a non-canonical NaN.

Because they are exact, they compose with the fused operations without adding a rounding. The
negated fused multiply-add is `float_multiply_add` followed by `float_negate`, and the result
is still single-rounded.

The base spends no opcode on sign injection, because `copysign` is three integer instructions
on the register that already holds the value: clear the destination's sign with `and` against
`$7FFFFFFFFFFFFFFF`, isolate the source's sign with `and` against `$8000000000000000`, and
combine them with `or`.

### float_negate

**Syntax:**

    float_negate rs rd

**Encoding:** `op r r`, three bytes. The opcode byte is in the floating-point band of the
opcode-map appendix.

**Operation:** The machine copies the whole 64-bit value in rs into rd with bit 63 inverted.
Every other bit is unchanged, so the negation of a NaN is that NaN with the other sign and the
negation of a zero is the zero of the other sign.

**Traps:** None.

**Example:**

    float_negate r2 r3         ; r3 becomes r2 with the sign bit flipped

### float_negate.h

**Syntax:**

    float_negate.h rs rd

**Encoding:** `op r r`, three bytes. The opcode byte is in the floating-point band of the
opcode-map appendix.

**Operation:** The machine copies the low half-word of rs into rd with bit 31 inverted and
bits 63 through 32 of rd set to zero. Bits 30 through 0 are unchanged.

**Traps:** None.

**Example:**

    float_negate.h r2 r3       ; r3 becomes the binary32 negation, upper half zero

### float_absolute

**Syntax:**

    float_absolute rs rd

**Encoding:** `op r r`, three bytes. The opcode byte is in the floating-point band of the
opcode-map appendix.

**Operation:** The machine copies the whole 64-bit value in rs into rd with bit 63 cleared.
Every other bit is unchanged.

**Traps:** None.

**Example:**

    float_absolute r2 r3       ; r3 becomes the magnitude of r2

### float_absolute.h

**Syntax:**

    float_absolute.h rs rd

**Encoding:** `op r r`, three bytes. The opcode byte is in the floating-point band of the
opcode-map appendix.

**Operation:** The machine copies the low half-word of rs into rd with bit 31 cleared and bits
63 through 32 of rd set to zero.

**Traps:** None.

**Example:**

    float_absolute.h r2 r3     ; r3 becomes the binary32 magnitude

## Fused multiply-add

The fused operations compute the exact product of their first two sources, add or subtract
their third source exactly, and round the whole expression once. No intermediate rounding
occurs and no intermediate value is representable-limited, so the fused result differs from
the separate multiply and add wherever the product is inexact.

All four take four register operands in source-to-destination order, so the destination is the
last operand and none of the three sources is destroyed. The invalid cases are 754's: a
product of zero and infinity is invalid regardless of the addend, and an exact infinite product
combined with an infinite addend of the opposite sign is invalid. A NaN addend with an
otherwise valid product delivers the canonical quiet NaN.

The negated forms of both operations are the operation followed by `float_negate`, which is
exact, so the base spends no opcode on them.

### float_multiply_add

**Syntax:**

    float_multiply_add rs1 rs2 rs3 rd

**Encoding:** `op r r r r`, five bytes. The opcode byte is in the floating-point band of the
opcode-map appendix.

**Operation:** The machine computes the exact binary64 product of rs1 and rs2, adds the
binary64 value in rs3 to it exactly, rounds the sum once under the current rounding mode, and
writes the whole 64-bit result into rd. All three sources are read before rd is written, so
naming a source as the destination is well defined.

**Traps:** Illegal-operand, when the rounding-mode field holds a reserved encoding.

**Example:**

    float_multiply_add r2 r3 r4 r5   ; r5 becomes r2 times r3 plus r4, rounded once

### float_multiply_add.h

**Syntax:**

    float_multiply_add.h rs1 rs2 rs3 rd

**Encoding:** `op r r r r`, five bytes. The opcode byte is in the floating-point band of the
opcode-map appendix.

**Operation:** The machine computes the exact binary32 product of the low half-words of rs1
and rs2, adds the binary32 value in the low half-word of rs3 exactly, rounds once under the
current rounding mode, and writes the 32-bit result zero-extended into rd.

**Traps:** Illegal-operand, when the rounding-mode field holds a reserved encoding.

**Example:**

    float_multiply_add.h r2 r3 r4 r5 ; the binary32 fused multiply-add into r5

### float_multiply_subtract

**Syntax:**

    float_multiply_subtract rs1 rs2 rs3 rd

**Encoding:** `op r r r r`, five bytes. The opcode byte is in the floating-point band of the
opcode-map appendix.

**Operation:** The machine computes the exact binary64 product of rs1 and rs2, subtracts the
binary64 value in rs3 from it exactly, rounds the difference once under the current rounding
mode, and writes the whole 64-bit result into rd. All three sources are read before rd is
written.

**Traps:** Illegal-operand, when the rounding-mode field holds a reserved encoding.

**Example:**

    float_multiply_subtract r2 r3 r4 r5  ; r5 becomes r2 times r3 minus r4

### float_multiply_subtract.h

**Syntax:**

    float_multiply_subtract.h rs1 rs2 rs3 rd

**Encoding:** `op r r r r`, five bytes. The opcode byte is in the floating-point band of the
opcode-map appendix.

**Operation:** The machine computes the exact binary32 product of the low half-words of rs1
and rs2, subtracts the binary32 value in the low half-word of rs3 exactly, rounds once under
the current rounding mode, and writes the 32-bit result zero-extended into rd.

**Traps:** Illegal-operand, when the rounding-mode field holds a reserved encoding.

**Example:**

    float_multiply_subtract.h r2 r3 r4 r5 ; the binary32 fused multiply-subtract

## Minimum and maximum

The minimum and maximum operations select one of their two operands and copy it, so they never
round and never consult the rounding mode. They order negative zero below positive zero, which
makes them total over every pair of numeric operands and makes the pair of them
sign-symmetric.

Their NaN behavior is the minNum and maxNum behavior of IEEE 754-2008 rather than the
minimum and maximum operations of 754-2019, which propagate NaNs instead. The choice is
deliberate and is named here because a reader holding the 2019 standard will expect the other
one. A single quiet NaN operand is treated as missing data and the numeric operand is
returned, which is what a reduction over an array with holes in it wants and what the C
library's `fmin` and `fmax` promise. Two NaN operands deliver the canonical quiet NaN. A
signaling NaN operand raises `nv` and is then treated as its quiet counterpart for the
selection, so the result is the numeric operand when there is one.

### float_minimum

**Syntax:**

    float_minimum rs1 rs2 rd

**Encoding:** `op r r r`, four bytes. The opcode byte is in the floating-point band of the
opcode-map appendix.

**Operation:** The machine writes the smaller of the binary64 values in rs1 and rs2 into rd,
copying the selected operand's bits exactly. Negative zero is smaller than positive zero, so
the pair of zeros yields negative zero. A single NaN operand yields the other operand, and two
NaN operands yield the canonical quiet NaN.

**Traps:** None.

**Example:**

    float_minimum r2 r3 r4     ; r4 becomes the smaller of r2 and r3

### float_minimum.h

**Syntax:**

    float_minimum.h rs1 rs2 rd

**Encoding:** `op r r r`, four bytes. The opcode byte is in the floating-point band of the
opcode-map appendix.

**Operation:** The machine writes the smaller of the binary32 values in the low half-words of
rs1 and rs2 into rd, zero-extended, under the same zero and NaN rules as the binary64 form.

**Traps:** None.

**Example:**

    float_minimum.h r2 r3 r4   ; the binary32 minimum into r4

### float_maximum

**Syntax:**

    float_maximum rs1 rs2 rd

**Encoding:** `op r r r`, four bytes. The opcode byte is in the floating-point band of the
opcode-map appendix.

**Operation:** The machine writes the larger of the binary64 values in rs1 and rs2 into rd,
copying the selected operand's bits exactly. Positive zero is larger than negative zero, so
the pair of zeros yields positive zero. A single NaN operand yields the other operand, and two
NaN operands yield the canonical quiet NaN.

**Traps:** None.

**Example:**

    float_maximum r2 r3 r4     ; r4 becomes the larger of r2 and r3

### float_maximum.h

**Syntax:**

    float_maximum.h rs1 rs2 rd

**Encoding:** `op r r r`, four bytes. The opcode byte is in the floating-point band of the
opcode-map appendix.

**Operation:** The machine writes the larger of the binary32 values in the low half-words of
rs1 and rs2 into rd, zero-extended, under the same zero and NaN rules as the binary64 form.

**Traps:** None.

**Example:**

    float_maximum.h r2 r3 r4   ; the binary32 maximum into r4

## Comparisons

Each comparison writes the whole 64-bit destination with 1 or 0. None of them rounds, none
consults the rounding mode, and all of them are quiet under the policy stated earlier in this
chapter. A signaling NaN operand raises `nv` on every one of them and then behaves as the
corresponding quiet NaN.

Comparing a value against itself is the NaN test, since a value is unordered with itself when
and only when it is a NaN, so `float_compare_unordered r2 r2 r3` is `isnan`.

### float_compare_eq

**Syntax:**

    float_compare_eq rs1 rs2 rd

**Encoding:** `op r r r`, four bytes. The opcode byte is in the floating-point band of the
opcode-map appendix.

**Operation:** The machine writes 1 into rd when the binary64 values in rs1 and rs2 are
ordered and numerically equal, and 0 otherwise. Positive zero and negative zero are equal. An
unordered pair yields 0.

**Traps:** None.

**Example:**

    float_compare_eq r2 r3 r4  ; r4 becomes #1 when r2 equals r3

### float_compare_eq.h

**Syntax:**

    float_compare_eq.h rs1 rs2 rd

**Encoding:** `op r r r`, four bytes. The opcode byte is in the floating-point band of the
opcode-map appendix.

**Operation:** The machine writes 1 into rd when the binary32 values in the low half-words of
rs1 and rs2 are ordered and numerically equal, and 0 otherwise.

**Traps:** None.

**Example:**

    float_compare_eq.h r2 r3 r4 ; the binary32 ordered-equal test

### float_compare_ne

**Syntax:**

    float_compare_ne rs1 rs2 rd

**Encoding:** `op r r r`, four bytes. The opcode byte is in the floating-point band of the
opcode-map appendix.

**Operation:** The machine writes 1 into rd when the binary64 values in rs1 and rs2 are
unordered or numerically unequal, and 0 otherwise. This is the exact logical negation of
`float_compare_eq` on the same operands, so an unordered pair yields 1, which is what C's `!=`
operator requires.

**Traps:** None.

**Example:**

    float_compare_ne r2 r3 r4  ; r4 becomes #1 when r2 and r3 differ or either is a NaN

### float_compare_ne.h

**Syntax:**

    float_compare_ne.h rs1 rs2 rd

**Encoding:** `op r r r`, four bytes. The opcode byte is in the floating-point band of the
opcode-map appendix.

**Operation:** The machine writes 1 into rd when the binary32 values in the low half-words of
rs1 and rs2 are unordered or numerically unequal, and 0 otherwise.

**Traps:** None.

**Example:**

    float_compare_ne.h r2 r3 r4 ; the binary32 not-equal test

### float_compare_lt

**Syntax:**

    float_compare_lt rs1 rs2 rd

**Encoding:** `op r r r`, four bytes. The opcode byte is in the floating-point band of the
opcode-map appendix.

**Operation:** The machine writes 1 into rd when the binary64 values in rs1 and rs2 are
ordered and the value in rs1 is less than the value in rs2, and 0 otherwise. An unordered pair
yields 0. Greater-than is this instruction with the sources written in the other order.

**Traps:** None.

**Example:**

    float_compare_lt r3 r2 r4  ; r4 becomes #1 when r2 is greater than r3

### float_compare_lt.h

**Syntax:**

    float_compare_lt.h rs1 rs2 rd

**Encoding:** `op r r r`, four bytes. The opcode byte is in the floating-point band of the
opcode-map appendix.

**Operation:** The machine writes 1 into rd when the binary32 values in the low half-words of
rs1 and rs2 are ordered and the first is less than the second, and 0 otherwise.

**Traps:** None.

**Example:**

    float_compare_lt.h r2 r3 r4 ; the binary32 ordered less-than test

### float_compare_le

**Syntax:**

    float_compare_le rs1 rs2 rd

**Encoding:** `op r r r`, four bytes. The opcode byte is in the floating-point band of the
opcode-map appendix.

**Operation:** The machine writes 1 into rd when the binary64 values in rs1 and rs2 are
ordered and the value in rs1 is less than or equal to the value in rs2, and 0 otherwise. An
unordered pair yields 0. Greater-or-equal is this instruction with the sources written in the
other order.

**Traps:** None.

**Example:**

    float_compare_le r3 r2 r4  ; r4 becomes #1 when r2 is greater than or equal to r3

### float_compare_le.h

**Syntax:**

    float_compare_le.h rs1 rs2 rd

**Encoding:** `op r r r`, four bytes. The opcode byte is in the floating-point band of the
opcode-map appendix.

**Operation:** The machine writes 1 into rd when the binary32 values in the low half-words of
rs1 and rs2 are ordered and the first is less than or equal to the second, and 0 otherwise.

**Traps:** None.

**Example:**

    float_compare_le.h r2 r3 r4 ; the binary32 ordered less-or-equal test

### float_compare_ordered

**Syntax:**

    float_compare_ordered rs1 rs2 rd

**Encoding:** `op r r r`, four bytes. The opcode byte is in the floating-point band of the
opcode-map appendix.

**Operation:** The machine writes 1 into rd when neither the binary64 value in rs1 nor the
binary64 value in rs2 is a NaN, and 0 otherwise.

**Traps:** None.

**Example:**

    float_compare_ordered r2 r2 r4 ; r4 becomes #0 when r2 is a NaN

### float_compare_ordered.h

**Syntax:**

    float_compare_ordered.h rs1 rs2 rd

**Encoding:** `op r r r`, four bytes. The opcode byte is in the floating-point band of the
opcode-map appendix.

**Operation:** The machine writes 1 into rd when neither binary32 value in the low half-words
of rs1 and rs2 is a NaN, and 0 otherwise.

**Traps:** None.

**Example:**

    float_compare_ordered.h r2 r3 r4 ; the binary32 ordered test

### float_compare_unordered

**Syntax:**

    float_compare_unordered rs1 rs2 rd

**Encoding:** `op r r r`, four bytes. The opcode byte is in the floating-point band of the
opcode-map appendix.

**Operation:** The machine writes 1 into rd when either the binary64 value in rs1 or the
binary64 value in rs2 is a NaN, and 0 otherwise. This is the exact logical negation of
`float_compare_ordered` on the same operands.

**Traps:** None.

**Example:**

    float_compare_unordered r2 r2 r4 ; r4 becomes #1 when r2 is a NaN

### float_compare_unordered.h

**Syntax:**

    float_compare_unordered.h rs1 rs2 rd

**Encoding:** `op r r r`, four bytes. The opcode byte is in the floating-point band of the
opcode-map appendix.

**Operation:** The machine writes 1 into rd when either binary32 value in the low half-words of
rs1 and rs2 is a NaN, and 0 otherwise.

**Traps:** None.

**Example:**

    float_compare_unordered.h r2 r2 r4 ; the binary32 NaN test on r2

## Format conversions

The two format conversions are the only members of the floating-point band with no format
pairing, because each names both formats by itself. Narrowing rounds, since binary32 cannot
hold every binary64 value. Widening is exact, since every binary32 value is a binary64 value.

Both conversions quiet a signaling NaN and canonicalize any NaN, so a NaN reaching either one
comes out as the destination format's canonical quiet NaN. A signaling NaN raises `nv` and a
quiet NaN does not.

### float_narrow

**Syntax:**

    float_narrow rs rd

**Encoding:** `op r r`, three bytes. The opcode byte is in the floating-point band of the
opcode-map appendix.

**Operation:** The machine converts the binary64 value in rs to binary32, rounding once under
the current rounding mode, and writes the 32-bit result zero-extended into rd. A magnitude too
large for binary32 delivers infinity or the largest finite binary32 value according to the
rounding direction and raises `of` with `nx`. A tiny inexact result raises `uf` with `nx`. Any
NaN delivers the binary32 canonical quiet NaN, and a signaling NaN also raises `nv`.

**Traps:** Illegal-operand, when the rounding-mode field holds a reserved encoding.

**Example:**

    float_narrow r2 r3         ; r3 holds the binary32 form of the binary64 value in r2

### float_widen

**Syntax:**

    float_widen rs rd

**Encoding:** `op r r`, three bytes. The opcode byte is in the floating-point band of the
opcode-map appendix.

**Operation:** The machine converts the binary32 value in the low half-word of rs to binary64
and writes the whole 64-bit result into rd. The upper half of rs is ignored. The conversion is
exact for every finite value, including every subnormal binary32 value, which becomes a normal
binary64 value, so it raises neither `nx` nor `uf` and does not consult the rounding mode. Any
NaN delivers the binary64 canonical quiet NaN, and a signaling NaN also raises `nv`.

**Traps:** None.

**Example:**

    float_widen r2 r3          ; r3 holds the binary64 form of the binary32 value in r2

## Integer conversions

The eight integer conversions move between the floating-point formats and the 64-bit signed
and unsigned integer interpretations of a register. There are no narrower integer widths in
this family, because a 32-bit integer result is the 64-bit result truncated by an existing
integer instruction and a 32-bit integer source is already sign-extended or zero-extended in
its register by the load that brought it in.

Both float-to-integer conversions round toward zero, always, regardless of the current
rounding mode. The direction is a property of the opcode and not of machine state, which makes
a C cast a single Maize instruction rather than a cast wrapped in two control-register
accesses. Software that wants a
different direction rounds the value to an integral floating-point value first and then
converts. The mode-respecting rounding-to-integral idiom for a binary64 value of magnitude
below 2^52 is to add and then subtract 2^52 of the value's own sign, which is exact in every
mode and leaves an integral value that the truncating conversion then reads without further
loss.

Both integer-to-float conversions round under the current rounding mode, because a 64-bit
integer is frequently not representable in either destination format and no fixed direction
is the obviously right one for it.

The saturation rules and the NaN-yields-zero rule are stated once in the conversions section
earlier in this chapter and apply to all four float-to-integer instructions.

**Lowering note (non-normative).** The truncating direction is one host instruction on
AArch64, where `fcvtzs` and `fcvtzu` saturate to the endpoint and return zero for a NaN
exactly as this chapter requires. It is not one instruction on x86-64. There `cvttsd2si`
returns the integer-indefinite value for every out-of-range magnitude and for a NaN alike,
where this chapter requires the signed endpoint for an out-of-range magnitude and zero for a
NaN, so a translator adds a test and a fix-up path. The unsigned conversion has no single
SSE2 instruction at all, since `cvttsd2usi` arrives only with AVX-512DQ, so its baseline
lowering is the bias-and-subtract sequence around 2^63. The argument for putting the rounding
direction in the opcode holds on both hosts; the instruction count does not.

### float_to_signed

**Syntax:**

    float_to_signed rs rd

**Encoding:** `op r r`, three bytes. The opcode byte is in the floating-point band of the
opcode-map appendix.

**Operation:** The machine truncates the binary64 value in rs toward zero and writes the
result as a signed 64-bit integer into rd. A value at or above 2^63 yields
`$7FFFFFFFFFFFFFFF` and raises `nv`, a value at or below the negative of 2^63 minus one yields
`$8000000000000000` and raises `nv`, and a NaN of either sign yields zero and raises `nv`. A
finite in-range value with a fractional part raises `nx`.

**Traps:** None.

**Example:**

    float_to_signed r2 r3      ; r3 holds the truncated signed integer value of r2

### float_to_signed.h

**Syntax:**

    float_to_signed.h rs rd

**Encoding:** `op r r`, three bytes. The opcode byte is in the floating-point band of the
opcode-map appendix.

**Operation:** The machine truncates the binary32 value in the low half-word of rs toward zero
and writes the result as a signed 64-bit integer into rd, under the same saturation and NaN
rules as the binary64 form.

**Traps:** None.

**Example:**

    float_to_signed.h r2 r3    ; r3 holds the truncated signed value of the binary32 in r2

### float_to_unsigned

**Syntax:**

    float_to_unsigned rs rd

**Encoding:** `op r r`, three bytes. The opcode byte is in the floating-point band of the
opcode-map appendix.

**Operation:** The machine truncates the binary64 value in rs toward zero and writes the
result as an unsigned 64-bit integer into rd. A value at or above 2^64 yields
`$FFFFFFFFFFFFFFFF` and raises `nv`, a value at or below negative one yields zero and raises
`nv`, and a NaN of either sign yields zero and raises `nv`. A negative value greater than
negative one truncates to zero and raises `nx` rather than `nv`, because zero is its exact
truncation.

**Traps:** None.

**Example:**

    float_to_unsigned r2 r3    ; r3 holds the truncated unsigned integer value of r2

### float_to_unsigned.h

**Syntax:**

    float_to_unsigned.h rs rd

**Encoding:** `op r r`, three bytes. The opcode byte is in the floating-point band of the
opcode-map appendix.

**Operation:** The machine truncates the binary32 value in the low half-word of rs toward zero
and writes the result as an unsigned 64-bit integer into rd, under the same saturation and NaN
rules as the binary64 form.

**Traps:** None.

**Example:**

    float_to_unsigned.h r2 r3  ; r3 holds the truncated unsigned value of the binary32 in r2

### signed_to_float

**Syntax:**

    signed_to_float rs rd

**Encoding:** `op r r`, three bytes. The opcode byte is in the floating-point band of the
opcode-map appendix.

**Operation:** The machine reads rs as a signed 64-bit integer, converts it to binary64
rounding under the current rounding mode, and writes the whole 64-bit result into rd. The
conversion is exact for every integer of magnitude at most 2^53 and raises `nx` otherwise.
Zero converts to positive zero.

**Traps:** Illegal-operand, when the rounding-mode field holds a reserved encoding.

**Example:**

    signed_to_float r2 r3      ; r3 holds the binary64 value of the signed integer in r2

### signed_to_float.h

**Syntax:**

    signed_to_float.h rs rd

**Encoding:** `op r r`, three bytes. The opcode byte is in the floating-point band of the
opcode-map appendix.

**Operation:** The machine reads rs as a signed 64-bit integer, converts it to binary32
rounding under the current rounding mode, and writes the 32-bit result zero-extended into rd.
The conversion is exact for every integer of magnitude at most 2^24 and raises `nx` otherwise.

**Traps:** Illegal-operand, when the rounding-mode field holds a reserved encoding.

**Example:**

    signed_to_float.h r2 r3    ; r3 holds the binary32 value of the signed integer in r2

### unsigned_to_float

**Syntax:**

    unsigned_to_float rs rd

**Encoding:** `op r r`, three bytes. The opcode byte is in the floating-point band of the
opcode-map appendix.

**Operation:** The machine reads rs as an unsigned 64-bit integer, converts it to binary64
rounding under the current rounding mode, and writes the whole 64-bit result into rd. The
conversion is exact for every value at most 2^53 and raises `nx` otherwise. The result is
never negative and never infinite.

**Traps:** Illegal-operand, when the rounding-mode field holds a reserved encoding.

**Example:**

    unsigned_to_float r2 r3    ; r3 holds the binary64 value of the unsigned integer in r2

### unsigned_to_float.h

**Syntax:**

    unsigned_to_float.h rs rd

**Encoding:** `op r r`, three bytes. The opcode byte is in the floating-point band of the
opcode-map appendix.

**Operation:** The machine reads rs as an unsigned 64-bit integer, converts it to binary32
rounding under the current rounding mode, and writes the 32-bit result zero-extended into rd.
The conversion is exact for every value at most 2^24 and raises `nx` otherwise.

**Traps:** Illegal-operand, when the rounding-mode field holds a reserved encoding.

**Example:**

    unsigned_to_float.h r2 r3  ; r3 holds the binary32 value of the unsigned integer in r2

## Conformance notes

Each property below is directly testable by a conformance binary, and a conforming machine
exhibits all of them.

- Every binary32 instruction produces the same result and the same flags whether the upper
  half of each source register holds zero or an arbitrary pattern, and every binary32 result
  has an upper half of zero.
- Every rounding operation, executed at each of the five defined rounding modes on the same
  operands, produces the correctly rounded result for that mode, and executed at each of the
  three reserved encodings raises the illegal-operand trap and leaves the destination register
  and the sticky flags unchanged.
- Every non-rounding operation produces its defined result at all eight encodings of the
  rounding-mode field, including the three reserved ones.
- A signaling NaN operand raises the invalid flag on every instruction in the chapter except
  `float_negate`, `float_negate.h`, `float_absolute`, and `float_absolute.h`, and a quiet NaN
  operand raises no flag on any comparison.
- Every NaN-producing arithmetic result is bit-identical to the canonical quiet NaN of its
  format, and `float_negate` and `float_absolute` return their operand's payload unchanged.
- A float-to-integer conversion of a NaN yields zero with the invalid flag raised, and a
  conversion of a value outside the destination range yields the nearest endpoint of that
  range with the invalid flag raised.
- Greater-than and greater-or-equal, formed by swapping the operands of `float_compare_lt` and
  `float_compare_le`, agree with the mathematical relation on every pair of operands and yield
  zero on every unordered pair.
- A write to any reserved bit of the floating-point control and status register raises the
  illegal-operand trap with subcode 6, and a read of that register returns zero in every
  reserved bit.
