# Instruction Reference: Memory, Fields, and Select

This chapter is normative. It gives the full per-instruction reference for the four families
that move data between memory and registers, name pieces of a register, move regions of
memory, and choose between two values without branching. The instruction inventory
summarizes these families in one line each; the entries here state the whole behavior,
including the outcome of every operand value and every encoding a program can present.

Four families appear below. The loads and stores are the only single-access memory
operations in the base. Extract and insert are the only instructions that name a slice of a
register. The block-memory instructions are the only multi-step operations in the base, and
they carry the restartability contract that the trap-model chapter names as a spec-wide
requirement. The select pair is the base's branchless conditional move.

## How to read an entry

Every entry gives the same five labels in the same order, and the family preamble above it
carries the rules the whole family shares rather than repeating them per entry.

- **Syntax** lists every assembly form of the instruction, one per line.
- **Encoding** names the length class from the instruction-encoding chapter and points at the
  band of the opcode-map appendix that assigns the opcode byte.
- **Operation** states what the machine does to every destination, including the extension
  rule and the wrap rule where either applies.
- **Traps** names the traps the instruction raises while executing, or the single word None.
- **Example** gives a short snippet and one sentence about it.

Two rules govern the **Traps** label throughout the chapter, and neither is repeated in an
entry. The illegal-operand trap that the instruction-encoding chapter raises on a malformed
operand byte applies to every instruction here, so a plain slot carrying a nonzero form field
and a sliced slot carrying an out-of-range element index both trap before execution begins,
whatever the entry says. An entry that names no trap raises none during execution for any
operand value.

Register r0 reads as zero and discards writes. An instruction that names r0 as its
destination still performs every other effect it has, including every memory access and every
fault that access can raise, so `load @r9 r0` is a probe of the address in r9 rather than a
no-operation.

## The width boundary

Two different notations name narrow data in Maize v2, and they never overlap. Width lives on
the mnemonic of a memory operation, where `.b`, `.q`, and `.h` name the byte, the
quarter-word, and the half-word, and where `z` or `s` in front of the width letter names the
extension rule for a load. Position lives on the register operand of an extract or an insert,
where `rN.b5`, `rN.q2`, and `rN.h1` name an element inside a register. A bare mnemonic
operates at the full word, which is 64 bits.

The boundary between the two notations is the invariant the register-model chapter states,
and this chapter depends on it rather than restating it. A program that wants a loaded byte
at an interior position writes a load followed by an `insert`, and a program that wants an
interior byte in memory writes an `extract` followed by a `store.b`. The machine has no
encoding for either fusion, which is what keeps every memory access a whole-register operation and keeps every
merge visible at its own instruction.

## Loads and stores

Every instruction in this family performs exactly one memory access, of one, two, four, or
eight bytes, at an address computed from one register and an optional displacement. These
instructions and the block-memory family are the only base instructions that touch memory,
and every operand through which memory is touched carries the `@` sigil, so a reader finds
every access in a program by scanning for one character.

### Addressing

Two addressing forms exist, and the bare form is exactly the displaced form with a
displacement of zero.

- The bare form `@rN` uses the value of rN as the effective address.
- The displaced form `@rN+$disp` adds a signed 16-bit displacement to the value of rN. The
  assembler also accepts `@rN-$disp`, which encodes the negation of the written value.

The machine sign-extends the displacement to 64 bits and adds it to the base register,
discarding any carry out of bit 63, so the effective address is the sum taken modulo 2^64.
Wrapping is an ordinary defined outcome and raises nothing by itself: an address near the top
of the address space plus a positive displacement continues at the bottom, and an access that
begins at address `$FFFFFFFFFFFFFFFF` and covers eight bytes reads or writes seven bytes
starting at address zero. Each byte of such an access is translated on its own, so the usual
page-fault rules apply to every byte of it. There is no indexed form and no scaled form in
the base, and a second register never contributes to an address.

### Width, extension, and the register written

Access width and the extension rule live entirely in the mnemonic, so no operand byte in this
family carries anything but a register number. A load reads the named width from memory and
writes the full 64-bit destination register: a `z` load writes the loaded bits into the low
end of the destination and zero into every bit above them, and an `s` load copies the top bit
of the loaded value into every bit above it. A bare `load` transfers a whole word and needs no
extension letter. A store reads the low bytes of its source register at the named width,
writes them to memory, ignores the bits above the stored width, and modifies no register at
all.

Memory is little-endian at every width, so the lowest address of a multi-byte access holds
the least significant byte.

### Alignment, atomicity, and faults

Misaligned accesses are permitted at every width and raise no trap for their misalignment.
A naturally aligned access of one, two, four, or eight bytes is single-copy atomic; a
misaligned access carries no atomicity guarantee and an observer may see it in pieces. The
memory-model chapter documents the performance cost of a misaligned access, which
is a cost and not a correctness matter.

Page-fault cleanness is the contract that makes every instruction in this family restartable,
and it holds at every width and both addressing forms.

- A load that raises a page fault writes nothing to its destination register and leaves
  memory untouched.
- A store that raises a page fault writes no byte of memory, including the case where the
  first page of the access is present and writable and the second is not.
- The faulting instruction reports its own first byte as the faulting instruction address, so
  re-executing it after the kernel services the fault produces exactly the result the
  uninterrupted instruction would have produced.

An access that crosses a page boundary is one access for fault purposes rather than two. When
more than one byte of the access is inaccessible, the fault reports the lowest inaccessible
address the access covers, which makes the reported address a function of the access alone
and not of the order in which an implementation touches bytes.

Instruction fetch is coherent with stores. A program that stores bytes and then transfers to
them executes what it wrote, with no cache-maintenance instruction and no synchronization
sequence.

### load

**Syntax:**

    load @rb rd
    load @rb+$disp rd

**Encoding:** `op r r` bare and `op r r i2` displaced, in the loads-and-stores band of the
opcode-map appendix. Both operand slots are plain.

**Operation:** The machine computes the effective address from rb and the displacement, reads
the eight bytes at that address as a little-endian word, and writes that word into all 64
bits of rd. No extension is involved, because the access width equals the register width.
When rd and rb name the same register, the loaded value replaces the address after the access
completes.

**Traps:** Page fault, on any inaccessible byte of the access.

**Example:**

    load @r30+$20 r5

The example reads the word 32 bytes above the address in r30 into r5.

### load.zb

**Syntax:**

    load.zb @rb rd
    load.zb @rb+$disp rd

**Encoding:** `op r r` bare and `op r r i2` displaced, in the loads-and-stores band of the
opcode-map appendix. Both operand slots are plain.

**Operation:** The machine reads one byte from the effective address, writes it into bits 7
through 0 of rd, and writes zero into bits 63 through 8.

**Traps:** Page fault, on an inaccessible byte.

**Example:**

    load.zb @r9 r4

The example reads one byte through the pointer in r9 and leaves r4 holding a value between 0
and 255.

### load.sb

**Syntax:**

    load.sb @rb rd
    load.sb @rb+$disp rd

**Encoding:** `op r r` bare and `op r r i2` displaced, in the loads-and-stores band of the
opcode-map appendix. Both operand slots are plain.

**Operation:** The machine reads one byte from the effective address, writes it into bits 7
through 0 of rd, and copies bit 7 of that byte into bits 63 through 8.

**Traps:** Page fault, on an inaccessible byte.

**Example:**

    load.sb @r9+$01 r4

The example reads the byte one above the pointer in r9 and leaves r4 holding a value between
-128 and 127.

### load.zq

**Syntax:**

    load.zq @rb rd
    load.zq @rb+$disp rd

**Encoding:** `op r r` bare and `op r r i2` displaced, in the loads-and-stores band of the
opcode-map appendix. Both operand slots are plain.

**Operation:** The machine reads two bytes from the effective address as a little-endian
quarter-word, writes them into bits 15 through 0 of rd, and writes zero into bits 63 through
16.

**Traps:** Page fault, on any inaccessible byte of the access.

**Example:**

    load.zq @r6 r7

The example reads a 16-bit unsigned field through the pointer in r6.

### load.sq

**Syntax:**

    load.sq @rb rd
    load.sq @rb+$disp rd

**Encoding:** `op r r` bare and `op r r i2` displaced, in the loads-and-stores band of the
opcode-map appendix. Both operand slots are plain.

**Operation:** The machine reads two bytes from the effective address as a little-endian
quarter-word, writes them into bits 15 through 0 of rd, and copies bit 15 of that
quarter-word into bits 63 through 16.

**Traps:** Page fault, on any inaccessible byte of the access.

**Example:**

    load.sq @r6+$04 r7

The example reads a signed 16-bit field four bytes into the structure that r6 points at.

### load.zh

**Syntax:**

    load.zh @rb rd
    load.zh @rb+$disp rd

**Encoding:** `op r r` bare and `op r r i2` displaced, in the loads-and-stores band of the
opcode-map appendix. Both operand slots are plain.

**Operation:** The machine reads four bytes from the effective address as a little-endian
half-word, writes them into bits 31 through 0 of rd, and writes zero into bits 63 through 32.
This is the load that pairs with the `.h` arithmetic forms, since those also leave a
zero-extended half-word in the full register.

**Traps:** Page fault, on any inaccessible byte of the access.

**Example:**

    load.zh @r8 r9

The example reads a 32-bit unsigned value through the pointer in r8.

### load.sh

**Syntax:**

    load.sh @rb rd
    load.sh @rb+$disp rd

**Encoding:** `op r r` bare and `op r r i2` displaced, in the loads-and-stores band of the
opcode-map appendix. Both operand slots are plain.

**Operation:** The machine reads four bytes from the effective address as a little-endian
half-word, writes them into bits 31 through 0 of rd, and copies bit 31 of that half-word into
bits 63 through 32. A C compiler emits this form when it loads an `int` that a later
expression uses at full width.

**Traps:** Page fault, on any inaccessible byte of the access.

**Example:**

    load.sh @r8-$08 r9

The example reads a signed 32-bit value eight bytes below the address in r8.

### store

**Syntax:**

    store rs @rb
    store rs @rb+$disp

**Encoding:** `op r r` bare and `op r r i2` displaced, in the loads-and-stores band of the
opcode-map appendix. Both operand slots are plain.

**Operation:** The machine computes the effective address from rb and the displacement and
writes all eight bytes of rs there in little-endian order. No register is modified, and
nothing is read back from memory.

**Traps:** Page fault, on any inaccessible or read-only byte of the access.

**Example:**

    store r4 @r30+$10

The example writes the word in r4 sixteen bytes above the address in r30.

### store.b

**Syntax:**

    store.b rs @rb
    store.b rs @rb+$disp

**Encoding:** `op r r` bare and `op r r i2` displaced, in the loads-and-stores band of the
opcode-map appendix. Both operand slots are plain.

**Operation:** The machine writes bits 7 through 0 of rs to the effective address and ignores
bits 63 through 8. Exactly one byte of memory changes, and no register changes.

**Traps:** Page fault, on an inaccessible or read-only byte.

**Example:**

    store.b r4 @r9

The example writes the low byte of r4 through the pointer in r9.

### store.q

**Syntax:**

    store.q rs @rb
    store.q rs @rb+$disp

**Encoding:** `op r r` bare and `op r r i2` displaced, in the loads-and-stores band of the
opcode-map appendix. Both operand slots are plain.

**Operation:** The machine writes bits 15 through 0 of rs to the effective address in
little-endian order and ignores bits 63 through 16.

**Traps:** Page fault, on any inaccessible or read-only byte of the access.

**Example:**

    store.q r4 @r6+$02

The example writes the low quarter-word of r4 two bytes into the structure that r6 points at.

### store.h

**Syntax:**

    store.h rs @rb
    store.h rs @rb+$disp

**Encoding:** `op r r` bare and `op r r i2` displaced, in the loads-and-stores band of the
opcode-map appendix. Both operand slots are plain.

**Operation:** The machine writes bits 31 through 0 of rs to the effective address in
little-endian order and ignores bits 63 through 32.

**Traps:** Page fault, on any inaccessible or read-only byte of the access.

**Example:**

    store.h r4 @r8

The example writes the low half-word of r4 through the pointer in r8.

## Extract and insert

The instructions in this family are how software names a byte, a quarter-word, a half-word,
or an arbitrary bit range inside a register. None of them touches memory, none of them can
fault on an address, and every one of them is a pure function of its register operands and
its immediates.

A dotted **source** extracts. The instruction produces a fresh full-width value from the
named element and creates no dependency on whatever the destination held before, which is why
an extract is safe to schedule freely. A dotted **destination** inserts. The instruction reads
the destination, replaces the named element, and writes the whole register back, and that
read-modify-write is the only merge site in the instruction set.

All fourteen positional forms are reachable and each has exactly one encoding: eight bytes
`rN.b0` through `rN.b7`, four quarter-words `rN.q0` through `rN.q3`, and two half-words
`rN.h0` and `rN.h1`. Element numbering runs from the least significant end, so `rN.b0` is bits
7 through 0, `rN.b7` is bits 63 through 56, `rN.q0` is bits 15 through 0, and `rN.h1` is bits
63 through 32. The element width comes from the opcode and the element index from the operand
byte's form field, as the instruction-encoding chapter fixes, so an out-of-range index is an
encoding error that traps rather than a value the machine has to interpret.

The general bitfield instructions sit behind the positional forms with an immediate bit
position and an immediate bit width, and the positional forms are their aligned shorthands.
Both immediates are unsigned 8-bit values. Register-driven position and width are not in the
base.

Naming the same register in both operand slots is well defined in every entry of this family:
the machine reads the source value before it writes the destination, so `insert.b r5 r5.b3`
places the low byte of r5 into byte 3 of r5 and leaves the rest of r5 alone.

### extract.zb

**Syntax:**

    extract.zb rs.bN rd

**Encoding:** `op r r`, in the extract-and-insert band of the opcode-map appendix. The first
slot is byte-sliced and the second is plain.

**Operation:** The machine takes byte N of rs, writes it into bits 7 through 0 of rd, and
writes zero into bits 63 through 8. Register rs is unchanged unless rd names it.

**Traps:** None.

**Example:**

    extract.zb r3.b5 r7

The example leaves r7 holding bits 47 through 40 of r3 as a value between 0 and 255.

### extract.sb

**Syntax:**

    extract.sb rs.bN rd

**Encoding:** `op r r`, in the extract-and-insert band of the opcode-map appendix. The first
slot is byte-sliced and the second is plain.

**Operation:** The machine takes byte N of rs, writes it into bits 7 through 0 of rd, and
copies bit 7 of that byte into bits 63 through 8.

**Traps:** None.

**Example:**

    extract.sb r3.b0 r7

The example sign-extends the low byte of r3 into r7.

### extract.zq

**Syntax:**

    extract.zq rs.qN rd

**Encoding:** `op r r`, in the extract-and-insert band of the opcode-map appendix. The first
slot is quarter-word-sliced and the second is plain.

**Operation:** The machine takes quarter-word N of rs, writes it into bits 15 through 0 of rd,
and writes zero into bits 63 through 16.

**Traps:** None.

**Example:**

    extract.zq r3.q3 r7

The example leaves r7 holding the top quarter-word of r3, bits 63 through 48, zero-extended.

### extract.sq

**Syntax:**

    extract.sq rs.qN rd

**Encoding:** `op r r`, in the extract-and-insert band of the opcode-map appendix. The first
slot is quarter-word-sliced and the second is plain.

**Operation:** The machine takes quarter-word N of rs, writes it into bits 15 through 0 of rd,
and copies bit 15 of that quarter-word into bits 63 through 16.

**Traps:** None.

**Example:**

    extract.sq r3.q1 r7

The example sign-extends bits 31 through 16 of r3 into r7.

### extract.zh

**Syntax:**

    extract.zh rs.hN rd

**Encoding:** `op r r`, in the extract-and-insert band of the opcode-map appendix. The first
slot is half-word-sliced and the second is plain.

**Operation:** The machine takes half-word N of rs, writes it into bits 31 through 0 of rd,
and writes zero into bits 63 through 32. The form naming `rs.h0` is the canonical way to
truncate a word to a zero-extended half-word.

**Traps:** None.

**Example:**

    extract.zh r3.h1 r7

The example moves the upper half-word of r3 into the lower half-word of r7 and clears the rest
of r7.

### extract.sh

**Syntax:**

    extract.sh rs.hN rd

**Encoding:** `op r r`, in the extract-and-insert band of the opcode-map appendix. The first
slot is half-word-sliced and the second is plain.

**Operation:** The machine takes half-word N of rs, writes it into bits 31 through 0 of rd,
and copies bit 31 of that half-word into bits 63 through 32.

**Traps:** None.

**Example:**

    extract.sh r3.h0 r7

The example widens the low half-word of r3 to a signed word in r7, which is the register form
of the C conversion from `int` to `long`.

### insert.b

**Syntax:**

    insert.b rs rd.bN

**Encoding:** `op r r`, in the extract-and-insert band of the opcode-map appendix. The first
slot is plain and the second is byte-sliced.

**Operation:** The machine reads bits 7 through 0 of rs, writes them into byte N of rd, and
preserves every other bit of rd. Bits 63 through 8 of rs are ignored, and rs is unchanged
unless it is rd.

**Traps:** None.

**Example:**

    insert.b r4 r3.b5

The example replaces bits 47 through 40 of r3 with the low byte of r4.

### insert.q

**Syntax:**

    insert.q rs rd.qN

**Encoding:** `op r r`, in the extract-and-insert band of the opcode-map appendix. The first
slot is plain and the second is quarter-word-sliced.

**Operation:** The machine reads bits 15 through 0 of rs, writes them into quarter-word N of
rd, and preserves every other bit of rd.

**Traps:** None.

**Example:**

    insert.q r4 r3.q2

The example replaces bits 47 through 32 of r3 with the low quarter-word of r4.

### insert.h

**Syntax:**

    insert.h rs rd.hN

**Encoding:** `op r r`, in the extract-and-insert band of the opcode-map appendix. The first
slot is plain and the second is half-word-sliced.

**Operation:** The machine reads bits 31 through 0 of rs, writes them into half-word N of rd,
and preserves the other half-word of rd. Writing `rd.h0` is the one operation in the base that
changes the low half of a register without disturbing the high half, which is exactly what the
zero-extending `.h` arithmetic rule denies.

**Traps:** None.

**Example:**

    insert.h r4 r3.h1

The example replaces bits 63 through 32 of r3 with the low half-word of r4.

### bitfield_extract

**Syntax:**

    bitfield_extract rs #pos #width rd

**Encoding:** `op r r i1 i1`, in the extract-and-insert band of the opcode-map appendix. Both
operand slots are plain, and the two immediates are stored with the position first and the
width second.

**Operation:** The machine takes the `width` bits of rs beginning at bit `pos`, that is bits
`pos + width - 1` through `pos`, writes them into bits `width - 1` through 0 of rd, and writes
zero into every bit of rd above them. A position of 0 with a width of 64 copies the whole
register.

**Traps:** Illegal-operand, when the width is zero or when the position plus the width exceeds
64. A position of 64 or more is caught by the same test, and neither condition has a defaulted
interpretation.

**Example:**

    bitfield_extract r5 #12 #5 r6

The example leaves r6 holding bits 16 through 12 of r5 as a value between 0 and 31.

### bitfield_extract_signed

**Syntax:**

    bitfield_extract_signed rs #pos #width rd

**Encoding:** `op r r i1 i1`, in the extract-and-insert band of the opcode-map appendix. Both
operand slots are plain, and the position immediate precedes the width immediate.

**Operation:** The machine takes the same field the unsigned form takes and writes it into bits
`width - 1` through 0 of rd, then copies bit `width - 1` of the extracted field into every bit
of rd above it. A width of 64 leaves nothing to extend and copies the whole register.

**Traps:** Illegal-operand, when the width is zero or when the position plus the width exceeds
64.

**Example:**

    bitfield_extract_signed r5 #12 #5 r6

The example leaves r6 holding bits 16 through 12 of r5 as a value between -16 and 15.

### bitfield_insert

**Syntax:**

    bitfield_insert rs #pos #width rd

**Encoding:** `op r r i1 i1`, in the extract-and-insert band of the opcode-map appendix. Both
operand slots are plain, and the position immediate precedes the width immediate.

**Operation:** The machine reads bits `width - 1` through 0 of rs, writes them into bits
`pos + width - 1` through `pos` of rd, and preserves every other bit of rd. Bits of rs at or
above `width` are ignored, so a caller need not mask its source.

**Traps:** Illegal-operand, when the width is zero or when the position plus the width exceeds
64.

**Example:**

    bitfield_insert r6 #12 #5 r5

The example replaces bits 16 through 12 of r5 with the low 5 bits of r6.

## Block memory

The block-memory instructions copy and fill regions of memory of arbitrary length, and they
are in the base because they are machine operations that a translator lowers to a host memory
routine rather than services that an operating system has to provide. They replace the v1
syscall-provided copy and fill outright.

Each of the three instructions names three registers, reads a byte count from one of them,
and can transfer more bytes than any single access covers. A count is an unsigned 64-bit byte
count, and a count of zero is valid, performs no access, raises no fault, and leaves the named
registers as it found them. Pointer arithmetic wraps modulo 2^64 exactly as it does for a
displaced load, so a region that runs off the top of the address space continues at address
zero, and every byte it covers is translated on its own.

### The distinct-register requirement

The three register operands of a block-memory instruction name three distinct registers. An
encoding of `block_copy`, `block_copy_forward`, or `block_set` that names the same register
in more than one of its three operand slots raises the illegal-operand trap. Aliased slots
would make the mid-operation restart state unrepresentable, since a register that has to be
two of pointer, counter, and fill value at once cannot describe the work that remains.

Register r0 is excluded from the slots that carry that state, for the same reason from the
other side: a register that discards writes cannot hold a pointer that must advance, a count
that must reach zero, or the completion state the contract fixes. An encoding that names r0
in a pointer slot or in the count slot of any block-memory instruction raises the
illegal-operand trap with the same subcode as an aliased slot. The one slot that admits r0
is the value slot of `block_set`, because that register carries no operation state, is never
written, and r0 there is the natural spelling of a zero fill. Both rules constrain encodings
rather than values, so they are decided before any byte is transferred.

### The restartability contract

All three instructions are restartable under paging, and the contract below is normative and
directly testable by a conformance binary that arms a page fault partway through a transfer.

The machine may be interrupted between byte transfers, and it chooses the granularity at which
that is possible, so an implementation that moves eight bytes at a time is conforming wherever
chunking cannot change the bytes the operation leaves behind. That covers `block_copy`, whose
transfer order is implementation-chosen, and `block_set`, which writes one repeated value.
It does not cover `block_copy_forward` over regions that overlap, because that instruction's
ascending byte-by-byte result is defined and a chunked copy would leave different bytes in
memory. At every
point at which the machine can be interrupted, the three named registers describe exactly the
work that remains: the count register holds the number of bytes not yet transferred, and each
pointer register holds the lowest address in its region that has not yet been transferred. The
bytes already transferred are therefore exactly the bytes of the region that the remaining
region does not cover, and re-executing the instruction from that register state transfers
every remaining byte, copies no byte twice, and skips none.

Two consequences follow, and both are worth stating outright. A machine that transfers from low
address to high shows its progress by advancing both pointer registers and decrementing the
count together. A machine that transfers from high address to low shows its progress by
decrementing the count alone, because the untransferred bytes are still the low ones and the
pointers still name them. Both behaviors satisfy the same invariant, which is why the contract
is written as a description of the remaining work rather than as a direction of travel.

A page fault during a block-memory instruction reports the address of the block instruction
itself as the faulting instruction address, not an address inside a host memory routine, and
it reports the inaccessible guest address as the faulting data address. The kernel services
the fault and resumes; the instruction re-executes and completes. Bytes already written stay
written, which is why the register state has to be truthful before the fault is delivered.

On normal completion the count register holds zero and each pointer register holds its
original value plus the original count, so it points just past the last byte of its region.
That final state is the same for every implementation and every direction of travel, and it
leaves a repeated execution of the same instruction harmless, since a count of zero transfers
nothing.

### Overlap

Overlap is stated rather than implied, and neither copy instruction leaves any overlap
undefined. `block_copy` gives the result that reading the whole source before writing any of
the destination would give, for every overlap including the completely coincident case.
`block_copy_forward` gives the ascending byte-by-byte result, in which a write can be observed
by a later read of the same operation, and that result is fully defined rather than left to the
implementation. A compiler emits `block_copy` for a C `memmove` and `block_copy_forward` for a
C `memcpy`, and software that needs a specific direction of travel uses
`block_copy_forward` and reasons about it directly.

Single-copy atomicity does not extend across a block-memory operation. The naturally aligned
access inside such an operation is atomic on its own, and nothing guarantees that any two bytes
of the transfer are observed together.

### block_copy

**Syntax:**

    block_copy @rs @rd rn

**Encoding:** `op r r r`, in the block-memory band of the opcode-map appendix. All three slots
are plain, and all three name distinct registers.

**Operation:** The machine copies the number of bytes in rn from the region beginning at the
address in rs to the region beginning at the address in rd, producing the result that reading
the entire source before writing any of the destination would produce, whatever the overlap
between the two regions. The source region is unmodified except where the destination region
covers it. On completion rn holds zero, and rs and rd each hold their original value plus the
original count. Interruption leaves the three registers describing the remaining work, as the
restartability contract above defines.

**Traps:** Page fault, on any inaccessible byte of the source region and on any inaccessible or
read-only byte of the destination region. Illegal-operand, when two of the three operand slots
name the same register or any of the three names r0.

**Example:**

    move.zq $0400 r6
    block_copy @r4 @r5 r6

The example copies 1024 bytes from the address in r4 to the address in r5 with the overlap
behavior of a C `memmove`, and it leaves r6 holding zero.

### block_copy_forward

**Syntax:**

    block_copy_forward @rs @rd rn

**Encoding:** `op r r r`, in the block-memory band of the opcode-map appendix. All three slots
are plain, and all three name distinct registers.

**Operation:** The machine copies the number of bytes in rn from the address in rs to the
address in rd in ascending address order, one byte at a time, so a destination byte written
early in the operation is the value a later source read of that same address returns. On
completion rn holds zero, and rs and rd each hold their original value plus the original count.
Because the direction of travel is architectural here, interruption always leaves both pointer
registers advanced past the bytes already copied and the count register holding the number of
bytes still to copy.

**Traps:** Page fault, on any inaccessible byte of the source region and on any inaccessible or
read-only byte of the destination region. Illegal-operand, when two of the three operand slots
name the same register or any of the three names r0.

**Example:**

    move.zq $0100 r6
    block_copy_forward @r4 @r5 r6

The example copies 256 bytes ascending, which is what a compiler emits when it knows the two
regions do not overlap.

### block_set

**Syntax:**

    block_set rv @rd rn

**Encoding:** `op r r r`, in the block-memory band of the opcode-map appendix. All three slots
are plain, and all three name distinct registers.

**Operation:** The machine writes bits 7 through 0 of rv to the number of consecutive bytes in
rn beginning at the address in rd, in ascending address order. Bits 63 through 8 of rv are
ignored, and rv is unmodified throughout the operation and after it, including across an
interruption. On completion rn holds zero and rd holds its original value plus the original
count. Interruption leaves rd advanced past the bytes already written and rn holding the number
of bytes still to write.

**Traps:** Page fault, on any inaccessible or read-only byte of the region. Illegal-operand,
when two of the three operand slots name the same register or when the pointer slot or the
count slot names r0; the value slot admits r0.

**Example:**

    move.zq $1000 r6
    block_set r0 @r5 r6

The example zeroes 4096 bytes at the address in r5, using r0 as the source of the zero byte.

## Select

The select pair chooses between two values without a branch, and it is destructive in its
destination because that is exactly the host conditional-move contract on both translation
targets. Each instruction reads a condition register, and each either writes the destination
or leaves it entirely alone.

The condition is the whole 64-bit value of the condition register tested against zero. A
single bit is not privileged, so a compare result of 1, a pointer, a character code, and a
count all behave the same way: any value other than all-zero bits is nonzero. Neither
instruction modifies the condition register or the source register.

When the condition does not hold, the destination is not written at all, which matters because
that is the difference between select and a masked arithmetic sequence. Naming the same
register in two slots is well defined, since both sources are read before the destination is
written.

### select_nz

**Syntax:**

    select_nz rs rc rd

**Encoding:** `op r r r`, in the control-transfer-and-select band of the opcode-map appendix.
All three slots are plain.

**Operation:** The machine reads rc. When rc holds any nonzero value, the machine writes all 64
bits of rs into rd. When rc holds zero, rd keeps the value it already had and the machine
writes no register.

**Traps:** None.

**Example:**

    compare_lt_signed r4 r5 r6
    select_nz r5 r6 r4

The example leaves r4 holding the larger of the two original values, which is the branchless
maximum.

### select_z

**Syntax:**

    select_z rs rc rd

**Encoding:** `op r r r`, in the control-transfer-and-select band of the opcode-map appendix.
All three slots are plain.

**Operation:** The machine reads rc. When rc holds zero, the machine writes all 64 bits of rs
into rd. When rc holds any nonzero value, rd keeps the value it already had and the machine
writes no register.

**Traps:** None.

**Example:**

    select_z r7 r6 r4

The example replaces r4 with r7 only when r6 is zero, which is the guard a compiler emits for
a default value on an empty condition.

The three-way select composes from the pair in two instructions. Writing `move rf rd` and then
`select_nz rt rc rd` leaves rd holding rt when rc is nonzero and rf otherwise, at the cost of
one extra instruction and no branch.

## Conformance notes

The properties below are directly testable by a binary, and a conforming machine exhibits all
of them.

- A load or a store whose access spans two pages, the second of which is not present, leaves
  the destination register and every byte of memory unmodified, and reports the lowest
  inaccessible address in the access.
- A load naming r0 as its destination raises the same page fault at the same address that the
  identical load into r1 raises.
- An access at address `$FFFFFFFFFFFFFFF9` of eight bytes touches the seven bytes at the top of
  the address space and the byte at address zero, and faults only when one of those bytes is
  inaccessible.
- Every one of the fourteen positional forms of extract and insert is reachable, and each is
  reachable by exactly one operand-byte encoding.
- A bitfield instruction with a width of zero, and one whose position plus width exceeds 64,
  both raise the illegal-operand trap and modify no register.
- A block-memory instruction naming the same register in two of its three operand slots raises
  the illegal-operand trap and transfers no byte, and so does one naming r0 in a pointer slot
  or in the count slot, while `block_set` with r0 in the value slot executes normally.
- A block-memory instruction interrupted by a page fault leaves the untransferred bytes of each
  region exactly described by its pointer register and its count register, and re-executing it
  after the fault is serviced yields the memory contents that an uninterrupted execution would
  have yielded.
- A block-memory instruction that completes leaves its count register at zero and each pointer
  register at its original value plus the original count, and executing it a second time from
  that state changes nothing.
- A `select_nz` whose condition register holds zero leaves the destination register bit-for-bit
  unchanged, including when the destination is the same register as the source.
