# Chapter 10: Trap Model

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
traps occupy the low range 0..31; external and device interrupts occupy 32 and above.
Vector 1 is intentionally left unassigned, reserving a slot for a future debug /
single-step trap, matching the x86 lineage a reader will expect.

| Cause | Name | Class | Trigger | Captured aux |
|------:|------|-------|---------|--------------|
| 0 | Illegal instruction / illegal operand | Fault | An unknown / undefined opcode; an unallocated condition encoding on a Jcc / SETcc; or an illegal floating-point encoding (a B\* / Q\* subregister on an FP operand, an FP operand-width mismatch, a reserved / unsupported FCSR rounding mode on a rounding op, or a reserved FP opcode form) | Offending instruction byte |
| 1 | (reserved) | n/a | Unassigned; reserved for a future debug / single-step trap | n/a |
| 2 | Divide error | Fault | Divide-by-zero (signed or unsigned), or signed `INT_MIN / -1` quotient overflow | Subcode: 0 = divide-by-zero, 1 = quotient overflow |
| 3 | Breakpoint | Trap | The `BRK` (`$FF`) instruction executes | 0 |
| 4 | Privileged operation in user mode | Fault | A privileged instruction executes with the RF privilege bit clear | Offending instruction byte |
| 5 | Segment / bounds violation | Fault | An access falls outside a segment window, once segments exist | Faulting address |
| 6 | Stack fault | Fault | A stack access violates the stack limit, once a stack bound exists | Faulting address |
| 7 | SYS / syscall entry | Trap | The `SYS` instruction executes (a deliberate synchronous software trap into the kernel syscall dispatcher) | 0 (syscall number and arguments travel in registers per the syscall ABI) |
| 8 | Page fault | Fault | An Sv48 address translation (CR0 SATP.MODE = 1) finds no valid mapping (a PTE with V=0, or a non-leaf PTE at walk level 0) or a mapping that violates the requested access (X for a fetch, R for a load, W for a store, or the U bit for a user-mode access) | Faulting VA in CR1 FAULT_VA; a packed error code in CR2 FAULT_ERR |
| 9..31 | (reserved) | n/a | Future synchronous traps | n/a |
| 32.. | External / device interrupts | Interrupt | Device / timer sources; the timer is the first source | Source-defined |

Cause subcodes: where one cause number multiplexes distinct conditions, the cause word
carries a subcode so a handler can tell them apart without re-deriving the condition.
Divide error uses subcode 0 for divide-by-zero and 1 for signed quotient overflow.
Illegal instruction uses subcode 0 for an unknown opcode, 1 for an unallocated
condition encoding, and 2 for an illegal floating-point encoding / operand.

### Per-entry detail

**Cause 0, Illegal instruction / illegal operand (fault).** Fires when the decoder meets
an opcode byte with no defined meaning, when a Jcc / SETcc carries a condition selector
that is not an allocated encoding, or when a floating-point instruction carries an
illegal encoding or operand. The floating-point cases are: a B\* or Q\* subregister on an
FP operand (FP operands must select H0 / H1 for binary32 or W0 for binary64), an FP
operand-width mismatch (mixing binary32 and binary64 on a same-format op), a reserved or
unsupported FCSR rounding mode on a rounding op, or a reserved FP opcode form. All of
these are illegal-encoding / illegal-operand conditions, distinct from FP arithmetic
exceptions, which never trap (see "Defined non-trapping behavior"). The undefined
sub-register selector `$F` on any operand is also an illegal-operand case here (it is the
one operand field that traps; the undefined immediate-size field, by contrast, has a defined
default). The trap captures the faulting instruction's PC (a handler may rewrite the
encoding and retry) and the offending instruction byte as aux; the cause subcode is 0 for an
unknown opcode, 1 for an unallocated condition encoding, and 2 for an illegal FP or operand
encoding (the FP encodings and the undefined `$F` selector).

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

**Cause 4, Privileged operation in user mode (fault).** It fires when a privileged
instruction executes with the RF privilege bit clear. Enforcement is **live**: card
maize-21 gated IN / OUT / OUTR, and card maize-180 extended the head-of-dispatch privilege
gate to the control-register and TLB instructions (MOVTCR / MOVFCR, TLBINV / TLBINVA) and to
the previously-ungated HALT, SETINT / CLRINT and SETSYSG / CLRSYSG, and made IRET privileged
(closing the forged-RF escalation: user code cannot forge a privileged RF word and IRET into
supervisor mode). The privilege boundary also covers the direct-write vector: RF is an
operand-addressable destination register, so a user-mode guest write that names RF as its
destination (CP / LD / POP / CPZ / CLR, or an ALU write-back) has its privileged RF.H1 bits
masked, they retain their current values while only the non-privileged condition flags take
the written value (the x86 POPF-in-user model). A user instruction therefore cannot set the
privilege bit by writing RF directly, and this write-masking raises no fault (a benign
user-mode flag write still succeeds). A trap or interrupt entry raises privilege to
supervisor for the handler, so the handler's IRET runs privileged and drops back to user by
restoring a saved RF whose privilege bit is clear (a supervisor RF write is unmasked). INT is
privileged but has no active dispatch in v1.0, so its fault applies once its dispatch lands;
future segment-register writes join the set as they land.

**Cause 5, Segment / bounds violation (fault, reserved).** The cause number is frozen;
the mechanism ships with the segment / base-bounds registers as a future extension. In
the v1.0 flat sparse memory model no access is out of bounds, so this never fires yet.

**Cause 6, Stack fault (fault, reserved).** The cause number is frozen; there is no stack
bound in the flat sparse model, so it never fires until bounds exist.

**Cause 7, SYS / syscall entry (trap, reserved).** The cause number is frozen; the
trap-vector delivery mechanism and the syscall ABI are specified separately. `SYS` is a
deliberate synchronous software trap: unlike the fault causes, it is requested by the
program, so it vectors through the shared trap table at index 7 into the kernel syscall
dispatcher, uses the corrected capture layout below, and returns via the shared IRET. Its
model is that SYS ($34) is syscall entry with a saved-state contract and IRET return.
Like every synchronous trap it is unmaskable. It is trap-class: it captures the address
of the **following** instruction, so IRET resumes the program at the instruction after
`SYS`. The syscall number and arguments travel in registers per the syscall ABI; this
chapter only reserves the vector and names `SYS` as its source. In the reference VM `SYS`
today dispatches directly to the BIOS / syscall surface (`src/sys.cpp`) rather than
through the trap table; routing it through the trap table is a future path.

**Cause 8, Page fault (fault, live under Sv48).** Raised by the Sv48 address-translation
layer (card maize-194) when CR0 SATP.MODE = 1 (Sv48) and a guest memory access cannot be
translated. Two failure classes: no valid mapping (a PTE with V=0, or a non-leaf PTE
reached at walk level 0) and a permission violation (a leaf that lacks the bit the access
requires: X for a fetch, R for a load, W for a store; or a U-clear leaf accessed from user
mode, since supervisor bypasses the U check). The A and D bits are software-managed and
never cause a fault. Unlike the reserved causes above, page fault is delivered through the
real trap table so a kernel handler can run: `raise_page_fault` latches the faulting VA
into CR1 FAULT_VA and a packed error code into CR2 FAULT_ERR, then vectors through
entry[8]. It is FAULT-class (captures the faulting instruction's own PC, so a handler that
repairs the mapping can IRET and re-execute it), not trap-class like SYS. With no handler
installed (entry[8] = 0) it is a deterministic halt with the cause surfaced. A page fault
raised from inside the trap-frame push itself (an unmapped or read-only kernel stack) is a
double fault: it halts deterministically rather than recursing.

CR2 FAULT_ERR bit layout: bit 0 PRESENT (0 = no valid mapping, 1 = a mapping was found but
violates the requested permission); bits 2:1 ACCESS_KIND (0 = fetch, 1 = load, 2 = store);
bit 3 USER (1 if the faulting access ran in user mode); bits 63:4 reserved, written 0.
Bare mode (MODE = 0, the reset default) never page-faults: translation is the identity and
every access passes through unchanged.

## Defined non-trapping behavior

The following conditions look like undefined behavior on a conventional machine but are
first-class **defined outcomes** on Maize. None of them traps, and none of them is host
undefined behavior. They are listed here so no reader mistakes a defined result for a
gap in the taxonomy.

- **Integer overflow wraps.** ADD, SUB, MUL, INC, DEC, NEG, and shift results wrap
  two's-complement and set the C and V flags per the flags model (see the Register Model
  chapter). There is no trap-on-overflow mode in v1.0. The JO / JNO and SETO / SETNO
  condition encodings stay reserved; overflow is observed through flags, never through a
  trap.
- **Out-of-range shift count is defined.** For a shift of width `bits`: `n == 0` leaves
  the flags unaffected; `1 <= n <= bits` shifts normally; `n > bits` yields a result of
  0 with C, V, and N cleared and Z set (Z = 1, since the result is zero). Never a trap,
  never host undefined behavior.
- **Unmapped / sparse memory access is defined.** Memory is sparse: a read of
  never-written memory returns 0, and a write allocates a zero-filled block on first
  touch. There is no EFAULT and no page fault in the v1.0 flat model. The absence of a
  memory-access trap here is deliberate, not a missing case; segment / bounds
  enforcement (cause 5) is the separate, reserved path a future extension adds.
- **Misaligned multi-byte access is defined-allow.** A multi-byte load or store may sit
  at any address; it is stitched byte-wise across the 256-byte allocation blocks with no
  alignment requirement and no trap. No vector is spent on alignment.
- **An undefined immediate-size field decodes to a defined default.** An undefined
  immediate-size encoding (4..7) decodes to the value-initialized default and does not
  trap; it is an operand field, not an opcode. (The undefined sub-register selector `$F`,
  by contrast, is an illegal-operand trap, cause 0, enumerated above; it is the one operand
  field that does trap.)
- **Floating-point arithmetic exceptions are sticky, never trapping.** An FP invalid
  operation, divide-by-zero, overflow, underflow, or inexact result does not trap: the
  operation produces its IEEE-754 defined result (a quiet NaN, a signed infinity, the
  correctly rounded value) and sets the corresponding sticky FFLAGS bit in the FCSR.
  Software clears those flags explicitly; the machine never raises a trap on an FP
  arithmetic exception. Only illegal FP *encodings / operands* trap, and those are cause
  0 (illegal instruction / illegal operand), not an arithmetic exception. See the
  Floating-Point chapter.

## Captured state

On a synchronous trap the machine captures, at a location the handler reads at known
offsets, three items:

1. **Faulting PC**, per the fault-versus-trap class above (faulting instruction for a
   fault, following instruction for a trap).
2. **Cause word**: the vector index, plus the subcode where a cause multiplexes distinct
   conditions (divide-by-zero versus quotient overflow; unknown-opcode versus
   unallocated-condition versus illegal-FP-encoding). The two pack into the 64-bit cause
   word as: the cause number in the low byte (bits 7:0), the subcode in the next byte
   (bits 15:8), and the remaining bits (63:16) reserved and written zero. A handler reads
   the cause with `AND $FF` and the subcode with a shift-and-mask; the reserved-zero high
   bits leave room for the vector-table format to widen the field without breaking
   existing handlers. This packing is frozen here.
3. **Aux info**: the faulting address for memory-class faults; the offending instruction
   byte for illegal-instruction; zero otherwise.

### Saved-state stack layout

The capture reuses the shared IRET return path unchanged rather than inventing a
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

The vector-table format is fixed. The table has a fixed low base address of
`0x1000`, one 4 KiB page above the null/zero location, so the "a null pointer reads 0"
convention stays clean and an uninstalled entry reads as an unambiguous zero. Each entry
is 8 bytes wide and holds a full 64-bit handler address; the table has 256 entries
(2 KiB), indexed 0..255 by cause number (synchronous traps 0..31, external interrupts
32..255). The index is a cause byte, so it is always within the table. An out-of-range
vector or a zero (uninstalled) entry is a deterministic halt with the cause surfaced,
never an out-of-bounds read or a stray dereference. A relocatable base, held in the
reserved privileged control register, is a deferred extension; v1.0 fixes the base. This
chapter freezes the taxonomy, the cause / subcode numbering, the capture layout above,
and this table format.

## Vector table and delivery

Traps and interrupts share **one** vector table, **one** saved-state layout, and **one**
return instruction (IRET). This single-table design is the coherence requirement with
the interrupt model.

- **Indexing.** The table is indexed by cause number: entry[cause] holds the handler.
  It lives at the fixed base `0x1000` with 8-byte entries, so entry[cause] is at
  `0x1000 + cause * 8`. Synchronous traps occupy indices 0..31; external interrupts
  occupy 32 and above, through 255 (256 entries, 2 KiB).
- **Handler entry.** On a fired trap the machine looks up entry[cause], captures the
  state described above onto the stack, and loads the handler address into PC. The
  handler address is a full 64-bit value. The interrupt-enable state on handler entry is
  governed by the maskability rule below.
- **Handler return.** The handler removes its trap-only words (aux, cause) and executes
  IRET, which pops RF and then PC. This is shared with the interrupt return path.
- **No handler installed (pre-OS / bare metal).** If no handler is installed for a fired
  trap, the machine **halts deterministically** with the cause surfaced. A divide-by-zero,
  an unknown opcode, or a breakpoint stops the VM with an observable cause rather than
  wandering or invoking host undefined behavior. A third-party VM with no OS loaded must
  behave identically.

## Maskability and the interrupt model

Synchronous traps (the faults and the breakpoint) are **unmaskable**: a masked
divide-by-zero cannot be silently dropped, because that would reintroduce undefined
behavior through the back door. External and device interrupts are **maskable** through
the RF interrupt-enable bit, toggled by the SETINT and CLRINT instructions. The
interrupt-enable bit governs only the maskable external sources; it has no effect on a
synchronous trap.

Nesting, priority, and interrupt acknowledge are the interrupt model's to define, but
they are built on the saved-state layout this chapter fixes. The reference VM already
carries the seam: RF holds the interrupt-enable and interrupt-set bits, SETINT / CLRINT
toggle the enable bit, IRET pops RF then PC, and `run()` holds a commented delivery
skeleton (push PC, push RF, load the handler address) filled against this layout.

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
(`asm/test_brk.mazm` plus the breakpoint-trap runner in `scripts/run-tests.sh`).

## Reference implementation

The reference VM (`src/cpu.cpp`, `src/maize_cpu.h`) grounds this chapter:

- **Illegal instruction / illegal operand (cause 0)** is the `default:` case of the
  opcode dispatch, the `default:` case of the condition evaluator, and the
  `raise_illegal_fp` call sites (a B\* / Q\* subregister on an FP operand, an FP
  operand-width mismatch, a reserved / unsupported FCSR rounding mode, and a reserved FP
  opcode form).
- **Divide error (cause 2)** is `raise_divide_error`, which guards signed and unsigned
  divide-by-zero and the signed `INT_MIN / -1` quotient overflow across all four DIV /
  MOD / UDIV / UMOD widths rather than letting the host divide fault.
- **Breakpoint (cause 3)** is `raise_breakpoint`: `BRK` (`$FF`) is a defined breakpoint
  trap, not a no-op.
- Where no in-guest handler is installed, these synchronous-trap paths halt the VM
  deterministically with the cause surfaced. In-guest vector-table delivery (vector
  lookup, four-word capture, handler entry) is realized for external interrupts, which
  vector through the table at 32..255; the synchronous faults keep the throw-and-exit
  no-handler behavior until an OS handler-install path exists.
- **Privileged operation (cause 4)**: the RF privilege bit is set on power-up. IN / OUT /
  OUTR enforce it (card maize-21), and card maize-180 extends the gate to MOVTCR / MOVFCR,
  TLBINV / TLBINVA, HALT, SETINT / CLRINT, SETSYSG / CLRSYSG, and IRET: executed with the bit
  clear they raise cause 4. A trap or interrupt entry raises privilege to supervisor for the
  handler; user mode is reached only by an IRET (now privileged, so executable only from
  supervisor) that restores an RF word with the privilege bit clear.
- **Segment / bounds (cause 5) and stack fault (cause 6)**: reserved numbers; the flat
  sparse memory model has no out-of-bounds access and no stack bound, so neither fires.
- **SYS / syscall entry (cause 7)**: reserved number; `SYS` ($34) dispatches directly to
  the BIOS / syscall surface today, with trap-vector delivery reserved.
- **Page fault (cause 8)**: live under Sv48 (card maize-194). When CR0 SATP.MODE = 1 the
  memory-access path translates every guest access through a 64-entry software TLB backed
  by a 4-level Sv48 walk; a not-present or permission failure calls `raise_page_fault`,
  which latches CR1 FAULT_VA and CR2 FAULT_ERR and vectors through entry[8] so a kernel
  handler can run (FAULT-class: the saved PC is the faulting instruction's own). CR0 writes
  and TLBINV flush the whole TLB; TLBINVA flushes one entry. Bare mode (MODE = 0, reset
  default) is the identity passthrough and never faults.
- **Interrupt delivery substrate**: live for external interrupts. RF carries the
  interrupt-enable and interrupt-set bits; SETINT / CLRINT toggle the enable bit; IRET
  pops RF then PC. At each instruction boundary the run loop delivers a pending, enabled
  IRQ: it acknowledges (clears the pending latch), pushes the four-word aux / cause / RF /
  PC frame, masks interrupts, and loads the handler from entry[vector]. The timer is the
  first interrupt source. **INT (`$24` / `$64`)** has no dispatch case yet and is deferred
  as a guest-requested software-trap path.

The defined non-trapping behaviors are all shipped: overflow wrap and flags, out-of-range
shift result-0, sparse allocate-on-touch memory, byte-wise misaligned access, the
value-initialized default for undefined sub-register and immediate-size encodings, and
sticky (never-trapping) FP arithmetic exceptions via the FCSR FFLAGS.
