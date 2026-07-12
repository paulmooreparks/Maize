# Maize ISA: Device-facing Surface (v1.0 freeze)

This chapter is a normative part of the Maize ISA specification. It fixes the contract by
which a Maize program reaches devices: the port-I/O model, external-interrupt vectoring,
and the standard device set. A third-party VM that follows this chapter accepts the same
device programs and delivers the same interrupts as the reference VM, so the conformance
suite (maize-18) can exercise a device surface without reference to any host backend.

This chapter freezes a **contract**, not an implementation. The surface it describes is
already latent in the reference VM as reserved encoding and partial mechanism; the
chapter's job is to nail that surface into conformance-testable prose so third-party VMs
match this one, not to invent it. The interrupt-controller implementation, the device
plugin API, and the host-backed devices are out of scope and are built, against this frozen
contract, by the implementing cards (the port-I/O and interrupt substrate and controller
by maize-21; the plugin API and host devices by maize-83). Where this chapter says a
mechanism is "deferred to the implementing card," it means the contract is frozen here and
the code that satisfies it lands in maize-21 or maize-83, which must not deviate from what
this chapter fixes.

Ground truth for every encoding and register below is `src/maize_cpu.h` (the opcode map,
the `device` shape, and the RF flag bits), `src/cpu.cpp` (the IN / OUT / OUTR dispatch, the
`devices` port table, SETINT / CLRINT, IRET, and the `run()` delivery seam), `README.md`
(the opcode tables and the privilege and interrupt notes), and the ratified trap model in
`docs/spec/trap-model.md`, which owns the shared vector table, the saved-state layout, and
the IRET return this chapter reuses.

## Coherence with the trap model

External and device interrupts do not define a parallel entry story. They vector through
the **same** table, capture the **same** saved state, and return through the **same** IRET
that the trap model freezes. This single-table, single-frame, single-return design is the
one load-bearing decision of this chapter, and it is the coherence requirement stated from
the interrupt side that the trap model states from the trap side. Concretely, this chapter
inherits from `docs/spec/trap-model.md`:

- One shared vector table indexed by cause number: synchronous traps occupy the low range
  0..31; external and device interrupts occupy the high range 32 and above. The vector
  index of an interrupt is its cause code.
- One saved-state frame. The entry sequence pushes PC, then RF, then the cause word, then
  aux, so the frame reads, from SP upward toward higher addresses (top to bottom): aux,
  cause, RF, PC. The handler pops the two trap-only words (aux, cause) itself, leaving SP
  at the saved RF.
- One return instruction, the shipped IRET, which pops RF and then PC. There is no TRET
  and no IRET variant.
- The privileged-operation fault is cause 4 ("privileged operation in user mode"), whose
  candidate privileged set the trap model records as including IN / OUT. This chapter pins
  that membership for port I/O; the trap model owns the vector number and the enforcement
  mechanism.
- The vector-table base address, the per-entry width, and the index-bounds check are the
  one part of the format the trap model does not freeze; they are co-authored with maize-21
  before v1.0. This chapter fixes only that interrupt vectors live in the high range and
  how many are reserved.

## Surface 1: the port-I/O model

### Separate device address space, no MMIO

Devices are reached only through a dedicated **port space**, never through the memory
address space. No device register is mapped into memory. A conformant VM MUST NOT expose
any device state through `LD`, `ST`, or `CP` memory access, and MUST NOT allow a port
access to read or write ordinary memory. Keeping the two spaces disjoint is what leaves the
flat 64-bit memory model, and the future paging sketch in `docs/spec/reservations.md`, free
of device carve-outs: memory is uniformly sparse RAM, and devices are uniformly ports.

In the reference VM the two spaces are structurally distinct: memory is the sparse
allocate-on-touch block store, and the port space is a separate table (`std::map<u_qword,
device*> devices` in `src/cpu.cpp`) keyed by port id and reached only by the IN / OUT / OUTR
dispatch. There is no path from a memory address to a device or from a port to memory.

### Port space is 16-bit

The port space is a flat numeric namespace of **65,536 ports**, `$0000` through `$FFFF`,
disjoint from memory addresses. A port id is a 16-bit value. This freezes the shipped
convention: the port table is keyed by a 16-bit id, and every dispatch site takes the port
id from the low 16 bits (the `.q0` field) of the port operand, regardless of the encoded
operand width. An implementation MUST mask the port operand to 16 bits; the high 48 bits of
a register-named port are ignored, not an error.

### Instruction set

Port I/O is performed by three instructions, whose already-shipped encodings this chapter
freezes. Each has the four addressing-mode forms the family reserves, selected by the two
high opcode bits (value / immediate and value / memory-address), exactly as the rest of the
ISA. In every form the **port operand** names the port and the **data operand** names the
CPU side of the transfer.

- **OUT (`$14`)** transfers a value from the CPU to the device selected by an **immediate**
  port operand. Forms: `$14` regVal, `$54` immVal, `$94` regAddr, `$D4` immAddr (the data
  source is a register value, an immediate, a value at a register address, or a value at an
  immediate address, respectively; the port is the trailing immediate).
- **OUTR (`$1E`)** is OUT with the port named by a **register** operand rather than an
  immediate. Forms: `$1E` regVal, `$5E` immVal, `$9E` regAddr, `$DE` immAddr. The port id
  is the port register's `.q0`.
- **IN (`$1F`)** transfers a value from the selected device to a register. The port may be
  named by a register or an immediate. Forms: `$1F` regVal, `$5F` immVal, `$9F` regAddr,
  `$DF` immAddr. The transfer direction is device-to-register, the mirror of OUT.

In all twelve forms the port id is the low 16 bits (`.q0`) of the port operand. This
12-form surface (four forms each of OUT, OUTR, and IN) is the frozen v1.0 port-I/O
instruction set; no new port-I/O encoding is introduced.

### Transfer semantics and the device register model

Every standard device is expressed in an abstract **(address, data) register pair**, which
is the shipped device shape (`class device : public reg { reg address_reg; }` in
`src/maize_cpu.h`): a device is a data register (the `reg` backing) plus an `address_reg`
that selects an internal offset or sub-function.

- A **data transfer** moves a value between the CPU operand and the device's data register.
  OUT and OUTR write the CPU-side value into the device data register; IN reads the device
  data register into the destination register. The device side of the transfer is the full
  register width (`w0`) per the shipped dispatch; the CPU side is the operand's selected
  sub-register.
- An **address / sub-function select** is expressed by writing the device's `address_reg`
  through a port write, then reading or writing the data register. Whether a device needs
  the `address_reg` indirection (a windowed device with many internal registers) or presents
  a single flat data register (a byte-at-a-time device) is a per-device property, fixed in
  Surface 3.

A device is free to give a port read and a port write to the same port id different
meanings (for example, reading a status register where writing sends a command); the
per-device register model in Surface 3 states the read and write meaning of each port.

### Privilege

IN, OUT, and OUTR are **privileged** instructions. Executed with the RF privilege bit
(`bit_privilege`) clear, that is in user mode, each raises the **cause-4 "privileged
operation in user mode"** fault. This chapter fixes only the membership: port I/O is not
available to unprivileged code, so a user-mode program cannot touch a device directly and
must go through the kernel. The trap model owns the cause-4 vector number and the general
enforcement mechanism; the enforcement itself (gating the dispatch on the privilege bit) is
deferred to the implementing card, which must apply it to all three instructions. The RF
privilege bit exists today and the machine starts privileged, but no instruction gates on
it yet.

### Unpopulated port

An access to a port with no attached device is a **defined** outcome, never host undefined
behavior. This mirrors the trap model's governing rule that every condition is either a
named trap or an explicitly enumerated defined, non-trapping result, with no third category,
and it mirrors the sparse-memory model where a read of never-written memory returns 0 and a
write is absorbed.

The frozen outcome is **read-0 / write-discard**:

- An **IN** from an unpopulated port yields 0.
- An **OUT** or **OUTR** to an unpopulated port is discarded (a quiet no-op).

Neither traps. This is a conformance-visible defined-behavior guarantee: a third-party VM
MUST return 0 and discard writes on an unpopulated port, so a program that probes the port
space behaves identically on every conforming VM.

The reference VM does not yet satisfy this. Each of the twelve dispatch cases currently
indexes the port table with `devices[id]`, which on a map miss default-constructs a null
`device*` and then dereferences it, a latent crash rather than the defined outcome. Closing
this is deferred to the implementing card (maize-21), which MUST apply the read-0 /
write-discard outcome at **all twelve** port-I/O dispatch sites (the four forms each of
OUT, OUTR, and IN), preferably through a single shared lookup helper, so no form retains
the raw value-initialize-null-then-dereference path. This chapter freezes the outcome; the
code fix is maize-21's.

## Surface 2: external-interrupt vectoring

### One shared table, high vector range

External and device interrupts vector through the trap model's shared table, occupying the
**high vector range 32 and above**; synchronous traps hold the low range 0..31. v1.0
reserves **vectors 32 through 255 as IRQ vectors: 224 interrupt vectors in a 256-entry
table**. Interrupts start at exactly 32 (the first index above the reserved synchronous-trap
range), settled jointly with the trap model's vector-table format. The vector index of an
interrupt **is** its cause code: an IRQ delivered at vector `v` enters the handler at
`entry[v]` with cause `v` (where `32 <= v <= 255`).

### Shared saved state and return

External-interrupt entry reuses the trap model's saved-state frame unchanged. On delivery
the machine pushes PC, then RF, then the cause word, then aux, so the frame reads top to
bottom aux, cause, RF, PC. For an external interrupt:

- **cause** is the delivered vector index (a value 32 or above), packed in the low byte of
  the cause word exactly as a trap cause, with the subcode and high bits reserved-zero
  unless the source defines a subcode.
- **aux** is 0 unless a source defines an IRQ-specific subcode word.

The handler pops its two trap-only words (aux, cause) and returns with the shipped **IRET**,
which pops RF and then PC. Traps and interrupts share one return path; there is no separate
interrupt-return instruction.

### Maskability

External interrupts are **maskable** through the shipped RF interrupt-enable bit
(`bit_interrupt_enabled`), toggled by **SETINT (`$29`)** and **CLRINT (`$69`)**. Delivery is
gated at an instruction boundary: a pending IRQ is delivered only while the interrupt-enable
bit is set. Synchronous traps remain unmaskable per the trap model; the interrupt-enable bit
governs only the maskable external sources and has no effect on a fault or breakpoint.

On delivery the machine **clears the interrupt-enable bit**, so the handler runs with
interrupts masked. Because the pre-interrupt RF, including the enable bit, is saved on the
frame and restored when IRET pops RF, a normal handler return re-enables interrupts
automatically. A handler that wants to accept a further interrupt before it returns does so
explicitly with SETINT.

### Minimal acknowledge contract

This chapter pins the minimal interrupt-acknowledge contract the ISA must promise; the
richer per-line controller is the implementing card's:

1. A device or controller **raises** an IRQ by making a vector pending. The reference VM
   carries the seam as the RF `bit_interrupt_set` latch, which signals the `run()` loop.
2. The CPU, at the next instruction boundary while interrupts are enabled, **acknowledges**
   by clearing the pending latch **before** entering the handler, so the same IRQ is not
   re-delivered on return. Acknowledge-on-delivery is the single deterministic ack this
   chapter fixes.
3. Any **device-level end-of-interrupt or re-arm** (writing an ack register, or reading a
   status port to clear the source) is expressed through the device's own port registers
   (Surface 3) and is co-authored with the implementing card's controller.

The reference VM holds the delivery seam as a commented skeleton in `run()` that sketches a
two-word (RP, RF) push. The implementing card fills that seam and MUST align it to the
four-word aux / cause / RF / PC push order this chapter and the trap model fix, not the
skeleton's older two-word sketch.

### Interrupt model: flat

v1.0 promises a **flat** interrupt model: a single pending source, no preemptive nesting,
and a handler that runs to IRET with interrupts masked unless it explicitly SETINTs. This
matches the shipped single-latch shape. The ISA contract does not promise priority levels:
a conforming third-party VM implements flat delivery, and a program written to the v1.0
contract does not assume that one IRQ can preempt another. The implementing card (maize-21)
may build a prioritized controller with per-line pending and mask and priority registers as
an implementation detail, but that is not an ISA-visible promise, and a program that relies
on priority levels is not portable across conforming VMs.

## Surface 3: the standard device set

v1.0 fixes a **conformance baseline of exactly five mandatory devices**, PLUS a reserved
device-class and port-range convention so future optional devices attach without an ISA
revision. The floor is fixed; the surface is extensible.

Each device is defined **abstractly**: its port-access pattern, its minimal register
skeleton, and its IRQ condition and acknowledge behavior, at the level the conformance suite
(maize-18) needs a third-party VM to implement. The **full** per-device register maps,
including concrete port-number assignments and the block device's fixed logical block size,
are co-authored with maize-83 and land as the device chapters of the ISA document; this
chapter freezes the skeleton, not the pinout. No device definition here references the host
plugin API, native terminal internals, or any concrete host backend; those are maize-83.

Console and framebuffer are **required interrupt-capable** in v1.0. The block device, timer,
and keyboard are interrupt sources as described below. A device that is not interrupt-driven
is still pollable through its status register.

### 1. Console / terminal

The reference device, built in and default, as today. A native terminal remains built in and
default exactly as it is now.

- **Register skeleton:** a byte-wide data register and a status register. The status
  register carries at least an input-available bit and an output-ready bit.
- **Port access:** flat data register (no `address_reg` indirection). OUT writes one byte to
  the output stream; IN reads one byte from the input stream. A program polls the status
  register for readiness.
- **IRQ:** input-available raises an IRQ (the console is required interrupt-capable). The
  handler reads the data register to consume the byte, which clears the input-available
  condition.

### 2. Block device

Random-access storage.

- **Register skeleton:** a block-number register (selecting the logical block, via the
  `address_reg`), a data register or data port for the block payload, and a control / status
  register (start-read, start-write, busy, error). The device has a **fixed logical block
  size**, stated in the contract; the concrete value is co-authored with maize-83.
- **Port access:** `address_reg`-indirected. A program writes the block number, issues a
  read or write through the control register, and transfers the payload through the data
  port.
- **IRQ:** transfer-complete raises an IRQ. The handler reads the status register to observe
  completion and clear the condition.

### 3. Timer

The first interrupt source and the end-to-end proof of the interrupt mechanism.

- **Register skeleton:** a period / reload register, a control register (enable, one-shot
  versus periodic), and a status / acknowledge register.
- **Port access:** small windowed register set. A program programs the period, sets the mode
  and enable, and services and acknowledges ticks through the status / ack register.
- **IRQ:** fires on each tick (periodic mode) or once (one-shot mode). The handler
  acknowledges through the status / ack register to re-arm or clear the source. This is the
  device whose IRQ semantics the conformance suite exercises end to end.

### 4. Framebuffer

Display output.

- **Register skeleton:** dimension and format registers (width, height, pixel format) and a
  pixel-write path.
- **Port access:** because there is **no MMIO**, pixel data reaches the framebuffer through
  the port model, not a mapped memory region: either an (address_reg = pixel or scanline
  offset, data = pixel value) window or a bulk data port. A program never writes pixels
  through `ST`.
- **IRQ:** an optional vsync / frame IRQ (the framebuffer is required interrupt-capable, so
  the vsync path exists; a program that does not use it simply leaves it masked).

### 5. Keyboard

Key input.

- **Register skeleton:** a scancode / data register and a status register (key-available).
- **Port access:** flat data register with a status register. A program polls key-available
  and reads the scancode.
- **IRQ:** key-available raises an IRQ; the handler reads the scancode register, which clears
  the condition.

### Reserved device-class and port-range convention

v1.0 reserves a device-class and port-range convention so an optional device (a mouse, a
network interface, a real-time clock, and so on) attaches without an ISA revision. The five
mandatory devices occupy reserved low-port blocks; the remainder of the 16-bit port space is
held for future device classes assigned by later spec work. Reserving the convention now,
and defining only the five-device floor, is what lets the device set grow within v1.x
without a binary-compatibility break: a v1.0 program that uses only the five mandatory
devices is unaffected by any later device class, because those classes attach in reserved
port ranges the program never touches. The concrete port-range assignment is co-authored
with maize-83 alongside the full per-device register maps.

## Explicitly out of scope

The following stay in the implementing cards, post-freeze, and are built against this frozen
contract. Stating them here keeps the contract's boundary sharp.

- **The interrupt-controller implementation** (maize-21): per-line pending, mask, and
  priority registers, the concrete IRQ-line wiring, the timer as a live source, and filling
  the `run()` delivery skeleton against the four-word aux / cause / RF / PC frame.
- **The device plugin API** (maize-83): the host-side device-model API, port-range
  registration, the shim / passthrough shape, native-code plugin loading, and the fuzzing
  and trust-boundary discipline.
- **The host-backed devices** (maize-83): the real framebuffer, keyboard, timer, and block
  backends and the built-in native terminal. A native terminal remains built in and default
  exactly as today.
- **The vector-table base, entry-width, and index-bounds freeze** (jointly owned with the
  trap model and maize-21): this chapter only places IRQ vectors in the high range and
  reserves their count and start.
- **Closing INT (`$24`) dispatch** and any privileged control-register encoding: the trap
  model and `docs/spec/reservations.md` (the reserved control-register mechanism at base
  slot `$26`) own that surface. This chapter reserves the privileged port-I/O and
  interrupt-control instructions but does not land their enforcement.

## Binary-compatibility statement

This contract introduces **no new ISA-visible encoding**. It freezes already-reserved and
already-dispatched opcodes (`$14` / `$1E` / `$1F` for OUT / OUTR / IN), the shipped RF.H1
flag bits (privilege, interrupt-enabled, interrupt-set, running), the shipped IRET / SETINT
/ CLRINT semantics, the shipped `device` (address_reg, data) shape, and the shared vector
table the trap model defines. It is a specification of existing and reserved surface, not a
compatibility break, and it requires no change to `src/`, `mazm`, or `mzdis`. The one code
change this surface implies, applying the read-0 / write-discard unpopulated-port outcome at
the twelve dispatch sites and gating IN / OUT / OUTR on the privilege bit, is behavior the
reference VM does not yet exhibit and is delivered by the implementing card (maize-21)
against this frozen contract, not on this card.

Any change that would alter an encoding, for example a different port-space width, would be
a binary-compatibility break and would have to be flagged as such when the decision lands.
No such change is made here.

## Cross-references

- **maize-15** (ISA specification v1.0 document): consumes this chapter as the device-facing
  surface. This chapter blocks maize-15; the device chapters cannot be written into the ISA
  document until this contract is ratified.
- **docs/spec/trap-model.md** (maize-78): owns the shared vector table, the saved-state
  frame, the IRET return, and the cause-4 privileged-operation fault this chapter reuses. The
  two chapters share one table, one frame, and one return path.
- **docs/spec/reservations.md** (maize-84): owns the reserved control-register mechanism and
  the forward-compatibility guarantee; the privileged port-I/O and interrupt-control
  instructions named here are consistent with its privilege-and-syscall reservations.
- **maize-21** (interrupt and port substrate, controller, and timer): the implementing card
  for Surfaces 1 and 2. It fills the delivery seam, gates IN / OUT / OUTR on privilege, and
  applies the unpopulated-port outcome at the twelve dispatch sites.
- **maize-83** (device plugin API and host devices): co-authors the full per-device register
  maps and the concrete port-range assignments, and delivers the host backends.
- **maize-18** (conformance suite): exercises the port-I/O, external-interrupt, and
  standard-device surface this chapter fixes.
