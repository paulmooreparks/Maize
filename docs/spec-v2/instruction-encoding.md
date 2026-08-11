# Instruction Encoding

This chapter is normative. It fixes the byte-level structure of every Maize v2 instruction:
the opcode byte, the escape bytes that open extension opcode pages, the operand bytes, the
placement and size of immediates, and the rules that make instruction length a pure
function of the leading one or two bytes. The instruction inventory names the operations
and their semantics; this chapter says how they are spelled in memory.

The encoding is byte-granular, table-regular, and variable-length, because the machine's
two consumers are a software interpreter that wants cheap length determination and cheap
dispatch, and a translator that wants regular patterns and dense guest code. Nothing here
requires bit-field extraction to read an operand, and no value is split across
non-contiguous fields.

## The regularity invariants

Six invariants govern every encoding in the base and every encoding any extension may add.
A conforming machine satisfies all six, and a conformance binary can exercise each of them
directly.

1. **Fixed component order.** An instruction is one opcode byte (optionally preceded by one
   escape byte), then zero or more operand bytes, then zero or more immediates. No other
   component exists, and no component appears out of that order.
2. **Length from the leading bytes alone.** The total length of an instruction in bytes is a
   pure function of its first byte, or of its first two bytes when the first byte is an
   escape byte. A decoder obtains the length from a 256-entry table indexed by the opcode
   byte, and from that page's own 256-entry table when an escape byte opened a page.
3. **No state-dependent length.** No instruction reads a register, a memory location, a
   control and status register, or any mode bit to determine its own length. Two machines
   in different states decode the same byte string to the same length.
4. **One operand per byte.** Every operand byte encodes exactly one operand. No operand
   shares a byte with another operand, and no operand straddles a byte boundary.
5. **Immediates are whole and aligned to bytes.** Every immediate occupies a whole number of
   bytes, is stored little-endian, and is never split across other components.
6. **Reserved encodings trap.** Every opcode byte that this specification does not assign
   raises the illegal-instruction trap when the machine fetches it as an instruction.
   Nothing in reserved space executes as a no-operation, and no undefined encoding produces
   a defaulted or approximated result.

## Instruction shape

An instruction in the base occupies between one and ten bytes, and the components appear in
this order:

- One **escape byte**, present only for an instruction on an extension opcode page.
- One **opcode byte**, always present, which names the operation and fixes the shape of
  everything that follows.
- Zero to four **operand bytes**, one per register operand, in the order the instruction's
  inventory entry lists its operands.
- Zero to two **immediates**, each of 1, 2, 4, or 8 bytes, in the order the inventory entry
  lists them.

The operand bytes appear in the same source-to-destination order the assembly syntax uses,
so `add r1 r2 r3` places the byte for r1 first and the byte for r3 last. Immediates,
however, always follow every operand byte, even when the assembly syntax writes an
immediate before a register. The instruction `load @r9+$20 r4` therefore encodes the base
register, then the destination register, then the displacement.

The longest instruction in the base is `move.w`, the 64-bit immediate move, at ten bytes.

## The opcode byte

The opcode byte is a flat 8-bit index. It carries no mode bits, no packed condition rows,
and no sub-fields; the machine reads the whole byte and looks it up. The primary page is
the 256-entry table in the opcode-map appendix, which assigns each byte one of three
meanings.

- An **assigned opcode** names one operation and one length class.
- An **escape byte** opens one extension opcode page, and the byte that follows it is that
  page's opcode byte.
- A **reserved byte** raises the illegal-instruction trap.

Flat indexing is what makes the length table a plain array lookup and the dispatch a single
switch. It is also what makes the reserved space auditable: a reader counts assigned bytes
in the appendix and knows exactly what is left.

## Escape bytes and extension pages

The primary page reserves seven escape bytes, `$F8` through `$FE`. Each escape byte opens
one 256-entry extension opcode page, and each page belongs to exactly one named, versioned
extension. Allocation of a page to an extension happens in the extension registry, not
here, so this chapter fixes only the mechanism.

An instruction on an extension page is the escape byte, then the page opcode byte, then
that entry's operand bytes and immediates. The page's own 256-entry table gives the length,
and the length it gives is the total length of the instruction including the escape byte,
so adding a page table's entry to the escape byte's address always yields the next
instruction. The length of any instruction is therefore still fixed by its leading one or
two bytes and nothing else.

A machine that does not implement the extension owning an escape byte raises the
illegal-instruction trap on the escape byte itself, reporting the escape byte as the
offending byte and the escape byte's own address as the faulting address. It does not
consume, examine, or skip the following byte. A machine that does implement the page
decodes the second byte against that page's table, and a reserved entry on the page raises
the illegal-instruction trap in the same way. A base-only machine therefore traps on all
seven escape bytes, which is what makes the presence or absence of an extension observable
by a conformance binary rather than silently tolerated.

## The operand byte

Every register operand is exactly one byte, split as five bits of register number and three
bits of operand form.

- Bits 4 through 0 hold the **register number**, 0 through 31, encoding r0 through r31
  directly. Every one of the 32 values is a valid register, so this field has no undefined
  encoding.
- Bits 7 through 5 hold the **form field**, whose meaning is fixed by the opcode for that
  operand position.

The form field never changes the length of the instruction. It cannot select between an
8-bit and a 32-bit displacement, and it cannot add or remove an operand, because either
would violate the length invariant. What it does carry is length-neutral operand structure,
and in the base that means exactly one thing: which slice of a register an extract or
insert names.

### Operand slot classes

Each opcode declares, statically, which class each of its operand slots belongs to. The
declaration lives in the instruction's inventory entry and in the opcode-map appendix, and
it never varies with machine state.

A **plain slot** names a whole register. The form field is `%000`, and a plain slot whose
form field holds any other value raises the illegal-operand trap. Almost every operand in
the base is a plain slot, including the base-address register of a load or a store and the
pointer registers of the block-memory instructions.

A **sliced slot** names one element of a register, and the form field holds that element's
index. The element width comes from the opcode, so no state is read and no length changes.

- On a byte-sliced opcode the form field holds the byte index, `%000` through `%111`,
  selecting `rN.b0` through `rN.b7`. All eight encodings are valid.
- On a quarter-word-sliced opcode the form field holds the quarter index, `%000` through
  `%011`, selecting `rN.q0` through `rN.q3`. The values `%100` through `%111` raise the
  illegal-operand trap.
- On a half-word-sliced opcode the form field holds the half index, `%000` or `%001`,
  selecting `rN.h0` or `rN.h1`. The values `%010` through `%111` raise the illegal-operand
  trap.

The fourteen positional forms of the register model are therefore all reachable, eight
bytes plus four quarter-words plus two half-words, and each is reachable by exactly one
encoding.

### Why three bits are spent this way

Reserving three bits in every operand byte and requiring them to be zero in a plain slot
costs density, and the cost is deliberate. A uniform 5-plus-3 split means an operand read is
one load, one mask, and one shift with constants known at build time, on every operand of
every instruction, with no per-opcode operand layout to branch on. It also gives every
future extension a place to put length-neutral operand structure without disturbing the
base layout, since an extension page may define new form values for its own slots. Finally,
a nonzero form field in a plain slot is a mistake the machine catches rather than ignores,
which is the same mistake-proofing stance that makes reserved opcodes trap and makes
unmarked numeric literals a syntax error in the assembler.

## Immediates

An immediate is 1, 2, 4, or 8 bytes, stored little-endian, at a byte boundary, immediately
after the last operand byte. The size is fixed by the opcode and appears in the
instruction's length class; no field in the instruction selects it, and no immediate is ever
split across other components.

An instruction that carries two immediates stores them adjacently in the order its
inventory entry lists them, each little-endian and each whole. The bitfield instructions are
the only base instructions with two immediates, and they store the bit position first and
the bit width second.

Whether a narrow immediate is sign-extended or zero-extended into the full 64-bit value the
instruction uses is a property of the opcode, stated in the inventory entry and visible in
the mnemonic wherever the choice exists. The narrow forms of `move` spell the choice with
`z` and `s` exactly as loads do, and `move.w` carries a full word to which no extension rule
applies;
ALU immediates and memory displacements are sign-extended;
branch and jump displacements are sign-extended; a control-and-status-register number is an
unsigned 16-bit value; a syscall number is an unsigned 8-bit value.

## Length classes

The base uses fifteen length classes. Each class is written as a shape string in which `op`
is the opcode byte, `r` is one operand byte, and `iN` is an immediate of N bytes. The
appendix names one class for every assigned opcode.

| Class | Components | Length in bytes |
|:------|:-----------|:---------------:|
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

An extension page may use classes not in this list, subject to the same invariants. The
maximum instruction length across the base and every extension is sixteen bytes, which
fixes the size of a decoder's fetch window for all time.

## Decoding

The machine decodes an instruction in a fixed sequence with no back-tracking.

1. Read the byte at the program counter. If it is a reserved byte, raise the
   illegal-instruction trap with that byte as the offending byte and the program counter
   unchanged. If it is an escape byte for an unimplemented extension, raise the same trap
   in the same way.
2. If the byte is an escape byte for an implemented extension, read the next byte and use
   that page's table for everything below.
3. Look up the length class. Add the length to the address of the opcode byte (or of the
   escape byte) to obtain the address of the next instruction. This value is fixed before
   any operand is read.
4. Read the operand bytes in order. Check each one's form field against its declared slot
   class, raising the illegal-operand trap on any encoding the class does not define.
5. Read the immediates in order, little-endian.
6. Execute. A fault raised during execution reports the address of the opcode byte (or the
   escape byte), because a faulting instruction is restartable and must re-decode
   identically.

Step 3 is the invariant that lets a decoder, a disassembler, a JIT front end, and a
tracing tool all walk a byte stream without executing it. A disassembler that meets an
illegal operand form still knows where the next instruction begins.

## Traps arising from the encoding

The encoding layer raises exactly two traps, both fault-class, and both report the address
of the instruction's first byte.

- The **illegal-instruction** trap fires on a reserved opcode byte on the primary page, on a
  reserved entry on an implemented extension page, and on an escape byte for an extension
  the machine does not implement. The offending byte accompanies the trap.
- The **illegal-operand** trap fires on an operand byte whose form field is not defined for
  its slot class, and on an immediate value the instruction's own entry defines as invalid
  (a bitfield width of zero, for instance). The offending byte or value accompanies the
  trap.

Neither condition has a defaulted interpretation. The v1 rule that an undefined
immediate-size field decoded to a value-initialized default does not carry over, because v2
has no immediate-size field to get wrong: the size lives in the opcode.

## Worked examples

Each example below gives the assembly form, the exact bytes in memory order, and the reason
each byte holds what it holds. Register numbers are decimal in the prose and hexadecimal in
the byte listings.

### A three-operand register ALU instruction

`add r1 r2 r3` adds r1 and r2 and writes r3. Its class is `op r r r`, so it is four bytes:

    $10 $01 $02 $03

The opcode `$10` is `add`. The three operand bytes are plain slots, so each has a form field
of `%000` and carries only its register number: `$01` is r1, `$02` is r2, `$03` is r3.

### An ALU instruction with an immediate second source

`add r4 $1000 r4` adds the constant `$1000` to r4 in place. Its class is `op r r i4`, so it
is seven bytes:

    $31 $04 $04 $00 $10 $00 $00

The opcode `$31` is the immediate form of `add`. The operand bytes name r4 twice, once as
the register source and once as the destination. The four immediate bytes are `$00001000`
little-endian, sign-extended to 64 bits before the addition.

### A load with a displacement

`load @r30+$20 r5` reads a word from 32 bytes above the stack pointer into r5. Its class is
`op r r i2`, so it is five bytes:

    $87 $1E $05 $20 $00

The opcode `$87` is the displaced form of the word-width `load`. The operand byte `$1E` is
r30 in a plain slot, and `$05` is r5. The two immediate bytes are the signed displacement
`$0020` little-endian. The assembly writes the displacement inside the memory operand, but
the encoding places it after every operand byte, per the fixed component order.

### A narrow load

`load.zb @r9 r4` reads one byte and zero-extends it into r4. Its class is `op r r`, so it is
three bytes:

    $81 $09 $04

The opcode carries the access width and the extension rule, so neither operand byte needs a
width field. This is the boundary the register model fixes: width rides the memory
operation, and no memory operation targets a register slice.

### A positional extract

`extract.zb r3.b5 r7` takes byte 5 of r3 and writes it zero-extended into r7. Its class is
`op r r`, so it is three bytes:

    $A0 $A3 $07

The opcode `$A0` declares its first operand slot byte-sliced. The operand byte `$A3` is
`%101` in the form field, which is byte index 5, over register number `$03`. The second slot
is plain, so `$07` is r7 with a zero form field.

### A general bitfield extract

`bitfield_extract r5 #12 #5 r6` takes the 5 bits starting at bit 12 of r5 and writes them
zero-extended into r6. Its class is `op r r i1 i1`, so it is five bytes:

    $A9 $05 $06 $0C $05

The two operand bytes are plain slots naming r5 and r6. The two 1-byte immediates are the
bit position `#12` and then the bit width `#5`, in the order the inventory entry lists them.

### A fused compare-and-branch

`branch_lt_signed r4 r5 loop_top` branches when r4 is signed-less-than r5. Its class is
`op r r i4`, so it is seven bytes. With a displacement of -24 bytes it assembles to:

    $62 $04 $05 $E8 $FF $FF $FF

The displacement is a signed 32-bit byte count relative to the address of the instruction
following the branch, stored little-endian, so `$FFFFFFE8` is -24.

### A 64-bit constant

`move.w $1122334455667788 r10` materializes a full word. Its class is `op r i8`, so it is ten
bytes:

    $02 $0A $88 $77 $66 $55 $44 $33 $22 $11

The opcode `$02` is the 64-bit immediate form of `move`, the operand byte names r10, and the
eight immediate bytes are little-endian.

### A zero-operand instruction

`return` transfers control to the address in r31. Its class is `op`, so it is one byte:

    $74

## Conformance notes

The following properties are directly testable by a binary, and a conforming machine
exhibits all of them.

- Decoding each of the 256 primary opcode bytes yields either the length this specification
  assigns or the illegal-instruction trap, and never any other length.
- Every reserved byte in the opcode map raises the illegal-instruction trap with the
  faulting address equal to that byte's own address.
- Every escape byte raises the illegal-instruction trap on a base-only machine, and the byte
  following the escape byte is never fetched.
- A plain slot with a nonzero form field raises the illegal-operand trap, for every
  instruction that has a plain slot.
- A quarter-word-sliced slot with form field `%100` and a half-word-sliced slot with form
  field `%010` both raise the illegal-operand trap.
- An instruction placed at an address such that its last byte is the last accessible byte of
  a mapped page decodes and executes normally, and one that runs past the end of a mapped
  page raises a page fault reporting the first inaccessible address.
