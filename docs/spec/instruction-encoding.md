# Chapter 6: Instruction Encoding

This chapter is normative. It fixes the byte-level structure of an instruction: the opcode
byte, the operand bytes, and the placement of immediate values.

## 6.1 Instruction shape

Instructions are **variable-length**. An instruction is:

1. one **opcode byte**, then
2. zero, one, two, or three **operand bytes** (one per operand), then
3. for an immediate source operand, the **immediate bytes**, little-endian.

There are no other instruction components. The decoder reads the opcode byte, uses it to
determine how many operand bytes and immediate bytes follow, and advances the program
counter (RP) past the whole instruction. When an instruction has two parameters, the first
is the **source** and the second is the **destination**. Three-operand instructions (LEA,
CMPXCHG, MULW, UMULW, FMADD, FMSUB) name their operands in the order the instruction's
entry in Chapter 7 gives.

## 6.2 The opcode byte

The opcode byte is two flag (mode) bits plus a six-bit base opcode:

    %BBxx`xxxx   bits 7,6    mode bits (Chapter 5 section 5.1)
    %xxBB`BBBB   bits 5..0   base opcode (0..63)

The six base-opcode bits give **64 base slots**. Most instructions occupy one base slot and
use the two mode bits to select the addressing-mode form (regVal `$0x`, immVal `$4x`,
regAddr `$8x`, immAddr `$Cx`). Some families instead pack multiple **register-only**
micro-ops into one base slot using the two high bits as a "condition row" selector rather
than as addressing-mode bits:

- **Condition families** (Jcc, SETcc): the two high bits select the condition row and the
  base slot selects the column; together `(row * 3 + col)` indexes one shared predicate
  table (Chapter 7 section 7.6/7.7).
- **Row-packed unary families**: for example base `$31` packs INC (`$31`), DEC (`$71`),
  NOT (`$B1`), NEG (`$F1`); base `$29` packs SETINT / CLRINT / SETCRY / CLRCRY; base `$27`
  packs RET (`$27`), IRET (`$67`), NOP (`$A7`), with row 3 (`$E7`) reserved. (BRK is the
  standalone byte `$FF`, not a member of base `$27`.) The FP unary/min-max/convert families
  are row-packed the same way (Chapter 8).

Which interpretation applies is fixed per base slot by the instruction map (Appendix A);
the decoder does not choose.

## 6.3 Operand bytes

Each operand is one byte (Chapter 5 sections 5.3, 5.4):

- A **register** operand byte is register nibble + subregister nibble (`$3E` = R3.W0).
- An **immediate** source operand byte carries the immediate width in its low three bits
  (`$02` = a 4-byte immediate follows).

An instruction with N operands has N operand bytes, in source-then-destination order (with
the three-operand order per the instruction's entry).

## 6.4 Immediate placement

Immediate bytes follow the operand bytes, in **little-endian** order, in the width the
source operand byte selected (1, 2, 4, or 8 bytes). Only a source operand can be an
immediate; a destination is always a register or a memory address named by a register or
immediate.

## 6.5 Worked example

`CP $FFCC4411 R3` encodes as seven bytes:

    $41   $02   $3E   $11 $44 $CC $FF
    |     |     |     |
    |     |     |     +-- immediate $FFCC4411, little-endian (low byte first)
    |     |     +-------- destination operand byte: R3 ($3) . W0 ($E)
    |     +-------------- source operand byte: immediate size $2 = 4 bytes
    +-------------------- opcode: base $01, mode bit 6 set (immediate source) = "CP immVal reg"

The decoder reads `$41`, sees an immVal-form CP, reads the source operand byte `$02` (a
4-byte immediate), reads the destination operand byte `$3E`, then reads the four immediate
bytes, and advances RP by seven. Because CP is narrower (32-bit immediate) than its
destination (R3.W0, 64-bit), the value is sign-extended (Chapter 5 section 5.6).

## 6.6 Decoded-but-undefined fields

Operand-field encodings split into two cases. The undefined **immediate-size** encoding
4..7 decodes to the value-initialized default and never traps (Chapter 5 section 5.4). The
undefined **subregister** selector `$F`, by contrast, is a deterministic illegal-operand
trap (cause 0; Chapter 3 section 3.3, Chapter 10). An undefined *opcode* byte is likewise
the illegal-instruction trap (cause 0). So an undefined opcode and an undefined subregister
both trap; only the undefined immediate-size field has a defined non-trapping default.

## Sourcing

- Instruction shape and the source-then-destination order: README "Instruction Description".
- Opcode byte structure and the 64 base slots: README "Opcode Bytes"; `src/maize_cpu.h`
  `opcode_flag*` (lines 26-29) and the base-opcode constants; `docs/spec/reservations.md`
  section 1.
- Row-packed / condition families: `src/maize_cpu.h` `cond_row_*` and the Jcc/SETcc/unary
  constants (lines ~638-745); `src/cpu.cpp` `decode_condition` / `eval_condition`
  (lines ~775-800).
- Immediate placement: README "Instruction Description" and "Immediate Parameter".
