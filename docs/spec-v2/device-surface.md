# Device Surface

This chapter is normative. It fixes the contract by which Maize v2 software reaches devices:
the port space and the two instructions that reach it, the rule that keeps device state out
of the memory space, the bounded exception that lets a device move bulk data through guest
memory, the outcome of touching a port no device answers, the way a device raises an
interrupt, and the standard device classes with their port-level contracts.

The surface is small on purpose. A second implementer writing a Maize machine from this
chapter alone accepts the same device programs and delivers the same interrupts as any other
conforming machine, and the conformance suite exercises the whole surface without naming a
host backend, a windowing system, or an operating system.

## The port space

Devices live in a port space that is numerically and structurally disjoint from the memory
space. A port identifier is a 16-bit value, so the space holds 65,536 ports, `$0000` through
`$FFFF`. A port number is not an address, no arithmetic relates a port number to a memory
address, and the two spaces never alias.

Two instructions reach the space, and no other instruction does.

| Mnemonic | Operands | Effect |
|:---------|:---------|:-------|
| `port_in` | `port_in rp rd` | Reads a word from the port named by rp into rd. |
| `port_out` | `port_out rs rp` | Writes the word in rs to the port named by rp. |

The port identifier is the low quarter-word of the port register. The upper 48 bits of that
register are ignored rather than checked, so a port register holding `$00000000DEAD0041`
names port `$0041`. Masking rather than trapping keeps a computed port number, for instance
a class base plus a register offset, free of a range check that would earn nothing.

Both instructions are privileged. Executing either at user level raises the
privileged-operation trap, which the trap-model chapter names and vectors. Unprivileged
software therefore cannot touch a device at all, and every device access in a running system
passes through the kernel.

Every transfer is a whole word in each direction. A device register narrower than a word
reads zero-extended into the full 64-bit destination, and on a write the device ignores the
bits above its own width. This is the v2 adaptation of the v1 device-register model: v1's
port instructions carried the sub-register width of their operand, and v2 has no sub-register
file, so the width belongs to the device rather than to the operand.

A device may give the same port number different meanings for a read and for a write, and
the class contracts below state the read meaning and the write meaning of every port
separately. Reading a write-only port yields zero, and writing a read-only port is discarded.

## No memory-mapped input and output

No device register is reachable through a load, a store, or a block-memory instruction. A
memory access never reads or writes device state, never has a device side effect, and never
observes one. The memory-model chapter's account of memory is therefore complete on its own:
memory is uniformly memory, with no carve-out that behaves differently because a device sits
behind it.

The rule governs device registers. It does not forbid a device from using ordinary guest
memory as a bulk data buffer, and the next section states the bounded form that takes.

## Bulk transfer through guest memory

A device may, on an explicit port command, perform a bounded transfer between itself and a
region of ordinary guest memory whose base address the guest registered through a port. The
framebuffer's present command and the block device's read and write commands are the
transfers the base defines, and a later device class may define others under the same rule.

Four conditions bound every such transfer, and together they are what keeps this from being
memory-mapped input and output by another name.

- The transfer happens only as the direct result of a `port_out` to a command port, never
  implicitly and never as a side effect of a memory access.
- The buffer is ordinary memory. A load or a store to it is an ordinary load or store with no
  device effect, before the command, during the transfer, and after it.
- The base address and the length are fixed before the command by values the guest wrote to
  ports, and the device transfers no byte outside the region they describe.
- The transfer is complete when the machine executes the instruction following the
  `port_out`, unless the class contract states that the command completes asynchronously and
  names the status bit that reports completion.

A transfer whose region is unregistered, whose length is zero, or whose region is not fully
readable and writable by the guest is a defined, non-transferring failure. The device
performs no partial transfer, sets the invalid-request bit in its status port, and raises no
trap. The guest's own accesses to a badly registered buffer still fault or not exactly as the
memory-model chapter says they do, since the buffer is ordinary memory.

The base uses physical addresses for every registered buffer. A device holds no translation
state and consults no page table, so a kernel that has paging on translates before it
registers, and pins the region for the duration of the transfer.

## Unpopulated ports

An access to a port no device answers has a defined outcome on every conforming machine, so a
program that probes the port space behaves identically everywhere.

A `port_in` from an unpopulated port yields zero. A `port_out` to an unpopulated port is
discarded. Neither raises a trap, and neither is affected by which devices the machine does
carry.

Read-zero is what makes presence detection work without a trap handler. Every device class
below places a nonzero identification value at the first port of its block, so a single
`port_in` distinguishes a present class from an absent one.

## Device interrupts

A device raises an interrupt by asserting its interrupt line, and the machine delivers it
through the same trap mechanism, the same frame, and the same return instruction that the
trap-model chapter fixes for every other cause. There is no second entry path, no second
frame shape, and no interrupt-specific return.

This chapter fixes the line assignment and the device-side behavior, and the trap-model
chapter fixes everything else about delivery, including the mapping from a line index to a
cause number, the masking bit in the control and status register space, the point in
execution at which a pending line is taken, and the acknowledgement the machine performs
before it enters the handler.

Each device class owns exactly one interrupt line, and the line index equals the class code
in the class table below. A machine that does not carry a class never asserts that class's
line. Applying the trap-model chapter's rule that a device interrupt's cause number is 32
plus its line index, the timer's class code of 3 gives it line index 3 and cause number 35.

The device side of an interrupt is uniform across classes and has three parts. A device
asserts its line when the condition named in its class contract becomes true and its
interrupt-enable port bit is set. The device keeps the line asserted until the guest clears
the condition, which it does by writing the acknowledgement port or by performing the
class-specific read that consumes the pending event. A device whose interrupt-enable bit is
clear still records the condition in its status port, so every class is fully usable by
polling and no class requires interrupts to be enabled at all.

## The class table

The base defines seven device classes. Each class owns a block of sixteen consecutive ports,
and the block's base port is the class code multiplied by sixteen.

| Class code | Class | Port block | Presence |
|:----------:|:------|:-----------|:---------|
| 1 | Console | `$0010` through `$001F` | Required |
| 2 | Keyboard | `$0020` through `$002F` | Optional |
| 3 | Timer | `$0030` through `$003F` | Required |
| 4 | Block storage | `$0040` through `$004F` | Optional |
| 5 | Framebuffer | `$0050` through `$005F` | Optional |
| 6 | Network | `$0060` through `$006F` | Optional |
| 7 | Entropy | `$0070` through `$007F` | Optional |

A conforming machine carries the console and the timer, because a machine with neither a way
to say anything nor a way to measure time cannot run the conformance suite. Every other class
is optional, and a machine that omits one leaves that class's whole block unpopulated, so
every port in it reads zero and discards writes. A machine that carries a class implements
that class's whole contract; partial implementation of a class is not conforming.

Block `$0000` through `$000F` is the machine block rather than a device class. Port `$0000`
reads a nonzero machine identification word whose low quarter-word is the literal `$4D32`
and whose second quarter-word carries the base specification version, with the major version
in bits 31 through 24 and the minor version in bits 23 through 16, so base 2.0 puts `$0200`
in that quarter-word. A probe of one port therefore establishes that a Maize port space is
present at all and which base version it answers for. Port `$0001` reads a bitmap in which
bit N is set when class N is present. The remaining ports of the machine block are reserved,
and a reserved port in a populated block reads zero and discards writes exactly as an
unpopulated port does.

Ports `$0080` through `$7FFF` are reserved for device classes that later specification work
assigns, and they are unpopulated on a base machine. Ports `$8000` through `$FFFF` are
available to an implementation for devices this specification does not describe. Software
that uses a port in the implementation range is not portable, and no conformance test reads
or writes one.

## The common class skeleton

Every class block lays out its first three ports the same way, so probing, status polling,
and interrupt control are written once and work for every class.

| Offset | Direction | Meaning |
|:------:|:----------|:--------|
| 0 | Read | Identification: class code in the low quarter-word, class contract version in the second quarter-word, remaining bits zero. |
| 1 | Read | Status: bit 0 is the class's primary ready or pending condition, bit 1 is busy, bit 2 is invalid-request, and the class contract names the rest. |
| 1 | Write | Acknowledge: every status bit set in the written value is cleared, and a bit the device holds true remains set. |
| 2 | Read and write | Interrupt control: bit 0 enables the class's interrupt line, and the remaining bits are reserved and read zero. |

Offsets 3 through 15 are class-specific, and the sections below give them. An offset a class
does not define is reserved within a populated block.

Writing a reserved bit of a defined port is discarded rather than trapped, and reading a
reserved bit yields zero. Discarding rather than trapping is the deliberate choice for
device-register bits, because a device is not the instruction stream: a driver that writes a
bit the machine does not implement then degrades to the behavior of a machine without that
bit, instead of faulting.

### The class contract version

The class contract version at offset 0 is a single 16-bit counter, assigned separately for
each class rather than shared across them. It starts at 1 for a class's first ratified
contract and increases by one for each additive change made to that class's contract
afterward. A number once issued for a class is never reused or withdrawn.

An additive change is the only kind of change that increments the counter. A change is
additive when it defines something the class's contract left undefined, such as an offset the
class reserved or a status bit it left unnamed, and leaves everything the contract already
defined meaning what it meant before. A change to a class's contract that is not additive,
such as one that would alter the meaning of an existing port or remove one, does not
increment the counter at all: it retires the class code and assigns a new one, and the new
class's counter starts again at 1. This mirrors the rule the versioning chapter fixes for an
incompatible extension, where the successor takes a new name and a fresh `1.0` rather than a
new major number on the old name.

A guest that reads a contract version higher than the one it was written against proceeds
rather than refuses. A higher number can only mean added capability behind ports the guest
already understands, since an incompatible change never reaches this field at all: it
arrives, if it ever does, as a class code the guest does not recognize, and an unrecognized
class code is absent as far as that guest is concerned. There is no case in which this field
entitles a guest to reject a class it does recognize.

The increment rule also answers a contract version lower than the one a guest was written
against. A lower number means the ports a later version of the contract added are not
present, while every port the lower version does define means what it has always meant. A
guest that uses only what the lower version defines runs unchanged, and a guest that needs a
port added later reads the version before it uses that port.

The version is advisory to software. The machine populates the field and behaves identically
whether the guest inspects it, acts on it, or ignores it entirely.

Every class this specification defines holds its counter at 1 permanently. Base 2.0 does not
revise, and no extension can reach a device class either. What an extension carries is fixed
at ratification, and the extensions chapter's list of those items includes no port
allocation. An extension that assigned or altered a class anyway would be reaching into the
range this chapter reserves for the classes later specification work assigns, or into the
offsets a class leaves reserved, and this chapter fixes both as reading zero on a base
machine, so they would read zero without the extension and read otherwise with it. That is
base behavior made conditional on whether an extension is present, which the extensions
chapter forbids outright. The implementation range above `$7FFF` is different in kind and
leaves this argument intact, since this chapter fixes nothing about what a machine puts
there and no portable program reads it, so a machine that populates it makes no base
behavior conditional on anything.
Nothing in this architecture as specified can therefore increment the field for any of the
seven classes above. The counter is provision for a class that later specification work might
assign; where such work would be recorded and by what process it would happen is not fixed by
this chapter.

## Reset state

Every present class holds one uniform reset state at the instant the first instruction
executes, and the boot chapter relies on it. The interrupt-enable bit of every class is
clear. The busy, invalid-request, overrun, and transfer-error status bits of every class are
clear. No bulk-transfer buffer is registered, so every buffer-address port reads zero. The
event and data conditions read what is true at that instant: no console input byte has been
delivered, no key event is queued, no network frame has been received, and no timer expiry
is pending, so those condition bits are clear, while the console's output-ready bit is set
whenever output can be accepted and the entropy device's data-available bit is set whenever
its source is ready. The timer's counting-enable and periodic bits are clear and its period
reads zero. Block storage has no transfer in flight and no transfer has ever completed, so
its transfer-complete condition is clear, and its block-number and transfer-length ports
read zero. The framebuffer's selected-surface and scanned-out indices are zero, no surface
is claimed, and no frame has been presented, so its frame-pending condition is clear.

## Console

The console is the required character device, and it carries a byte-at-a-time input stream
and a byte-at-a-time output stream.

| Offset | Direction | Meaning |
|:------:|:----------|:--------|
| 3 | Read | The next input byte, zero-extended, consuming it from the input stream. |
| 3 | Write | The low byte of the written word, emitted to the output stream. |

Status bit 0 is input-available, status bit 3 is output-ready, and status bit 4 is
end-of-input. Reading offset 3 when input-available is clear yields zero and consumes
nothing. Writing offset 3 while output-ready is clear discards the byte and sets status bit
5, the overrun bit, so a program that paces itself on output-ready loses nothing and one
that does not can see that it lost something. A console whose output can always accept a
byte holds output-ready permanently set, which is conforming. End-of-input latches once the
input stream is exhausted and stays set thereafter, so software distinguishes a byte that is
not there yet from a byte that will never come.

The interrupt condition is input-available. Reading offset 3 consumes the byte and clears the
condition when no further byte is waiting.

## Keyboard

The keyboard delivers key press and key release events, and it is separate from the console
because a program that wants key transitions cannot recover them from a character stream.

| Offset | Direction | Meaning |
|:------:|:----------|:--------|
| 3 | Read | The oldest queued key event, consuming it from the queue. |

A key event occupies the low half-word of the value read. Bits 15 through 0 hold the key
code, bit 16 is set for a press and clear for a release, and bits 31 through 17 are zero. Key
codes are the usage identifiers of the keyboard and keypad usage page of the USB HID usage
tables, which is a published external standard, so a conforming machine and a conforming
program agree on which key is which without this specification restating a table.

Status bit 0 is event-available. Reading offset 3 when no event is queued yields zero, which
is not a valid event because usage identifier zero is reserved in the source standard. The
queue depth is an implementation property and is at least one event; a machine that overruns
its queue discards the oldest events and sets status bit 5, the overrun bit.

The interrupt condition is event-available, and consuming the last queued event clears it.

## Timer

The timer is the required time source, and it is the device the conformance suite uses to
exercise interrupt delivery end to end.

| Offset | Direction | Meaning |
|:------:|:----------|:--------|
| 3 | Read and write | The reload period in nanoseconds. |
| 4 | Read and write | The mode: bit 0 enables counting and bit 1 selects periodic rather than one-shot. |
| 5 | Read | A monotonic count of nanoseconds since power-on, which never decreases and never wraps within any run. |

Writing the period while counting is enabled takes effect at the next expiry rather than
immediately, so a running periodic timer is reprogrammed without losing a tick. A period of
zero with counting enabled is an invalid request: the device sets the invalid-request status
bit, leaves counting disabled, and raises no trap.

The period and the monotonic count are stated in nanoseconds because a guest that wants real
time needs a unit that does not vary with how fast the machine runs. The resolution a machine
actually delivers is an implementation property, and the monotonic count advances in whatever
increment that resolution produces. Nothing in this contract requires a tick to arrive on
time, only that expiry, status, and the monotonic count agree with each other.

Status bit 0 is expiry-pending, and it is the interrupt condition. Writing the acknowledge
port clears it, which re-arms a periodic timer for the next expiry and leaves a one-shot
timer disarmed.

## Block storage

Block storage is random-access persistent storage addressed in fixed-size logical blocks, and
its payload moves through guest memory rather than through a port.

| Offset | Direction | Meaning |
|:------:|:----------|:--------|
| 3 | Read | The logical block size in bytes, which is a power of two and at least 512. |
| 4 | Read | The capacity of the device in logical blocks. |
| 5 | Read and write | The logical block number at which the next transfer starts. |
| 6 | Read and write | The physical base address of the transfer buffer in guest memory. |
| 7 | Read and write | The transfer length in logical blocks. |
| 8 | Write | The command: 1 reads from the device into the buffer, and 2 writes from the buffer to the device. |

A command whose block number plus length exceeds the capacity, whose length is zero, or whose
buffer is unregistered is a defined, non-transferring failure that sets the invalid-request
status bit. A command issued while the busy status bit is set is refused the same way, and
the transfer already in flight is unaffected.

A command may complete asynchronously. The device sets the busy status bit when it accepts a
command and clears it when the transfer is complete, and status bit 0, transfer-complete, is
the interrupt condition. Software either polls busy or takes the interrupt. Status bit 6 is
the transfer-error bit, which the device sets alongside transfer-complete when the transfer
did not deliver every requested block, in which case the contents of the buffer are the bytes
the device did transfer with the remainder unmodified.

## Framebuffer

The framebuffer presents a rectangular image to a display, and its pixels live in ordinary
guest memory rather than behind a port, so software draws at memory speed with no per-pixel
port traffic.

| Offset | Direction | Meaning |
|:------:|:----------|:--------|
| 3 | Read | The width in pixels. |
| 4 | Read | The height in pixels. |
| 5 | Read | The pixel format identifier, where 1 is XRGB8888 at four bytes per pixel. |
| 6 | Read | The number of surfaces the device provides, which is at least one. |
| 7 | Read and write | The surface index that offsets 8 and 9 act on, which is zero at reset. |
| 8 | Read and write | The physical base address of the selected surface's pixel buffer, where a nonzero value claims the surface and zero releases it. |
| 9 | Write | The present command for the selected surface. |
| 10 | Read and write | The index of the surface currently scanned out. |

Width, height, and format are properties of the machine for the duration of a run, they are
identical for every surface, and guest software does not change them. Software reads them,
allocates a buffer of width times height times the format's bytes per pixel, registers the
buffer's physical base address, draws into it with ordinary stores, and writes the present
port. On present the device reads the whole buffer once and displays it.

Several surfaces exist so that several independent guest programs each register a buffer
without cooperating, and exactly one of them is scanned out at a time. Writing offset 10
chooses which. A machine that provides one surface satisfies this contract completely, since
software that never touches offsets 7 and 10 sees single-surface behavior.

A claim the machine cannot honor, which is what a machine with no display does with every
claim, leaves the surface unclaimed and sets the invalid-request status bit. Reporting the
refusal per claim rather than trapping is what lets an operating system fail one process's
request for the display instead of stopping the machine.

Status bit 0 is frame-pending, which the device sets when it has finished scanning out a
presented frame, and it is the interrupt condition. A program that does not want frame
interrupts leaves the interrupt-enable bit clear and simply presents when it is ready.

## Network

The network device transfers whole link-layer frames between the guest and a network the host
provides, and frames move through guest memory under the bulk-transfer rule.

| Offset | Direction | Meaning |
|:------:|:----------|:--------|
| 3 | Read | The maximum frame length in bytes. |
| 4 | Read and write | The physical base address of the transmit buffer. |
| 5 | Read and write | The physical base address of the receive buffer. |
| 6 | Write | The transmit command, whose value is the length in bytes of the frame in the transmit buffer. |
| 7 | Read | The length in bytes of the frame the device placed in the receive buffer, or zero when none is waiting, consuming that frame. |

A transmit whose length is zero or exceeds the maximum frame length is a defined,
non-transmitting failure that sets the invalid-request status bit. The device copies the
frame out of the transmit buffer before the `port_out` that commands the transmission
completes, so software reuses the buffer immediately.

Status bit 0 is receive-available, and it is the interrupt condition. The device sets it when
it has placed a frame in the receive buffer, and reading offset 7 consumes the frame and
clears the condition when no further frame is waiting. A frame that arrives while one is
already waiting, or while the receive buffer is unregistered, is dropped, and the device sets
status bit 5, the overrun bit.

This contract says nothing about addressing, protocols, or which network the host attaches
the device to. The machine's obligation ends at delivering and accepting frames, and
everything above the link layer is guest software.

## Entropy

The entropy device supplies unpredictable bits, which software cannot derive from anything
else the machine offers, since every other part of the machine is deterministic.

| Offset | Direction | Meaning |
|:------:|:----------|:--------|
| 3 | Read | A word of entropy, consuming it. |

Status bit 0 is data-available, and it is the interrupt condition. Reading offset 3 when
data-available is clear yields zero and is not entropy, so software checks the status port
before it reads. A machine whose entropy source is always ready leaves data-available
permanently set, which is conforming.

The quality of the bits is a property of the machine rather than of the architecture. A
machine states in its conformance claim whether its entropy source is suitable for
cryptographic use, and this specification requires only that successive reads are not a
constant and are not reproducible from the machine's other observable state.

## Conformance notes

The following properties are directly testable by a binary running on the machine, and a
conforming machine exhibits all of them.

- A `port_in` or a `port_out` executed at user level raises the privileged-operation trap and
  performs no transfer.
- A `port_in` from any port in an unpopulated block yields zero, and a `port_out` to it is
  discarded, for every port in the block.
- Port `$0000` reads a value whose low quarter-word is `$4D32` and whose second quarter-word
  holds the base specification version, the major version in its high byte and the minor
  version in its low byte, and port `$0001` reads a
  bitmap whose set bits are exactly the classes whose identification ports read nonzero.
- The identification port of every present class reads its class code in the low quarter-word
  and its class contract version, which is 1 for every class this specification defines, in
  the second quarter-word, and the identification port of every absent class reads zero.
- Writing a status port with a bit set clears that bit when the underlying condition is no
  longer true, and leaves it set when the condition still holds.
- A device with its interrupt-enable bit clear still sets its status bit when its condition
  occurs, and asserts no interrupt line.
- A bulk-transfer command with an unregistered buffer, a zero length, or an out-of-range
  request sets the invalid-request status bit, transfers no byte, and raises no trap.
- The timer's monotonic count read twice, with instructions in between, never returns a
  smaller value the second time.
