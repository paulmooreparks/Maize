# Appendix B: Encoding Quick Reference

This appendix is a restatement rather than a source. Every table and every byte listing below
is taken from the instruction-encoding chapter and the opcode-map appendix, and it exists so
that a reader writing a decoder, a disassembler, or a translator front end has the shapes on
one page. Where this appendix and either of those two disagrees, the disagreement is a defect
in this appendix and they govern.

## The shape of an instruction

An instruction is one escape byte (only on an extension page), then one opcode byte, then zero
to four operand bytes, then zero to two immediates, in that order and in no other. Operand
bytes appear in the source-to-destination order the assembly syntax uses. Immediates follow
every operand byte, even where the assembly syntax writes an immediate first.

    [ escape ] opcode [ operand ... ] [ immediate ... ]

An instruction in the base is one to ten bytes long. No instruction in the base or in any
future extension exceeds sixteen bytes.

## The six invariants

1. The component order is fixed, as diagrammed above.
2. Length is a pure function of the first byte, or of the first two when the first is an escape
   byte.
3. No instruction reads machine state to determine its own length.
4. One operand rides in one byte, and no operand straddles a byte boundary.
5. Immediates occupy whole bytes, are little-endian, and are never split.
6. Every unassigned encoding raises the illegal-instruction trap and never no-operates.

## Length classes

Fifteen classes cover the base, where `op` is the opcode byte, `r` is one operand byte, and
`iN` is an immediate of N bytes.

| Class | Components | Bytes |
|:------|:-----------|:-----:|
| `op` | opcode only | 1 |
| `op r` | opcode, one operand byte | 2 |
| `op r r` | opcode, two operand bytes | 3 |
| `op r r r` | opcode, three operand bytes | 4 |
| `op r r r r` | opcode, four operand bytes | 5 |
| `op i1` | opcode, 1-byte immediate | 2 |
| `op i4` | opcode, 4-byte immediate | 5 |
| `op r i1` | opcode, one operand byte, 1-byte immediate | 3 |
| `op r i2` | opcode, one operand byte, 2-byte immediate | 4 |
| `op r i4` | opcode, one operand byte, 4-byte immediate | 6 |
| `op r i8` | opcode, one operand byte, 8-byte immediate | 10 |
| `op r r i1` | opcode, two operand bytes, 1-byte immediate | 4 |
| `op r r i2` | opcode, two operand bytes, 2-byte immediate | 5 |
| `op r r i4` | opcode, two operand bytes, 4-byte immediate | 7 |
| `op r r i1 i1` | opcode, two operand bytes, two 1-byte immediates | 5 |

An extension page may use classes not in this list, under the same invariants and the same
sixteen-byte ceiling.

## The operand byte

Every register operand is exactly one byte, five bits of register number and three bits of
form.

    bit    7   6   5   4   3   2   1   0
         +---+---+---+---+---+---+---+---+
         |   form    |   register number |
         +---+---+---+---+---+---+---+---+
          bits 7..5      bits 4..0

Bits 4 through 0 hold the register number, 0 through 31, naming r0 through r31 directly. All
thirty-two values are valid, so this field has no undefined encoding. Bits 7 through 5 hold
the form field, whose meaning the opcode fixes for that operand position. The form field never
changes the length of the instruction.

## Operand slot classes

Each opcode declares, statically, which class each of its operand slots belongs to. The
declaration is in the instruction's inventory entry and in the opcode map, and it never varies
with machine state.

| Slot class | Valid form fields | Names | Invalid form fields |
|:-----------|:------------------|:------|:--------------------|
| Plain | `%000` | The whole register | `%001` through `%111` |
| Byte-sliced | `%000` through `%111` | `rN.b0` through `rN.b7` | none |
| Quarter-word-sliced | `%000` through `%011` | `rN.q0` through `rN.q3` | `%100` through `%111` |
| Half-word-sliced | `%000`, `%001` | `rN.h0`, `rN.h1` | `%010` through `%111` |

Every invalid form field raises the illegal-operand trap. In the base, the byte-sliced,
quarter-word-sliced, and half-word-sliced classes appear only on the extract and insert
instructions, and every other slot in the base is plain.

## Immediates

An immediate is 1, 2, 4, or 8 bytes, little-endian, at a byte boundary, immediately after the
last operand byte. The size comes from the opcode and appears in the length class; no field in
the instruction selects it. An instruction carrying two immediates stores them adjacently in
the order its inventory entry lists them, and the bitfield instructions, the only base
instructions with two, store the bit position first and the bit width second.

Extension of a narrow immediate is a property of the opcode.

- The narrow forms of `move` spell the choice in the mnemonic with `z` and `s`, as the loads do,
  and `move.w` carries a full word that no extension touches.
- An ALU immediate and a memory displacement are sign-extended.
- A branch, jump, or call displacement is sign-extended, and it is a signed 32-bit byte count
  measured from the address of the following instruction.
- A control-and-status-register number is an unsigned 16-bit value.
- A syscall number is an unsigned 8-bit value.

## Escape bytes

The primary page reserves seven escape bytes. Each one opens one 256-entry extension opcode
page belonging to exactly one named, versioned extension, and this specification pre-assigns
none of them.

| Byte | Role |
|:----:|:-----|
| `$F8` | Escape byte 0 |
| `$F9` | Escape byte 1 |
| `$FA` | Escape byte 2 |
| `$FB` | Escape byte 3 |
| `$FC` | Escape byte 4 |
| `$FD` | Escape byte 5 |
| `$FE` | Escape byte 6 |

A machine that does not implement the extension owning an escape byte raises the
illegal-instruction trap on the escape byte itself, reports that byte and its own address, and
never fetches the byte after it. A base-only machine therefore traps on all seven.

## The opcode bands at a glance

| Range | Family | Assigned | Reserved |
|:------|:-------|:--------:|:--------:|
| `$00` | Zero-byte guard | 0 | 1 |
| `$01`..`$0F` | Constants and moves | 9 | 6 |
| `$10`..`$3F` | Integer arithmetic and logic | 46 | 2 |
| `$40`..`$5F` | Compares | 20 | 12 |
| `$60`..`$6F` | Branches | 10 | 6 |
| `$70`..`$7F` | Control transfer and select | 7 | 9 |
| `$80`..`$9F` | Loads and stores | 22 | 10 |
| `$A0`..`$AF` | Extract and insert | 12 | 4 |
| `$B0`..`$B7` | Block memory | 3 | 5 |
| `$B8`..`$C7` | System, control registers, TLB, ports | 12 | 4 |
| `$C8`..`$F7` | Floating point | 44 | 4 |
| `$F8`..`$FE` | Extension escape bytes | 7 escapes | 0 |
| `$FF` | Breakpoint | 1 | 0 |

The totals are 186 assigned instruction opcodes, 7 escape bytes, and 63 reserved bytes. The
reserved set is `$00`, `$0A`..`$0F`, `$3E`..`$3F`, `$54`..`$5F`, `$6A`..`$6F`, `$77`..`$7F`,
`$96`..`$9F`, `$AC`..`$AF`, `$B3`..`$B7`, `$C4`..`$C7`, and `$F4`..`$F7`.

## Decoding, in order

1. Read the byte at the program counter. A reserved byte, or an escape byte for an
   unimplemented extension, raises the illegal-instruction trap with the program counter
   unchanged.
2. An escape byte for an implemented extension selects that page's table for everything below.
3. Look up the length class and add the length to the address of the first byte. The address of
   the next instruction is fixed here, before any operand is read.
4. Read the operand bytes in order, checking each form field against its declared slot class.
5. Read the immediates in order, little-endian.
6. Execute. A fault reports the address of the first byte, meaning the escape byte on an
   extension page.

## Traps the encoding layer raises

Two traps arise from encoding alone, both fault-class, and both report the address of the
instruction's first byte.

- **Illegal instruction** fires on a reserved opcode byte, on a reserved entry on an
  implemented extension page, and on an escape byte for an unimplemented extension. The
  offending byte accompanies it.
- **Illegal operand** fires on an operand byte whose form field is undefined for its slot
  class, and on an immediate value the instruction's own entry defines as invalid. The
  offending byte or value accompanies it.

## Worked shapes

Each listing gives the assembly form and the exact bytes in memory order.

`add r1 r2 r3`, class `op r r r`, four bytes:

    $10 $01 $02 $03

`add r4 $1000 r4`, class `op r r i4`, seven bytes, with the 32-bit immediate sign-extended
before the addition:

    $31 $04 $04 $00 $10 $00 $00

`load @r30+$20 r5`, class `op r r i2`, five bytes, with the displacement after both operand
bytes even though the syntax writes it inside the memory operand:

    $87 $1E $05 $20 $00

`load.zb @r9 r4`, class `op r r`, three bytes, with the access width and the extension rule
carried by the opcode alone:

    $81 $09 $04

`extract.zb r3.b5 r7`, class `op r r`, three bytes, with byte index 5 in the first operand
byte's form field:

    $A0 $A3 $07

`bitfield_extract r5 #12 #5 r6`, class `op r r i1 i1`, five bytes, position first and width
second:

    $A9 $05 $06 $0C $05

`branch_lt_signed r4 r5 loop_top`, class `op r r i4`, seven bytes, shown with a displacement of
-24:

    $62 $04 $05 $E8 $FF $FF $FF

`move.w $1122334455667788 r10`, class `op r i8`, ten bytes, the longest instruction in the base:

    $02 $0A $88 $77 $66 $55 $44 $33 $22 $11

`return`, class `op`, one byte:

    $74
