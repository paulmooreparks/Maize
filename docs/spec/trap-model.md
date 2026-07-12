# Maize ISA: Trap Model (v1.0 freeze)

This chapter is a normative part of the Maize ISA specification. It fixes the trap
taxonomy, the cause/vector numbering, the state a trap captures, and how traps and
interrupts share one delivery path. A third-party implementation that follows this
chapter behaves identically to the reference VM on every condition described here.

## The governing rule: no undefined behavior

Every condition that is undefined behavior on a conventional machine is, on Maize, a
defined outcome. There are exactly two kinds of outcome and no third category:

1. A named **trap** with a stable numeric cause, delivered precisely (below), or
2. An explicitly enumerated **defined, non-trapping result** (see "Defined
   non-trapping behavior").

Nothing is left implementation-defined. A binary therefore behaves identically under
analysis and in production, and two conforming VMs cannot diverge on any input.

## Precise delivery

Trap delivery is **precise**, and this is the frozen v1.0 contract. The core is a
strictly in-order interpreter: it fully retires one instruction before decoding the
next. When a synchronous trap fires, every prior instruction has completed and no later
instruction has taken any architectural effect. Precise delivery is what makes a trap
conformance-testable and lets a corrective handler resume.

### Faults versus traps

Maize distinguishes two synchronous-trap classes by the program counter they capture.
The distinction follows the model most readers already know from x86, and is stated
explicitly so it is never folded away:

- A **fault** captures the address of the **faulting** instruction. A handler that
  corrects the condition can return and re-execute that instruction. Illegal-instruction,
  divide-error, privileged-operation, segment/bounds, and stack are faults.
- A **trap** (in the narrow sense) captures the address of the **following**
  instruction, because there is nothing to retry. Breakpoint is a trap.

Implementation note (reference VM): the decoder reads the opcode at `RP` and then
advances `RP` past it before dispatch, and an ALU decode advances `RP` further still.
The captured faulting-instruction PC is therefore the value of `RP` at the *entry* to
the instruction's decode, not the partially advanced value. A conforming VM must stash
each instruction's entry PC so that a mid-instruction fault reports the instruction's
own address. A breakpoint, by contrast, wants the already-advanced `RP` (the following
instruction), which the reference dispatch holds naturally at the point BRK executes.

## The trap taxonomy

Each synchronous trap has a stable numeric **cause**. The cause number doubles as the
trap's index into the shared vector table (see "Vector table and delivery"). Synchronous
traps occupy the low range 0..31; external and device interrupts occupy 32 and above
(maize-21). Vector 1 is intentionally left unassigned, reserving a slot for a future
debug / single-step trap, matching the x86 lineage a reader will expect.

| Cause | Name | Class | Trigger | Captured aux | Status |
|------:|------|-------|---------|--------------|--------|
| 0 | Illegal instruction | Fault | An unknown / undefined opcode, or an unallocated condition encoding on a Jcc / SETcc | Offending instruction byte | Implemented (as throw-and-exit); guest delivery newly pinned |
| 1 | (reserved) | n/a | Unassigned; reserved for a future debug / single-step trap | n/a | Reserved, unassigned |
| 2 | Divide error | Fault | Divide-by-zero (signed or unsigned), or signed `INT_MIN / -1` quotient overflow | Subcode: 0 = divide-by-zero, 1 = quotient overflow | Implemented (as throw-and-exit); guest delivery newly pinned |
| 3 | Breakpoint | Trap | The `BRK` (`$FF`) instruction executes | 0 | Newly pinned (was a no-op) |
| 4 | Privileged operation in user mode | Fault | A privileged instruction executes with the RF privilege bit clear | Offending instruction byte | Reserved number; enforcement mechanism deferred |
| 5 | Segment / bounds violation | Fault | An access falls outside a segment window, once segments exist | Faulting address | Reserved number (maize-92); mechanism deferred |
| 6 | Stack fault | Fault | A stack access violates the stack limit, once a stack bound exists | Faulting address | Reserved number (maize-92); mechanism deferred |
| 7 | (reserved) | n/a | Not spent on misaligned access, which is defined-allow (see below) | n/a | Reserved, unassigned |
| 8..31 | (reserved) | n/a | Future synchronous traps | n/a | Reserved |
| 32.. | External / device interrupts | Interrupt | Device / timer sources (maize-21); the timer is the first source | Source-defined | Deferred to maize-21 |

Cause subcodes: where one cause number multiplexes distinct conditions, the cause word
carries a subcode so a handler can tell them apart without re-deriving the condition.
Divide error uses subcode 0 for divide-by-zero and 1 for signed quotient overflow.
Illegal instruction uses subcode 0 for an unknown opcode and 1 for an unallocated
condition encoding.

### Per-entry detail

**Cause 0, Illegal instruction (fault).** Fires when the decoder meets an opcode byte
with no defined meaning, or when a Jcc / SETcc carries a condition selector that is not
an allocated encoding. It captures the faulting instruction's PC (a handler may rewrite
the byte and retry) and the offending instruction byte as aux. In the reference VM this
is the `default:` case of the opcode dispatch and the `default:` case of the condition
evaluator, both of which currently throw and exit; this chapter pins the guest-visible
delivery those become.

**Cause 2, Divide error (fault).** Fires on signed or unsigned divide-by-zero and on the
one signed quotient overflow (`INT_MIN / -1`, whose true quotient is not representable),
across all widths of DIV, MOD, UDIV, and UMOD. It captures the faulting instruction's PC
and a subcode distinguishing divide-by-zero (0) from quotient overflow (1). This is the
only arithmetic trap; every other arithmetic condition is defined non-trapping (below).

**Cause 3, Breakpoint (trap).** Fires when `BRK` (`$FF`) executes. It is a trap, not a
fault: it captures the address of the **following** instruction, so a debugger that
resumes lands on the instruction after the breakpoint. `$FF` is also the value that
fills erased or uninitialized memory, so a run of `$FF` bytes reached as code raises a
breakpoint rather than wandering, mirroring `HALT` (`$00`) at the other end.

**Cause 4, Privileged operation in user mode (fault, reserved).** The cause number is
frozen now; the enforcement mechanism is deferred. It fires when a privileged
instruction executes with the RF privilege bit clear. The RF privilege bit already
exists but no instruction gates on it yet. The candidate privileged set is IN / OUT,
SETINT / CLRINT, IRET, HALT, and future segment-register writes; the exact set is
finalized jointly with maize-21 (port I/O) and maize-92 (segments). Reserving the number
now leaves stable encoding space.

**Cause 5, Segment / bounds violation (fault, reserved).** The cause number is frozen
now for maize-92; the mechanism ships with the segment / base-bounds registers, possibly
as the first v1.x extension. In the v1.0 flat sparse memory model no access is out of
bounds, so this never fires yet.

**Cause 6, Stack fault (fault, reserved).** The cause number is frozen now; there is no
stack bound in the flat sparse model, so it never fires until bounds exist (maize-92).

## Defined non-trapping behavior

The following conditions look like undefined behavior on a conventional machine but are
first-class **defined outcomes** on Maize. None of them traps, and none of them is host
undefined behavior. They are listed here so no reader mistakes a defined result for a
gap in the taxonomy.

- **Integer overflow wraps.** ADD, SUB, MUL, INC, DEC, NEG, and shift results wrap
  two's-complement and set the C and V flags per the flags model (maize-1). There is no
  trap-on-overflow mode in v1.0. The JO / JNO and SETO / SETNO condition encodings stay
  reserved; overflow is observed through flags, never through a trap.
- **Out-of-range shift count is defined.** For a shift of width `bits`: `n == 0` leaves
  the flags unaffected; `1 <= n <= bits` shifts normally; `n > bits` yields a result of
  0 with C, V, N, and Z cleared. Never a trap, never host undefined behavior.
- **Unmapped / sparse memory access is defined.** Memory is sparse: a read of
  never-written memory returns 0, and a write allocates a zero-filled block on first
  touch. There is no EFAULT and no page fault in the v1.0 flat model. The absence of a
  memory-access trap here is deliberate, not a missing case; segment / bounds
  enforcement (cause 5) is the separate, reserved path that maize-92 adds.
- **Misaligned multi-byte access is defined-allow.** A multi-byte load or store may sit
  at any address; it is stitched byte-wise across the 256-byte allocation blocks with no
  alignment requirement and no trap. No vector is spent on alignment.
- **Decoded-but-undefined operand-field encodings decode to a defined default.** An
  undefined sub-register selector (`$F`) decodes to `b0`; an undefined immediate-size
  encoding (4..7) decodes to the value-initialized default. These are operand-field
  encodings, not opcodes, so they never raise the illegal-instruction trap, which is
  scoped to unknown opcodes and unallocated condition encodings only.

## Captured state

On a synchronous trap the machine captures, at a location the handler reads at known
offsets, three items:

1. **Faulting PC**, per the fault-versus-trap class above (faulting instruction for a
   fault, following instruction for a trap).
2. **Cause word**: the vector index, plus the subcode where a cause multiplexes distinct
   conditions (divide-by-zero versus quotient overflow; unknown-opcode versus
   unallocated-condition).
3. **Aux info**: the faulting address for memory-class faults; the offending instruction
   byte for illegal-instruction; zero otherwise.

### Saved-state stack layout

The capture reuses the shipped IRET return path unchanged rather than inventing a
separate trap-return instruction. IRET pops RF first and then the PC. For that to be
correct, the top of the saved frame (the word at SP on handler entry, once the handler
has removed the two extra words) must be RF, with the saved PC just below it.

The trap-entry sequence therefore pushes, in order: **PC, then RF, then cause, then
aux**. Because the stack grows downward (each push pre-decrements SP), the resulting
frame reads, from SP upward toward higher addresses (top to bottom):

    SP + 0   aux      <- SP points here on handler entry
    SP + 8   cause
    SP + 16  RF
    SP + 24  PC       (RF and PC are the IRET frame)

The handler pops the two trap-only words itself (aux, then cause), which leaves SP
pointing at the saved RF. It then returns with **IRET**, which pops RF and then PC,
restoring the interrupted context. This is the shared return path for both traps and
interrupts; there is no TRET.

(The vector-table base address, the per-entry width, and the index-bounds check are the
one part of the format not frozen in this chapter. They are co-authored with maize-21,
which owns the delivery mechanism, before v1.0. This chapter freezes the taxonomy, the
cause / subcode numbering, and the capture layout above; maize-21 inherits them.)

## Vector table and delivery

Traps and interrupts share **one** vector table, **one** saved-state layout, and **one**
return instruction (IRET). This single-table design is the coherence requirement with
the interrupt model (maize-21).

- **Indexing.** The table is indexed by cause number: entry[cause] holds the handler.
  Synchronous traps occupy indices 0..31; external interrupts occupy 32 and above.
- **Handler entry.** On a fired trap the machine looks up entry[cause], captures the
  state described above onto the stack, and loads the handler address into PC. The
  handler address is a full 64-bit value (maize-41). The interrupt-enable state on
  handler entry is governed by the maskability rule below.
- **Handler return.** The handler removes its trap-only words (aux, cause) and executes
  IRET, which pops RF and then PC. This is shared with the interrupt return path.
- **No handler installed (pre-OS / bare metal).** If no handler is installed for a fired
  trap, the machine **halts deterministically** with the cause surfaced. This is the
  defined successor to the reference VM's current throw-and-exit: a divide-by-zero, an
  unknown opcode, or a breakpoint stops the VM with an observable cause rather than
  wandering or invoking host undefined behavior. A third-party VM with no OS loaded must
  behave identically.

## Maskability and the interrupt model

Synchronous traps (the faults and the breakpoint) are **unmaskable**: a masked
divide-by-zero cannot be silently dropped, because that would reintroduce undefined
behavior through the back door. External and device interrupts are **maskable** through
the shipped RF interrupt-enable bit, toggled by the SETINT and CLRINT instructions. The
interrupt-enable bit governs only the maskable external sources; it has no effect on a
synchronous trap.

Nesting, priority, and interrupt acknowledge are maize-21's to define, but they are
built on the saved-state layout this chapter fixes. The reference VM already carries the
seam: RF holds the interrupt-enable and interrupt-set bits, SETINT / CLRINT toggle the
enable bit, IRET pops RF then PC, and `run()` holds a commented delivery skeleton (push
PC, push RF, load the handler address) that maize-21 fills in against this layout.

## Conformance

The trap contract is observable and testable. A conforming VM must pass, at minimum,
these checks for the divide-by-zero and illegal-opcode cases; the same shape generalizes
to every entry in the taxonomy.

**Divide-by-zero (cause 2).** Execute a signed `DIV` with a zero divisor.
- With a handler installed at entry[2]: the handler is entered with cause 2, subcode 0
  (divide-by-zero), and a faulting PC equal to the address of the DIV instruction (a
  fault captures the faulting instruction, so an IRET without correcting the divisor
  would re-execute and trap again).
- With no handler installed: the VM halts deterministically with cause 2 surfaced, and
  no instruction after the DIV takes effect.

**Illegal opcode (cause 0).** Execute an undefined opcode byte.
- With a handler installed at entry[0]: the handler is entered with cause 0, subcode 0
  (unknown opcode), aux equal to the offending byte, and a faulting PC equal to the
  address of that byte.
- With no handler installed: the VM halts deterministically with cause 0 surfaced.

**Breakpoint (cause 3).** Execute `BRK` (`$FF`). Because a breakpoint is a trap, the
captured PC is the address of the instruction **after** `BRK`. With no handler installed
the VM halts deterministically with cause 3 surfaced, and the instruction after `BRK`
does not execute. The reference VM ships a regression test asserting exactly this
(asm/test_brk.mazm plus the run_brk_trap_test runner in scripts/run-tests.sh).

## Implementation grounding and status

Each entry is marked against the reference VM (src/cpu.cpp, src/maize_cpu.h) as already
implemented, newly pinned by this chapter, or a reserved mechanism deferred to a later
card.

- **Illegal instruction (cause 0)**: implemented as host abort. The opcode dispatch
  `default:` and the condition-evaluator `default:` both throw and exit today. Guest
  delivery (vector lookup, capture, handler entry) is newly pinned here and delivered by
  maize-21.
- **Divide error (cause 2)**: implemented as host abort. `raise_divide_error` throws for
  signed and unsigned divide-by-zero and for signed `INT_MIN / -1` quotient overflow
  across all four DIV / MOD / UDIV / UMOD widths (this is the maize-86 lineage: guard the
  condition rather than let the host divide fault). Guest delivery newly pinned here.
- **Breakpoint (cause 3)**: newly pinned and implemented on this card. `BRK` was a no-op
  (maize-10 Decision D6460); this card promotes it to a defined breakpoint trap
  (`raise_breakpoint`), reconciling the `$FF` sentinel intent with dispatch. Unhandled,
  it halts deterministically with the breakpoint cause, through the same throw-and-exit
  mechanism as divide-error, until maize-21 supplies the in-guest handler path.
- **Privileged operation (cause 4)**: reserved number, mechanism deferred. The RF
  privilege bit exists and is set on power-up, but no instruction checks it yet.
- **Segment / bounds (cause 5) and stack fault (cause 6)**: reserved numbers, mechanism
  deferred to maize-92. The flat sparse memory model has no out-of-bounds access and no
  stack bound, so neither fires yet.
- **Interrupt delivery substrate**: present but inert. RF carries the interrupt-enable
  and interrupt-set bits; SETINT / CLRINT toggle the enable bit; IRET pops RF then PC;
  `run()` holds a commented-out delivery skeleton. No vector table is read anywhere yet;
  maize-21 fills that seam against the layout this chapter fixes.
- **INT (`$24` / `$64`)**: has no dispatch case yet and is deliberately deferred until
  the vector-table format exists (co-authored with maize-21).

The defined non-trapping behaviors are all shipped: overflow wrap and flags (maize-1),
out-of-range shift result-0 (maize-1), sparse allocate-on-touch memory (the flat memory
model, maize-75 lineage), byte-wise misaligned access, and the value-initialized default
for undefined sub-register and immediate-size encodings.
