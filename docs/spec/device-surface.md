# Chapter 11: Device-facing Surface

This chapter is a normative part of the Maize ISA specification. It fixes the contract by
which a Maize program reaches devices: the port-I/O model, external-interrupt vectoring,
and the standard device set. A third-party VM that follows this chapter accepts the same
device programs and delivers the same interrupts as the reference VM, so the conformance
suite can exercise a device surface without reference to any host backend.

This chapter freezes a **contract**, not an implementation. The surface it describes is
latent in the reference VM as reserved encoding and partial mechanism; the chapter's job
is to nail that surface into conformance-testable prose so third-party VMs match this one.
The interrupt-controller implementation, the device plugin API, and the host-backed
devices are out of scope and are built against this frozen contract by later work. Where
this chapter says a mechanism is "deferred to the implementing extension", it means the
contract is frozen here and the code that satisfies it lands later, and must not deviate
from what this chapter fixes.

Ground truth for every encoding and register below is the reference VM (`src/maize_cpu.h`
for the opcode map, the `device` shape, and the RF flag bits; `src/cpu.cpp` for the IN /
OUT / OUTR dispatch, the `devices` port table, SETINT / CLRINT, IRET, and the `run()`
delivery seam), cross-checked against the repository README opcode tables and the Trap
Model chapter, which owns the shared vector table, the saved-state layout, and the IRET
return this chapter reuses.

## Coherence with the trap model

External and device interrupts do not define a parallel entry story. They vector through
the **same** table, capture the **same** saved state, and return through the **same** IRET
that the trap model freezes. This single-table, single-frame, single-return design is the
one load-bearing decision of this chapter, and it is the coherence requirement stated from
the interrupt side that the trap model states from the trap side. Concretely, this chapter
inherits from the Trap Model chapter:

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
  candidate privileged set includes IN / OUT. This chapter pins that membership for port
  I/O; the trap model owns the vector number and the enforcement mechanism.
- The vector-table format is pinned with the interrupt delivery mechanism: a fixed base
  at `0x1000`, 8-byte entries holding a full 64-bit handler address, 256 entries (2 KiB)
  indexed 0..255 by cause number. This chapter fixes that interrupt vectors live in the
  high range 32..255; the trap model owns the full format. An out-of-range or uninstalled
  (zero) entry is a deterministic halt with the cause surfaced.

## Surface 1: the port-I/O model

### Separate device address space, no MMIO

Devices are reached only through a dedicated **port space**, never through the memory
address space. No device register is mapped into memory. A conformant VM MUST NOT expose
any device state through `LD`, `ST`, or `CP` memory access: a port access reads or writes
device state, not ordinary memory, and a memory access never touches a device register.
Keeping the two spaces disjoint is what leaves the flat 64-bit memory model, and the future
paging sketch in the Reserved Space chapter, free of device carve-outs: memory is uniformly
sparse RAM, and devices are uniformly ports.

This no-MMIO rule governs device *registers*, not a device's own use of guest memory as a
bulk data buffer. A device MAY, on an explicit port command, perform a bounded transfer
between the device and a region of ordinary guest RAM whose base address the guest
registered through a port, DMA-style. This is distinct from MMIO: no device register is
mapped into memory, the buffer is ordinary sparse RAM with no device side effects on a
`LD` / `ST` / `CP` to it, and the transfer happens only when the guest writes the command
port, never implicitly on a memory access. The memory-backed framebuffer's present command
(Surface 3) is the first such transfer; the control plane stays in ports while the bulk data
plane lives in RAM.

In the reference VM the two spaces are structurally distinct: memory is the sparse
allocate-on-touch block store, and the port space is a separate table (`std::map<u_qword,
device*> devices` in `src/cpu.cpp`) keyed by port id and reached only by the IN / OUT / OUTR
dispatch. There is no path from a memory address to a device or from a port to memory.

### Port space is 16-bit

The port space is a flat numeric namespace of **65,536 ports**, `$0000` through `$FFFF`,
disjoint from memory addresses. A port id is a 16-bit value. This freezes the shipped
convention: the port table's key type is a 64-bit word, but every dispatch site truncates
the port operand to its low 16 bits (the `.q0` field) regardless of the encoded operand
width, so the effective port id is 16-bit. An implementation MUST mask the port operand to
16 bits; the high 48 bits of a register-named port are ignored, not an error.

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
- **IN (`$1F`)** transfers a value from the selected device into a register; the four forms
  vary how the **port** is named, and the data destination is always a register. Forms:
  `$1F` regVal (the port id is a register value), `$5F` immVal (the port id is an
  immediate), `$9F` regAddr (the port id is fetched from memory at the address in a
  register), `$DF` immAddr (the port id is fetched from memory at an immediate address). In
  the two address forms the port id is the value read from memory, not the address itself.
  The transfer direction is device-to-register, the mirror of OUT.

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

**Reference-VM note.** The `$94` OUT form (out_regAddr_imm, "value at the address in the
source register") writes the value loaded from the source address, matching the transfer
semantics above and its sibling address forms (`$D4` OUT immAddr, and `$9E` / `$DE` OUTR
regAddr / immAddr). The conformance suite tests this transfer against the contract.

### Privilege

IN, OUT, and OUTR are **privileged** instructions. Executed with the RF privilege bit
(`bit_privilege`) clear, that is in user mode, each raises the **cause-4 "privileged
operation in user mode"** fault. This chapter fixes only the membership: port I/O is not
available to unprivileged code, so a user-mode program cannot touch a device directly and
must go through the kernel. The trap model owns the cause-4 vector number and the general
enforcement mechanism. The reference VM enforces the gate on all three instructions: the
machine starts privileged, and user mode is reached only by an IRET that restores an RF
word with the privilege bit clear.

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

The reference VM satisfies this at all twelve dispatch sites (the four forms each of OUT,
OUTR, and IN) through a single shared port-table lookup helper: a map miss returns a null
device and the caller applies read-0 (IN) or write-discard (OUT / OUTR), so no form retains
the earlier value-initialize-null-then-dereference path that crashed on an unpopulated
port.

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
richer per-line controller is a downstream implementation detail:

1. A device or controller **raises** an IRQ by making a vector pending. The reference VM
   carries the seam as the RF `bit_interrupt_set` latch, which signals the `run()` loop.
2. The CPU, at the next instruction boundary while interrupts are enabled, **acknowledges**
   by clearing the pending latch **before** entering the handler, so the same IRQ is not
   re-delivered on return. Acknowledge-on-delivery is the single deterministic ack this
   chapter fixes.
3. Any **device-level end-of-interrupt or re-arm** (writing an ack register, or reading a
   status port to clear the source) is expressed through the device's own port registers
   (Surface 3) and is settled with the controller.

The reference VM realizes this delivery seam at the instruction boundary in `run()`,
against the four-word aux / cause / RF / PC push order this chapter and the trap model fix.
A pending, enabled IRQ is acknowledged before the frame is built, so the same IRQ is not
re-delivered on IRET; delivery clears the interrupt-enable bit, and the saved RF restores
it on return.

### Interrupt model: flat

v1.0 promises a **flat** interrupt model: a single pending source, no preemptive nesting,
and a handler that runs to IRET with interrupts masked unless it explicitly SETINTs. This
matches the shipped single-latch shape. The ISA contract does not promise priority levels:
a conforming third-party VM implements flat delivery, and a program written to the v1.0
contract does not assume that one IRQ can preempt another. A downstream controller may build
a prioritized model with per-line pending and mask and priority registers as an
implementation detail, but that is not an ISA-visible promise, and a program that relies on
priority levels is not portable across conforming VMs.

## Surface 3: the standard device set

v1.0 fixes a **conformance baseline of exactly five mandatory devices**, PLUS a reserved
device-class and port-range convention so future optional devices attach without an ISA
revision. The floor is fixed; the surface is extensible.

Each device is defined **abstractly**: its port-access pattern, its minimal register
skeleton, and its IRQ condition and acknowledge behavior, at the level the conformance suite
needs a third-party VM to implement. The **full** per-device register maps, including
concrete port-number assignments and the block device's fixed logical block size, are
settled with the device-plugin work and land as the device chapters of the ISA document;
this chapter freezes the skeleton, not the pinout. No device definition here references the
host plugin API, native terminal internals, or any concrete host backend.

Console and framebuffer are **required interrupt-capable** in v1.0. The block device, timer,
and keyboard are interrupt sources as described below. A device that is not interrupt-driven
is still pollable through its status register.

### 1. Console / terminal

The reference device, built in and default. A native terminal remains built in and default.

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
  size**, stated in the contract; the concrete value is settled with the device-plugin work.
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

- **Register skeleton:** dimension and format registers (width, height, pixel format), a
  base-address register, and a present command register.
- **Port access:** the framebuffer is **memory-backed**, not register-per-pixel. The pixel
  buffer lives in a region of ordinary guest RAM: the program writes pixels with normal
  `ST` / `CP` stores at full speed, with no per-pixel port traffic. The control plane is
  ports. The program reads the host-configured width, height, and pixel format (read-only
  per-run host configuration), writes the guest base address of its pixel buffer to the
  base register, fills the buffer, and writes the present register to signal a completed
  frame. On present the device reads the buffer, `[base, base + width * height *
  bytes_per_pixel)`, from guest memory and displays it. This is **not** MMIO: the pixel
  memory has no device side effects (a store there is an ordinary store), and the device
  reads it only on the explicit present command (Surface 1's DMA carve-out). A present
  with an unregistered or out-of-range base is a defined, non-trapping invalid present. The
  buffer size is fixed by the host resolution, never guest-controlled.
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
port ranges the program never touches. The concrete port-range assignment and per-device
register maps are given below.

### Concrete pinout and per-device register maps (v1.0)

The five mandatory devices occupy a reserved low-port block below `$0080`, so a natural
8-bit immediate port operand reaches them without the immediate sign-extension that would
push the low-16-bit port id (the `.q0` field) into the high range. Ports at or above
`$0080` remain reachable only via a 16-bit immediate or a register-named port. The ratified
pinout:

    Port         Device / register           R / W meaning
    ----         -------------------------   ------------------------------------------
    $00          console data                R: next input byte    W: output byte
    $01          console status              R: bit0 input-available, bit1 output-ready
    $10          keyboard data               R: scancode (read clears key-available)
    $11          keyboard status             R: bit0 key-available
    $20 - $22    block device                reserved (no backend in this revision)
    $40          timer period                W: reload value (instruction ticks)
    $41          timer control               W: bit0 enable, bit1 periodic
    $42          timer status / ack          R: bit0 tick-pending;  W: ack
    $50          framebuffer width           R: pixels (host config)
    $51          framebuffer height          R: pixels (host config)
    $52          framebuffer format          R: format id (1 = XRGB8888)
    $53          framebuffer base            R/W: guest address of the SELECTED slot's pixel buffer
    $54          framebuffer present         W: present the selected slot's frame;  R: bit0 last-present-valid
    $55          framebuffer status          R: bit0 vsync-pending, bit2 register-rejected;  W: vsync-IRQ-enable / ack
    $56          framebuffer slot            R/W: select the target slot (0..7) for $53/$54/$55; reset 0
    $57          framebuffer activate        R/W: W switch the active (scanned-out) surface;  R the active slot / console sentinel

    IRQ vectors: timer 32, console input-available 33, keyboard key-available 34,
                 block transfer-complete 35 (reserved), framebuffer vsync/refresh 36.

**Console.** `OUT $00` emits a byte to the output stream; `IN $00` reads a byte from the
input stream; `$01` reports output-ready and input-available; input-available raises IRQ
33, and reading `$00` clears it.

**Keyboard.** The scancode register carries raw PC hardware scancodes: the **Set-1 (XT)**
code set. A key press delivers the key's make code; a key release delivers the same code
with bit 7 set (`make | $80`), the break code. A key event latches a scancode at `$10`,
sets `$11` bit0, and raises IRQ 34; reading `$10` consumes the scancode and clears
key-available.

**Block device.** Ports `$20`-`$22` and IRQ 35 are reserved as the block-device range; no
storage backend, logical block size, or filesystem is defined in this revision. A
reachable-but-unbacked access is a defined, non-trapping outcome.

**Timer.** The period, control, and status/ack registers at `$40`-`$42` with IRQ 32, as
already specified: a program programs the period, sets enable and periodic in the control
register, and services and acknowledges ticks through the status/ack register.

**Framebuffer.** Memory-backed (see Surface 3, device 4): `$50`/`$51`/`$52` are the
read-only host-configured width, height, and pixel format (format id 1 is XRGB8888,
`0x00RRGGBB`, 4 bytes per pixel). The program writes the guest base address of its pixel
buffer to `$53`, writes the pixels into that buffer with ordinary stores, and writes `$54`
to present a completed frame; on present the device reads `[base, base + width * height *
4)` from guest memory. Reading `$54` returns whether the last present was valid (bit0). The
resolution is host configuration for the run; the vsync/refresh IRQ (vector 36) exists at
`$55` but generation is disabled by default.

**Framebuffer registration table.** The framebuffer holds a fixed-size registration table
(8 slots), generalizing a single takeover latch so several guest processes can each register
their own pixel buffer at once (each brings its own guest-RAM buffer; there is no added copy).
Ports `$53`/`$54`/`$55` are *slot-relative*: they act on whichever slot `$56` currently
selects (reset 0, so a program that never touches `$56` sees the single-slot behavior
unchanged). A nonzero base written to `$53` **claims** the selected slot; a zero base
**releases** it. Only one surface is scanned out at a time; `$57` chooses it: writing a
claimed slot's index makes that slot active, and writing the console sentinel (`$FFFFFFFF`)
returns to the text console. Reading `$57` returns the active slot index, or the sentinel
when the console is active. Activation keeps a small history, so releasing the active slot
reverts scanout to the previous surface (a virtual-terminal-style switch-back) rather than
going blank. Width, height, and format are global and apply to every slot identically (no
per-slot resolution). On a display-less view a claim is **rejected**: the slot stays
unclaimed and `$55` bit2 (register-rejected) is set, which lets an operating system return a
per-process error to just that caller instead of the whole machine stopping. Which slot maps
to which host window, and which registration receives input, is host presentation policy
above the device; the device itself knows only slots, never processes.

## Explicitly out of scope

The following stay in downstream implementation work, post-freeze, and are built against
this frozen contract. Stating them here keeps the contract's boundary sharp.

- **A richer interrupt controller:** per-line pending, mask, and priority registers and
  concrete multi-line IRQ wiring. The flat single-latch controller, instruction-boundary
  delivery against the four-word frame, and the timer as the first live source are
  realized; a prioritized per-line controller stays a downstream implementation detail
  that is not an ISA-visible promise.
- **The device plugin API:** native-code (dynamic-load) plugin loading and the associated
  native-code trust boundary. The compile-time, statically-linked host device-model API
  (the port-access hooks, port-range registration, and the shim / passthrough shape) and
  the concrete port-number and IRQ-vector pinout are settled and published above; the timer
  keeps its `$40`-`$42` / vector 32 assignment. Dynamic native-plugin loading stays out of
  scope.
- **The host-backed devices:** the real framebuffer, keyboard, timer, and block backends and
  the built-in native terminal. The instruction-tick timer is deterministic; a real
  host-time backend is part of this work.
- **Closing INT (`$24`) dispatch** and any privileged control-register encoding: the trap
  model and the Reserved Space chapter (the reserved control-register mechanism at base slot
  `$26`) own that surface. Port-I/O privilege enforcement is landed; the INT software-trap
  dispatch and any privileged control-register encoding remain reserved.

## Binary-compatibility statement

This contract introduces **no new ISA-visible encoding**. It freezes already-reserved and
already-dispatched opcodes (`$14` / `$1E` / `$1F` for OUT / OUTR / IN), the shipped RF.H1
flag bits (privilege, interrupt-enabled, interrupt-set, running), the shipped IRET / SETINT
/ CLRINT semantics, the shipped `device` (address_reg, data) shape, and the shared vector
table the trap model defines. It is a specification of existing and reserved surface, not a
compatibility break. The three code changes this surface implies are realized in the
reference VM against this frozen contract, and none of them alters an encoding:

1. Gating IN / OUT / OUTR on the RF privilege bit, so executing them in user mode raises the
   cause-4 privileged-operation fault.
2. Applying the read-0 / write-discard unpopulated-port outcome at all twelve dispatch
   sites, closing the `devices[id]` value-initialize-null-then-dereference crash.
3. Correcting the `$94` OUT form (out_regAddr_imm) so it writes the value loaded from the
   source address rather than the raw source register, matching the transfer semantics and
   its sibling address forms.

Because all three are contract-conforming fixes to behavior a v1.0 binary cannot rely on
being different, none is a compatibility break.

Any change that would alter an encoding, for example a different port-space width, would be
a binary-compatibility break and would have to be flagged as such when the decision lands.
No such change is made here.

## Cross-references

- **Trap Model chapter:** owns the shared vector table, the saved-state frame, the IRET
  return, and the cause-4 privileged-operation fault this chapter reuses. The two chapters
  share one table, one frame, and one return path.
- **Reserved Space chapter:** owns the reserved control-register mechanism and the
  forward-compatibility guarantee; the privileged port-I/O and interrupt-control
  instructions named here are consistent with its privilege-and-syscall reservations.
- **Conformance chapter:** the conformance suite exercises the port-I/O, external-interrupt,
  and standard-device surface this chapter fixes.
