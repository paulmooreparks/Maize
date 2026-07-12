# Chapter 3: Subregister Model

This chapter is normative. It fixes how each 64-bit register is sliced into subregisters,
the subregister selector encoding, and the write-merge semantics.

## 3.1 The subregister layout

Every register is 64 bits and is addressable as smaller fields: bytes (B, 8 bits),
quarter-words (Q, 16 bits), half-words (H, 32 bits), and the full word (W, 64 bits). The
full register `Rn` is a synonym for `Rn.W0`. The fields tile the register from the low end:

    byte offset   7   6   5   4   3   2   1   0
                [B7][B6][B5][B4][B3][B2][B1][B0]
                [ Q3   ][ Q2   ][ Q1   ][ Q0   ]
                [   H1         ][   H0         ]
                [           W0                 ]

For the value `$FEDC_BA98_7654_3210` in R0:

| Field | Value | | Field | Value |
|:------|:------|-|:------|:------|
| R0 / R0.W0 | `$FEDCBA9876543210` | | R0.Q0 | `$3210` |
| R0.H1 | `$FEDCBA98` | | R0.B7 | `$FE` |
| R0.H0 | `$76543210` | | R0.B3 | `$76` |
| R0.Q3 | `$FEDC` | | R0.B1 | `$32` |
| R0.Q2 | `$BA98` | | R0.B0 | `$10` |
| R0.Q1 | `$7654` | | | |

B0 is the least-significant byte, B7 the most-significant; H0 is the low half-word, H1 the
high half-word; Q0..Q3 run low to high. The layout is a view over a single backing word,
so it is host-endianness-independent: the numbering above is the architectural truth
regardless of the host.

## 3.2 The subregister selector encoding

An operand byte's low nibble is the subregister selector (the high nibble is the register;
Chapter 5). The fifteen defined selectors:

| Selector | Field | Width |
|:--------:|:------|:------|
| `$0` | B0 | 1 byte |
| `$1` | B1 | 1 byte |
| `$2` | B2 | 1 byte |
| `$3` | B3 | 1 byte |
| `$4` | B4 | 1 byte |
| `$5` | B5 | 1 byte |
| `$6` | B6 | 1 byte |
| `$7` | B7 | 1 byte |
| `$8` | Q0 | 2 bytes |
| `$9` | Q1 | 2 bytes |
| `$A` | Q2 | 2 bytes |
| `$B` | Q3 | 2 bytes |
| `$C` | H0 | 4 bytes |
| `$D` | H1 | 4 bytes |
| `$E` | W0 | 8 bytes |

## 3.3 The `$F` selector: defined default

Selector `$F` is **not** an undefined encoding: it decodes to a defined default of **B0**.
This is one of the enumerated defined, non-trapping outcomes (Chapter 10): a
decoded-but-undefined *operand field* never raises the illegal-instruction trap, because
it is an operand field, not an opcode. An assembler will not emit `$F`; a hand-crafted or
corrupt image that carries it reads and writes B0.

## 3.4 Write-merge semantics

Writing a **named subregister** preserves the rest of the register. `CP $01 R0.B0` writes
only the low byte of R0 and leaves B1..B7 untouched. Writing the **full register** (`.W0`,
or a bare `Rn`) replaces all 64 bits.

This merge rule is uniform across the machine:

- A load takes its width from the destination subregister and lands the bytes in exactly
  that field, preserving the rest: `LD @addr R0.B0` reads one byte, `LD @addr R0.H0` reads
  four, `LD @addr R0` reads eight (Chapter 7 section 7.1).
- A copy that is narrower than its destination extends to the destination's full width: CP
  sign-extends, CPZ zero-extends (Chapter 5 section 5.6, Chapter 7 section 7.1). Naming a
  narrower destination subregister instead writes only that field and preserves the rest.
- SETcc and CLR follow the same rule: a bare register destination writes the full W0 (a
  clean value with no stale upper bits); an explicit subregister writes only that field.

## 3.5 Subregisters and floating-point

Under the Zfinx floating-point model (Chapter 8), the subregister field also selects the
floating-point format of an FP operand: **H0 or H1** selects binary32 (single), and **W0**
selects binary64 (double). A B\* or Q\* subregister on a floating-point operand is illegal
and traps (cause 0, illegal operand). This is the only place where the subregister field
carries a type rather than merely a width; see Chapter 8.

## Sourcing

- Field layout and numbering: README "Sub-registers" and the graphical map; `src/maize_cpu.h`
  `subreg_enum` (lines 112-128), `subreg_mask_enum` (lines 131-147), and the `subword_ref`
  view (lines 155-181).
- Selector encoding and the `$F`->B0 default: README "Sub-register bit field"; the reference
  decode maps in `src/cpu.cpp` (`subreg_*_map` tables) and the defined-default note in
  `docs/spec/trap-model.md` "Defined non-trapping behavior".
- Write-merge: `src/cpu.cpp` `write_subreg_bits` / `copy_regval_reg` / `copy_regval_reg_zext`
  (lines ~615-688); README "Copy width".
- FP width selection: `src/cpu.cpp` `fp_width_from_subreg` (lines ~979-989); Chapter 8.
