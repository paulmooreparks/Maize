# Chapter 5: Addressing Modes and Operand Encoding

This chapter is normative. It fixes the two opcode mode bits, the four addressing-mode
forms they select, the register-operand byte, the immediate-size field, the `@` memory
marker, and the sign/zero-extension rules.

## 5.1 The two mode bits

The opcode byte is two flag bits (bits 7 and 6) plus a six-bit base opcode (bits 5..0;
Chapter 6). For an instruction whose source operand may vary, the two mode bits select the
addressing-mode form of the **source** operand:

    %BBxx`xxxx   bits 7,6 = mode bits
    %x0xx`xxxx   bit 6 = 0: source is a register
    %x1xx`xxxx   bit 6 = 1: source is an immediate value
    %0xxx`xxxx   bit 7 = 0: source is a value
    %1xxx`xxxx   bit 7 = 1: source is a memory address (dereferenced)

Bit 6 chooses register vs immediate; bit 7 chooses value vs memory address. The
destination operand's shape is fixed by the instruction, not by these bits.

## 5.2 The four addressing-mode forms

The two bits give four forms. Using base opcode `$03` (ADD) as the worked example:

| Bits 7,6 | Hex form | Source shape | Meaning (ADD) |
|:--------:|:--------:|:-------------|:--------------|
| 0 0 | `$03` | regVal | Source is a register value: `ADD R1 R0` |
| 0 1 | `$43` | immVal | Source is an immediate: `ADD $05 R0` |
| 1 0 | `$83` | regAddr | Source is the value at the address in a register: `ADD @R1 R0` |
| 1 1 | `$C3` | immAddr | Source is the value at an immediate address: `ADD @$1000 R0` |

Not every instruction defines all four forms. Data-movement instructions are held to the
strict CP/LD/ST split (section 5.5): CP and CPZ define only the two value forms (regVal,
immVal), because a copy never touches memory; LD defines only the two address forms,
because a load always reads memory; ST is the store side. Control-transfer, stack, and
zero-operand instructions define the subset that makes sense for them (Chapter 7). The
opcode map (Appendix A) lists exactly which forms exist for each instruction; a form not in
the map is a reserved encoding.

## 5.3 The register-operand byte

Each operand that names a register is one byte: the high nibble is the register (Chapter 2
section 2.1) and the low nibble is the subregister selector (Chapter 3 section 3.2).

    %RRRR`SSSS   RRRR = register field ($0..$F)   SSSS = subregister selector ($0..$F)

For example the operand byte `$3E` names `R3.W0` (register `$3` = R3, subregister `$E` =
W0), and `$0C` names `R0.H0`.

## 5.4 The immediate-size field

When the source is an immediate (bit 6 = 1), the **source operand byte** carries the
immediate's width in its low three bits, and the immediate bytes follow in the instruction
stream, little-endian:

    %xxxx`x000   $x0   1-byte immediate  (8 bits)
    %xxxx`x001   $x1   2-byte immediate  (16 bits)
    %xxxx`x010   $x2   4-byte immediate  (32 bits)
    %xxxx`x011   $x3   8-byte immediate  (64 bits)

Bits 0..2 select the width. **Bit 3 and bits 4..7 are reserved** (must be zero) and carry
no operation; a previously documented immediate math-operation mode was never implemented
and is withdrawn. An immediate-size encoding of 4..7 is a decoded-but-undefined operand
field and decodes to the value-initialized default (Chapter 10), not a trap.

## 5.5 The `@` memory-access marker and the CP/LD/ST boundary

In assembly, a leading `@` on an operand marks a **memory access**: `@R1` is "the value at
the address in R1", `@$1000` is "the value at address `$1000`", and `@R1.H0` dereferences
the address held in R1.H0. An operand without `@` is a plain value. `@` corresponds to
bit 7 = 1 (the address mode bit) on the encoding side.

The three data-movement instructions name the memory boundary explicitly, and the
assembler enforces the split:

- **CP** (and **CPZ**) move a value into a register (register-to-register or
  immediate-to-register). They never touch memory, so their source is never an address;
  `CP @...` is rejected.
- **LD** reads from a memory address into a register. Its source is always an address
  (`@`-prefixed); `LD value` (a non-address source) is rejected.
- **ST** writes a register or immediate to a memory address. Its destination is always an
  address.

The ALU instructions are separate and, Maize being CISC, may take a memory-address operand
directly (`ADD @R1 R0`); only CP / LD / ST / CPZ are held to the strict split.

## 5.6 Extension rules: sign vs zero

A source narrower than its destination is extended to the destination's full width:

- **CP and the ALU immediates sign-extend.** `CP $FF R0` leaves R0 =
  `$FFFF_FFFF_FFFF_FFFF` (the byte `$FF` read as signed -1); `CP $01 R0` leaves R0 = 1.
  `ADD $01 R0` adds exactly 1 rather than carrying stale upper bytes, because the immediate
  is sign-extended to the operation width.
- **CPZ zero-extends.** `CPZ $FF R0` leaves R0 = `$0000_0000_0000_00FF`.

To write only part of a register and preserve the rest, name the destination subregister
explicitly (`CP $01 R0.B0` writes just the low byte; Chapter 3 section 3.4). A load takes
its width from the destination subregister and never has a narrower value to extend, which
is why there is no zero-extending load (no LDZ; Chapter 7 section 7.1).

## 5.7 Worked encoding example

`CP $FFCC4411 R3` copies the 32-bit immediate `$FFCC4411` into R3 (full register, so it
sign-extends):

    $41 $02 $3E $11 $44 $CC $FF

- `$41` = opcode: base `$01` (CP/LD family) with bit 6 set (immediate source): "CP immVal
  reg".
- `$02` = source operand byte: immediate-size `$2` (4-byte immediate).
- `$3E` = destination operand byte: register `$3` = R3, subregister `$E` = W0.
- `$11 $44 $CC $FF` = the 32-bit immediate `$FFCC4411`, little-endian.

The full encoding structure (opcode byte, operand bytes, immediate placement) is Chapter 6.

## Sourcing

- Mode bits and the four forms: README "Opcode Bytes"; `src/maize_cpu.h`
  `opcode_flag_srcReg`/`srcImm`/`srcAddr` (lines 26-29) and the per-instruction form
  constants.
- Register/subregister operand byte: README "Register bit field" and "Sub-register bit
  field"; Chapters 2 and 3.
- Immediate-size field: README "Immediate Parameter"; `src/maize_cpu.h` `opflag_imm_size*`
  (lines 74-78); operand decode in `src/cpu.cpp` (`op1_imm_size` etc., lines ~507-587).
- `@` marker and CP/LD/ST split: README "CP, LD, and ST: the memory boundary".
- Sign/zero-extension: README "Copy width"; `src/cpu.cpp` `copy_regval_reg` (sign-extend)
  and `copy_regval_reg_zext` (zero-extend), lines ~659-688.
