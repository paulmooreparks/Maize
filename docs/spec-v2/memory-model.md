# Memory Model

This chapter is normative except where a section says otherwise. It defines the memory a
Maize v2 program sees, which means the byte order of every value the machine reads or writes,
the shape of the address space, how much memory exists and how software learns that, what
happens at every alignment, what the machine guarantees about indivisibility, and why devices
are not reachable through a load or a store.

Address translation is not defined here. The privileged-architecture chapter owns the page
table format, the translation walk, and the faults translation raises. This chapter describes
memory as a program experiences it, and it applies whether translation is enabled or not.

## Byte order

Maize v2 is little-endian, and there is no byte-order control anywhere in the architecture.
A multi-byte value occupies consecutive ascending addresses with its least significant byte
at the lowest address, and this holds for every category of multi-byte value the machine
touches.

- Data written by `store`, `store.q`, and `store.h`, and read by the corresponding loads.
- Immediates embedded in an instruction, as the instruction-encoding chapter states.
- The bytes of an instruction itself, which the machine fetches in ascending address order
  starting at the opcode byte (or at the escape byte when the instruction sits on an
  extension page).
- Every field of the boot-information block defined below, and every architectural structure
  any other chapter defines in memory.

The machine offers no mode bit, no control and status register, and no instruction prefix
that changes byte order for any of these. Software that needs the other order uses
`byte_reverse` or `byte_reverse.h`, which are in the base for exactly that purpose.

Storing a word and then reading its bytes back individually is therefore fully determined.
The sequence below stores `$1122334455667788` at the address in r9 and leaves r10 holding
`$88`, on every conforming machine.

    move.w $1122334455667788 r2
    store r2 @r9
    load.zb @r9 r10

## The address space

A Maize v2 program addresses memory with a 64-bit unsigned byte address, and every one of
the 2^64 addresses is a legal address to name. The space is flat, with no segment, no
selector, no bank, and no window, and an address computed by any means denotes the same byte
as the same address computed by any other means. Address arithmetic wraps modulo 2^64, so a
displaced access whose sum exceeds the top of the space wraps to the bottom rather than
faulting on the arithmetic; whether the resulting address is accessible is a separate
question answered by translation and by the physical memory bounds.

Naming an address is not the same as reaching one. When translation is enabled, the
privileged-architecture chapter's rules decide which virtual addresses resolve and which
raise a page fault. When translation is disabled, an address is a physical address and the
physical memory bounds below decide the same question. In both cases every address has a
defined outcome, and an inaccessible address raises a fault reporting the offending address
rather than reading an arbitrary value.

The program counter lives in this same space with the same rules. Instruction fetch is an
ordinary read for the purposes of accessibility, so fetching from an inaccessible address
raises a fault the same way a load does, and the instruction-encoding chapter fixes what a
fetch that straddles a boundary does.

## Physical memory

Guest physical memory is bounded and configurable. A machine is built or launched with some
quantity of memory, that quantity is fixed for the life of the machine, and it is not a
constant of the architecture. Two conforming machines running the same program may have
different amounts, and the program is expected to ask rather than assume.

Software learns the quantity, and the layout it is arranged in, by reading the
boot-information block. Software does not learn it by probing. The architecture defines no
probe: writing a value to an address and reading it back is not a supported way to find the
top of memory, because an access outside populated memory raises a fault rather than
returning a wrapped, aliased, or discarded result. That fault is the physical-memory fault,
cause 11 in the trap-model chapter's enumeration, and its auxiliary word carries the faulting
physical address. The fault is raised in bare mode and under translation alike, so a probing
loop faults instead of producing an answer no matter how the machine is configured.

Within the populated regions the memory is uniform. Every byte behaves identically. A store
followed by a load of the same width at the same address returns the value stored, with no
address range that reads back a different value, no range that ignores writes, and no range
whose behavior depends on the width of the access. There are no device registers, no
firmware shadow, and no reserved aperture with side effects, because devices live in the
port space described below.

The populated regions need not be contiguous. The address map in the boot-information block
enumerates the populated regions and the holes between them, in ascending address order, and
that enumeration is complete. An address the map does not cover with a populated region is
not populated, and an access to it raises the physical-memory fault.

## The boot-information block

Before the machine executes its first instruction, it writes a boot-information block into
memory and records its address in a read-only control and status register, the
boot-information register. The privileged-architecture chapter assigns that register its
number and states the reset state that makes the address meaningful; at reset, translation is
disabled and the recorded address is a physical address. The block is the single discovery
mechanism for everything about the machine's memory and its extension set that the
architecture does not fix as a constant.

The block occupies ordinary memory. The machine reads it once, at construction time, and
never again, so software may overwrite the block after it has finished with the contents, and
doing so changes nothing about the machine. The block starts at an address that is a multiple
of 64, and every field within it is naturally aligned at its stated offset. Every multi-byte
field is little-endian, in keeping with the byte-order rule above.

Field offsets are fixed. A field's offset, size, and meaning never change once this
specification assigns them, and a later version of the block format adds fields at the end
rather than rearranging what is already there. The versioning rules below make that
guarantee precise.

### The header

The header is the first 64 bytes of the block. Offsets below are byte offsets from the start
of the block, and sizes are given in the machine's own vocabulary, where a word is 8 bytes, a
half-word is 4, a quarter-word is 2, and a byte is 1.

| Offset | Size | Field | Contents |
|:-------|:-----|:------|:---------|
| `$00` | 8 bytes | signature | The eight ASCII characters `M`, `A`, `I`, `Z`, `E`, `B`, `I`, `B` in ascending address order. |
| `$08` | quarter-word | format version, major | The major version of the block format. This specification defines major version 1. |
| `$0A` | quarter-word | format version, minor | The minor version of the block format. This specification defines minor version 0. |
| `$0C` | half-word | header length | The size of the header in bytes, which is 64 for format version 1.0. |
| `$10` | half-word | total length | The size of the whole block in bytes, header and tables together. |
| `$14` | quarter-word | address-map entry size | The size in bytes of one address-map entry, which is 32 for format version 1.0. |
| `$16` | quarter-word | extension entry size | The size in bytes of one extension-list entry, which is 32 for format version 1.0. |
| `$18` | word | block address | The address at which the machine placed this block, equal to the value in the boot-information register. |
| `$20` | word | physical memory size | The total number of bytes of populated physical memory, which is the sum of the lengths of the populated regions in the address map. |
| `$28` | half-word | address-map offset | The byte offset from the start of the block to the first address-map entry. |
| `$2C` | half-word | address-map count | The number of address-map entries. This value is at least one. |
| `$30` | half-word | extension-list offset | The byte offset from the start of the block to the first extension-list entry. |
| `$34` | half-word | extension-list count | The number of extension-list entries. This value is at least one, because the base always appears. |
| `$38` | quarter-word | base version, major | The major version of the base instruction set the machine implements. |
| `$3A` | quarter-word | base version, minor | The minor version of the base instruction set the machine implements. |
| `$3C` | half-word | reserved | Zero in format version 1.0. |

The signature is a plain byte sequence rather than an integer so that no reader has to reason
about the byte order of a magic number. The block address at `$18` is redundant with the
register, deliberately: a snapshot, a core dump, or a memory image inspected outside a running
machine carries its own address, so a tool can validate that the image it is reading is the
image the machine was given.

The header length and the two entry sizes exist so that a reader written against one format
version reads a block written to a later one. A reader uses these values rather than the
constants above whenever it walks the block.

### The address map

The address map enumerates the physical address space in ascending order of start address.
Entries do not overlap, and consecutive entries are adjacent, so the map is a partition of the
range from address zero to the highest address any entry covers. Holes appear in the map as
explicit unpopulated entries, which is what makes the map a complete description rather than
a list of the interesting parts.

Each entry is 32 bytes in format version 1.0, laid out as follows, with offsets relative to
the start of the entry.

| Offset | Size | Field | Contents |
|:-------|:-----|:------|:---------|
| `$00` | word | start | The address of the first byte of the region. |
| `$08` | word | length | The length of the region in bytes, which is nonzero. |
| `$10` | half-word | kind | The region's kind, from the list below. |
| `$14` | half-word | attributes | A bitmap of region attributes, from the list below. |
| `$18` | word | reserved | Zero in format version 1.0. |

Five kind values are defined, and every other value is reserved. A reader that meets a
reserved kind treats the region as unavailable rather than guessing, which keeps a
forward-compatible reader safe.

- Kind `#0` marks an unpopulated region, a hole in the physical address space. An access
  anywhere in it raises the physical-memory fault.
- Kind `#1` marks populated memory that is free for software to use.
- Kind `#2` marks populated memory holding the boot-information block itself.
- Kind `#3` marks populated memory holding the image the machine was loaded with, which is the
  boot ROM or the raw program the machine started from.
- Kind `#4` marks populated memory the machine has reserved for its own use, which software does
  not write.

Two attribute bits are defined in format version 1.0, and every other bit is zero. Bit 0 set
means the region is in use at the moment control reaches the first instruction. Bit 1 set
means the region is reclaimable once software has finished with its contents, which is how a
kernel learns that it may recycle the image region or the block's own region after boot.

The physical memory size in the header equals the sum of the lengths of every entry whose kind
is not `#0`. That redundancy is deliberate and directly testable: a conformance binary walks
the map, sums the populated lengths, and compares.

### The extension list

The extension list names every extension the machine implements, with its version, so that
software can make a compatibility decision from a single structure rather than from a bitmap
alone. The feature bitmap in the control and status register space answers the fast question,
which is whether an extension is present at all. The list answers the question the bitmap
cannot, which is which version of it is present.

Each entry is 32 bytes in format version 1.0.

| Offset | Size | Field | Contents |
|:-------|:-----|:------|:---------|
| `$00` | 16 bytes | name | The extension's name in ASCII, padded with zero bytes to 16 bytes. A 16-character name carries no terminator. |
| `$10` | quarter-word | version, major | The extension's major version. |
| `$12` | quarter-word | version, minor | The extension's minor version. |
| `$14` | half-word | opcode page | The escape byte that opens this extension's opcode page, zero-extended, or `$FFFFFFFF` when the extension allocates no opcode page. |
| `$18` | word | reserved | Zero in format version 1.0. |

The first entry is always the base, whose name is `base`, whose version repeats the base
version from the header, and whose opcode page field is `$FFFFFFFF` because the base occupies
the primary page rather than an escape page. Every remaining entry names an implemented
extension. Entries after the first appear in ascending order of the escape byte, with any
entries that allocate no opcode page last, so that the ordering is deterministic and a
conformance binary can compare two machines byte for byte.

An extension that this specification does not name may appear, because the extension registry
grows independently of the base. A reader that does not recognize a name ignores the entry.

### Versioning and growth

The format version at `$08` and `$0A` governs how a reader interprets everything else. A
change to the minor version only appends: it may enlarge the header by defining new fields
above the old header length, it may enlarge an entry by defining new fields above the old
entry size, and it may define new kind values and new attribute bits. It never moves a field,
never changes a field's size, and never redefines what an existing field means. A change to
the major version is free to do any of those, and a reader that does not recognize the major
version stops rather than interpreting the block.

Three rules follow for anyone writing a reader, and following them makes a version 1.0 reader
work unchanged against every later 1.x block.

- Read a field only when its offset plus its size is within the header length, or within the
  entry size for a field inside an entry.
- Advance from one entry to the next by the entry size in the header, never by the size of the
  entry layout the reader was written against.
- Treat an unrecognized kind value, an unrecognized attribute bit, and an unrecognized
  extension name as information to ignore, never as an error and never as a default.

A machine writes the lowest format version that can express its configuration, so a reader
does not meet a later version without a reason.

### Reading the block

The sequence below assumes r2 already holds the block address, read from the
boot-information register with `csr_read`. It loads the physical memory size, then computes
the address of the first address-map entry.

    load @r2+$20 r3          ; physical memory size in bytes
    load.zh @r2+$28 r4       ; address-map offset from the block start
    load.zh @r2+$2C r5       ; address-map entry count
    load.zq @r2+$14 r6       ; size of one address-map entry
    add r2 r4 r7             ; r7 addresses the first entry

Walking to the next entry is `add r7 r6 r7`, and the loop ends when the entry counter reaches
zero. Nothing in the sequence assumes a constant entry size, which is what makes it survive a
minor version bump.

## Alignment

Every memory access is permitted at every address, at every width, with no alignment
requirement and no trap. A word load from an address that is not a multiple of 8 succeeds and
returns the eight bytes at that address in little-endian order, and the same holds for
half-word and quarter-word accesses at any address. This is defined-allow misalignment, and
it applies to loads, to stores, to the block-memory instructions, and to instruction fetch,
which the instruction-encoding chapter already places at byte granularity with no alignment
requirement on branch targets.

A misaligned access that crosses a page boundary is a single access for the purposes of the
architecture, and it may fault on either page. When it faults, the destination register and
memory are unmodified, exactly as the loads-and-stores family states, so the instruction
re-executes cleanly after the fault is serviced. Software never sees a half-completed
misaligned access.

The architecture keeps misaligned access legal because the alternative teaches nothing useful.
A machine that traps on misalignment forces every program that parses a byte stream to
open-code a byte-at-a-time reassembly, and it turns a portability bug into a runtime fault
that is discovered late rather than a design question that is answered early. The cost of the
decision is a performance question rather than a correctness one, and the next paragraph is
where that cost is written down.

**Performance note (non-normative).** A misaligned access is legal but it is not free. On a
software machine a misaligned load is typically two host accesses and a shift-and-merge, and
on a translated one it may prevent the translator from emitting a single host instruction. An
access that crosses a page boundary is more expensive again, because it may require two
translations and can fault partway. Nothing about the correctness of a program depends on
this, and a conformance binary cannot observe it, but a program that aligns its hot data
structures naturally will run measurably faster than one that does not, on every
implementation the design anticipates.

## Atomicity

A naturally aligned access of 8 bytes or fewer is single-copy atomic. An aligned load of that
size returns the value written by exactly one store to the same address, never a mixture of
bytes from two stores, and an aligned store of that size is observed by any other reader as
having happened entirely or not at all. Natural alignment here means an address that is a
multiple of the access width, so an 8-byte access at a multiple of 8, a 4-byte access at a
multiple of 4, a 2-byte access at a multiple of 2, and any 1-byte access.

A misaligned access carries no atomicity guarantee. It may be performed as several smaller
accesses in an unspecified order, and a concurrent reader may observe some of its bytes and
not others. Software that requires indivisibility aligns the datum, and software that requires
indivisibility across a wider region uses the atomic extension.

The block-memory instructions carry no atomicity guarantee of any kind. `block_copy` transfers
bytes in an implementation-chosen order, `block_copy_forward` and `block_set` transfer in
ascending address order, and all three are interruptible partway, as the instruction inventory
states, so a concurrent reader of a region being copied may observe any mixture of old and new
bytes.

The base machine executes one instruction stream, so the observer that these guarantees speak
about is a device, a debugger, a snapshot, or another instruction stream introduced by an
extension. The base defines no memory-ordering instruction because it has nothing to order
against; ordering, fences, and read-modify-write atomics belong to the atomic extension.

## Instruction fetch and stores

Instruction fetch is coherent with stores. A store to an address, followed by a transfer of
control to that address in the same instruction stream, executes the bytes that were stored.
No cache-maintenance instruction exists, no synchronization sequence is required, and there is
no window during which the machine may execute the previous contents. The instruction
inventory states the same rule from the other side, which is that the base spends no opcode on
instruction-cache maintenance because the architecture defines none to be needed.

Coherence covers a store that overwrites part of an instruction as well as one that writes a
whole instruction, and it covers a store to the instruction immediately following the store.
An implementation that caches decoded instructions, or that translates blocks of guest code
ahead of execution, is responsible for noticing the write and discarding what it cached. That
cost is the implementation's to pay, and it is paid here rather than by every program on the
machine, because the alternative is a rule that self-modifying code, a just-in-time compiler
in the guest, a debugger patching a breakpoint, and a loader applying relocations would each
have to remember.

Coherence is a statement about the single instruction stream the base machine executes. An
extension that introduces a second instruction stream states its own rule for stores made by
one stream and fetched by another.

## Memory and devices

Devices are not in memory. Every device the machine carries is reached through the port space,
using `port_in` and `port_out`, and the port space is disjoint from the memory space: a port
identifier is not an address, no load or store can reach a device register, and no device
register can be reached by any means other than the two port instructions.

Two consequences follow, and both are properties a conformance binary can check. There is no
address range carved out of physical memory for device use, so the address map never hides a
device behind a populated region. There is no access to memory whose side effect is anything
other than reading or writing the bytes at that address, so a load never triggers an action and
a store never sends a command.

Keeping devices out of the address space is what lets the previous sections say what they say.
Memory is uniform because nothing in it is special. Misaligned access is universally legal
because no access has to be decomposed differently depending on what it lands on. A load can be
elided, repeated, or reordered by a translator with no risk, because a load has no effect other
than producing its value. The trade is that the port space needs its own instructions and its
own privilege rule, which the instruction inventory already fixes, and that a device with a
large data window transfers through a port rather than through a mapped buffer.

## Conformance notes

The following properties are directly testable by a binary, and a conforming machine exhibits
all of them.

- A word stored at any address, read back one byte at a time in ascending address order,
  yields the value's bytes from least significant to most significant.
- The boot-information register reads as a nonzero address, the eight bytes at that
  address are the signature, and the block address field at `$18` equals the register's value.
- A write to the boot-information register raises the illegal-operand trap, because the
  register is read-only.
- The sum of the lengths of every address-map entry whose kind is not `#0` equals the physical
  memory size field in the header.
- The address-map entries are in ascending order of start address, they do not overlap, and
  each entry begins where the previous one ended.
- The first extension-list entry is named `base`, and its version fields equal the base version
  fields in the header.
- For every extension whose bit is set in the feature bitmap register, an extension-list entry
  exists, and for every extension-list entry after the first, the corresponding bit is set.
- A load and a store of each width at every one of the eight residues modulo 8 succeeds, raises
  no trap, and returns the bytes at the named address.
- A misaligned access spanning two mapped pages succeeds; the same access spanning a mapped
  page and an unmapped one faults and leaves the destination register and memory unmodified.
- A store of instruction bytes followed by a jump to those bytes executes the stored
  instructions, including the case where the store overwrites the instruction that the machine
  is about to fetch next.
- An access to an address covered by an address-map entry of kind `#0`, and an access above the
  highest address the map covers, both raise cause 11 and report the offending physical address
  in the auxiliary word.

## Implementation notes (non-normative)

Nothing in this section is architecture. These notes record how the reference VM is expected
to represent memory and why the normative rules above were chosen to leave that representation
available. An implementation is free to do something else entirely, and a conformance binary
cannot tell the difference.

**A single flat host reservation.** The intended representation of guest physical memory is
one contiguous reservation in the host address space, committed lazily, rather than a map of
small blocks. Letting the host MMU provide sparseness makes a guest access a base-plus-offset
into a flat array in the common case, which is why this chapter defines memory as uniform and
keeps devices out of it: a uniform space needs no per-access dispatch on what the address
lands on.

**Inline translation on the fast path.** The translation path is designed so that a translated
guest load compiles to a tag check, an add, and a host load, with the software translation
cache probed inline rather than through a call. Measurement on the v1 machine found that the
dominant cost of guest memory access was the structure of the translation lookup rather than
the arithmetic of decode, and that multi-way inline probing closed most of the gap between
paged and unpaged execution. The rules above avoid anything that would force a call on the
fast path, which is one reason no memory access has a side effect and no access width is
special-cased.

**Dirty-page tracking.** Tracking which guest pages have been written, using host page
protection or the host's write-watch facility, is treated as first-class in the
implementation rather than bolted on. Incremental snapshot and copy-on-write fork both fall out
of that representation, and the machine-as-a-value goal depends on snapshot completeness being
a property of the representation rather than something reconstructed after the fact.

**Guard pages behind the backend seam.** Bounds-check elimination using host guard pages is an
option the host backend may take, with a portable-C fallback that performs explicit checks. The
seam exists so that the portable reference implementation stays portable while a tuned host
backend stays fast, and neither choice is observable to a guest program.

**A reserved tag plane.** The representation reserves room for a parallel tag plane, one tag
bit per aligned word of guest memory, which the planned capability extension will use to mark
unforgeable pointers. Reserving it now means the extension can attach tags without changing
the memory representation, and the base can stay silent about tags because a base-only machine
has no way to observe them. The same anticipation is why the base defines the address space as
a full 64-bit flat space with no architectural use of the upper address bits: a capability
pointer format wants that headroom, and a base that had already spent the upper bits on
something clever would have foreclosed it.
