# Chapter 12: Forward Compatibility and Reserved Space

This chapter is a normative part of the Maize ISA specification. It fixes what encoding
space and what contracts the v1.0 freeze holds open so that a paging MMU, atomics and
threads, base-and-bounds segments, and eventually a nommu-Linux (uClinux) port can arrive
as v1.x extensions without breaking a single v1.0 binary.

This chapter adds nothing to v1.0. It defines no new instruction, no new register, and no
new semantic. Every encoding it names as reserved already decodes as `reserved` in the
reference VM and in `mzdis`, and already appears as a `reserved` row in the README opcode
tables. The chapter is a reservation-and-contract schedule over state that is already
latent in the machine, so it introduces no binary-compatibility break and requires no
change to `src/`, `mazm`, or `mzdis`.

Ground truth for every number and name below is the reference VM (`src/maize_cpu.h` for the
opcode map and the register model), cross-checked against the repository README (the
Special-purpose Registers, Flags, Execution, and the two opcode tables) and the Trap Model
chapter.

## The forward-compatibility guarantee

A conforming v1.x Maize implementation runs every v1.0 binary unchanged. This guarantee
rests on two pillars fixed here:

1. **Reset state is the v1.0 world.** Every extension this chapter reserves is disabled at
   reset. Paging is off, no segment limit is armed, and the machine comes up privileged in
   the flat 64-bit model that v1.0 defines. A v1.0 image therefore sees exactly the v1.0
   machine on any conforming v1.x VM.
2. **Extensions arrive only from reserved space.** New instructions come from the reserved
   opcode encodings named below; new architectural state comes through the reserved
   control-register mechanism, never by widening the 4-bit operand register field or by
   repurposing a defined encoding. Because a v1.0 binary uses none of the reserved
   encodings, no extension can collide with it.

Nothing in this chapter is optional for a v1.x implementation that claims backward
compatibility.

## 1. Encoding-space reservation (the opcode map)

The opcode byte is two mode bits plus a six-bit base opcode, giving **64 base slots**. The
current map includes the full floating-point set plus FGETCSR / FSETCSR and JP / SETP; the
residual free-slot count below is pinned by inspecting that map, not from an earlier
estimate.

### Floating-point base-slot accounting

Floating point occupies **twelve** base slots: eleven arithmetic, compare, and conversion
slots plus one FCSR-access slot. The twelve are:

- `$1A` FADD, `$1B` FSUB, `$1C` FMUL, `$21` FDIV (four arithmetic).
- `$22` FSQRT / FNEG / FABS, `$23` FMADD, `$25` FMSUB (fused and unary).
- `$2A` FCMP (compare).
- `$33` FMIN / FMAX.
- `$39` FCVTFF / FCVTFS / FCVTFU, `$3A` FCVTSF / FCVTUF (conversions).
- `$15` FGETCSR / FSETCSR (FCSR access; the one slot outside the eleven-op arithmetic set).

JP (`$D8`) and SETP (`$EC`) do not consume a base slot; they claim one of the two reserved
spare condition encodings that the Jcc / SETcc families left open, and the other spare pair
(`$D9` / `$ED`) stays reserved.

### Residual free base slots at freeze

After the floating-point claim, four base slots were earmarked to a known v1.x claimant
class each, forming a labelled reservation band. Earmarking records intent only; it defines
no encoding. The `$26` and `$28` claimants (the paging MMU foundation) have since landed, so **two fully-free base slots remain** (`$37`, `$38`); the other two still decode
as `reserved` until their owning extension lands.

| Base slot | Reserved for | Status |
|-----------|--------------|--------|
| `$26` | Privileged control-register access mechanism (move-to / move-from control register) | **Landed:** MOVTCR `$26`/`$66`, MOVFCR `$A6` (`$E6` reserved) |
| `$28` | Paging / MMU control (paging-enable, TLB-invalidate) | **Landed:** TLBINV `$28`, TLBINVA `$68` (`$A8`/`$E8` reserved). Paging-enable is a MOVTCR write to CR0.MODE, no dedicated opcode; the Sv48 walk itself is live |
| `$37` | SMP and memory-ordering primitives (fences, acquire-release, LL-SC) | Reserved |
| `$38` | Versioning and capability query hook | Reserved |

Each class fits comfortably in a single base slot: a base slot carries four addressing-mode
forms, or up to four row-packed register-only micro-ops in the condition-row style the
machine already uses (INC / DEC / NOT / NEG at `$31`; SETINT / CLRINT / SETCRY / CLRCRY at
`$29`). For example, the control-register slot needs only a move-to and a move-from form,
and the atomics slot can row-pack a fence with an acquire-release or LL-SC pair.

### Full-byte-dispatch reserve and the escape prefix

Three encodings are held for future operations that dispatch on the whole opcode byte
rather than masking to a base slot: `$3F`, `$7F`, and `$BF`. Their sibling `$FF` is the BRK
breakpoint sentinel and is not available; a run of erased (`$FF`) memory reached as code
must trap as a breakpoint, so a base-`$3F` mask-to-base operation (whose immAddr form would
be `$FF`) can never be defined. These three encodings are the natural home for an escape
prefix.

v1.0 formally reserves an **escape-prefix page**: the concept of a single opcode byte that,
when formalized, opens a second 256-entry opcode plane and multiplies the machine's ultimate
encoding headroom. v1.0 reserves this page but defines nothing about it. It does not name
which byte is the escape prefix, and it does not define the second plane's contents; both
are deliberately left to a future extension. The `$3F` / `$7F` / `$BF` full-byte-dispatch
band is the reserved candidate carrier. Reserving the page now guarantees that even if every
base slot is one day spent, a whole second plane remains available without any v1.0 binary
being affected.

### Micro-op headroom in existing families

Beyond the four free base slots and the escape page, several reserved rows inside
already-defined row-packed families provide headroom for extensions that fit an existing
instruction's shape, at no cost to the free-slot count. These stay `reserved` and are listed
so the inventory is complete: the spare condition encodings `$D9` (Jcc) and `$ED` (SETcc),
reserved for a future integer-overflow JO / JNO and SETO / SETNO; the reserved zero-operand
row `$E7`; the FGETCSR / FSETCSR upper
rows `$95` and `$D5`; and the reserved FP rows `$E2`, `$F9`, `$BA`, `$FA`, `$B3`, `$F3`.

The two spare rows of the `$24` INT slot, `$A4` and `$E4`, were allocated as the v1.x
zero-operand pair SETSYSG / CLRSYSG: they set / clear the RF syscall-guest
bit that selects whether `SYS` dispatches to the native provider (clear, the boot default)
or traps through cause 7 to a guest-installed handler (set). This is a v1.x-compatible
extension (default-clear preserves every v1.0 binary); it spends no fully-free base slot
(the `$26` / `$28` control-register and MMU slots have since landed, and
`$37` / `$38` stay reserved for their v1.x claimants).

The reserved CPZ address-form rows `$93` and `$D3` were spent as LDZ, the zero-extending
load: LDZ reads N bytes (N = the destination subregister width) and
zero-extends into the full register, sharing CPZ's base slot `$13` the way LD shares CP's
`$01`. Unlike SETSYSG it is default-available (no opt-in bit) and is v1.x-compatible
because no v1.0 binary can contain `$93` / `$D3`: both decoded as reserved /
illegal-instruction from the removal of the original LDZ until this spend.

The reserved PUSH-slot row `$A0` and the reserved `$32`-slot row `$B2` were spent as
PUSHALL and POPALL, the zero-operand multi-register save and restore of the fixed
thirteen-register process-context block (see the instruction reference for the block ABI). The spend follows the LDZ pattern exactly: in-family rows, no free base
slot consumed (`$37` and `$38` keep their SMP and versioning earmarks), default-available,
and v1.x-compatible because both rows decoded as reserved until this spend. The `$32`
slot's remaining spare row `$F2` stays reserved.

## 2. Memory-ordering and atomics contract

v1.0 states a memory-ordering model rather than leaving it implementation-defined, so that
a future multi-hart extension cannot retroactively change what a v1.0 binary means.

### v1.0 baseline: single-hart sequential consistency

Maize v1.0 is a single execution context (one hart). All memory operations take effect in
program order, and each instruction is indivisible with respect to traps and interrupts:
trap delivery is precise (see the Trap Model chapter), so a memory operation either
completes fully before a trap is taken or has not begun. Under a single hart every ordinary
load and store is therefore already sequentially consistent, and every read-modify-write
instruction is already atomic with respect to the only observer that exists.

### CMPXCHG is the frozen compare-and-swap primitive

`CMPXCHG` (`$11`) exists and is dispatched in the reference VM. v1.0 fully specifies its
contract, so the atomic path has its compare-and-swap at freeze and no second freeze event
is needed for atomics. The instruction takes three operands in the shared three-operand
shape (the same shape as LEA and MULW): `CMPXCHG new target expected`, where operand 1 is
the new value, operand 2 is the target cell, and operand 3 is the expected (comparand)
value.

The frozen semantics:

- The instruction compares the target (operand 2) against the expected value (operand 3).
  The comparison is a pure equality test over the selected sub-register width and touches no
  flag.
- **On equality (success):** the Zero flag is set to 1, and the new value (operand 1) is
  copied into the target (operand 2). This is the swap.
- **On inequality (failure):** the Zero flag is set to 0, and the current target value
  (operand 2) is copied into the expected register (operand 3), handing the caller the
  observed value for a retry loop.
- **Flag effect:** the Zero flag is the sole and authoritative success indicator (1 =
  swapped, 0 = not swapped). The copy is flag-neutral and the equality test writes no flag,
  so `CMPXCHG` leaves the Carry, Negative, and Overflow flags unchanged and reports its
  outcome only in Zero. A caller tests success with a `JZ` / `JNZ` on the Zero flag.

The addressing-mode forms follow the family: operand 1 may be a register value (`$11`), an
immediate (`$51`), a value at a register address (`$91`), or a value at an immediate address
(`$D1`); the target and expected operands are always registers.

Because v1.0 is single-hart, this contract is trivially atomic. A conforming v1.x multi-hart
implementation must preserve it: `CMPXCHG` must remain an atomic compare-and-swap with
exactly these register and Zero-flag effects, so v1.0 binaries that use it as their atomic
keep working.

### Reserved SMP ordering path

A future multi-hart extension needs explicit ordering primitives that single-hart v1.0 does
not: memory fences, and either acquire-release annotations or a load-linked / store-
conditional pair. v1.0 reserves base slot `$37` and its encoding space for that primitive
set (section 1). The exact primitives are v1.x work; v1.0 only holds the door, so SMP can
arrive from reserved space without invalidating any single-hart v1.0 binary.

## 3. Privilege and trap hooks for syscall entry and return

The machine already carries the hooks a kernel syscall boundary needs. v1.0 pins them as
contract and reserves the surrounding trap-number headroom; it defines no new enforcement.

### Privilege mode

`RF.H1` holds the privilege, interrupt-enabled, interrupt-set, and running flags, which may
only be set in privileged mode (see the README Flags and Execution sections and the Register
Model chapter). The CPU starts privileged at reset. v1.0 pins user versus supervisor mode on
the privilege bit and pins the rule that privileged flags, registers, and instructions are
inaccessible when the bit is clear. The candidate privileged instruction set (IN / OUT,
SETINT / CLRINT, IRET, HALT, and future control-register and segment writes) is settled
jointly with the port-I/O and segment work; the enforcement mechanism itself is reserved,
and its trap is cause 4 in the trap taxonomy.

### Syscall entry and return

`SYS` (`$34`) is the syscall-entry primitive. The interrupt path back to privileged mode is
`INT` / `IRET` (`$24` / `$67`) with `SETINT` / `CLRINT` toggling the interrupt-enable bit.
v1.0 pins the user-to-supervisor transition on `SYS` and the saved-state and return
contract, which is consistent with the trap model's reserved **cause 7 (SYS / syscall
entry)**:

- `SYS` is a deliberate synchronous software trap. It is trap-class, not fault-class, so it
  captures the address of the **following** instruction; `IRET` therefore resumes the
  program at the instruction after `SYS`.
- The saved-state contract reuses the shared trap frame the trap model freezes: the entry
  sequence pushes PC, then RF, then the cause word, then aux, and the handler pops its two
  trap-only words (aux, cause) and returns with the shared `IRET`, which pops RF and then
  PC. There is no separate trap-return instruction.
- The syscall number and arguments travel in registers per the syscall ABI, not on the
  frame. The reference VM today dispatches `SYS` directly to the BIOS and syscall surface;
  routing it through the shared trap table at vector 7 is a future path. This chapter
  reserves the vector and names `SYS` as its source; it defines no syscall numbering.

The save and restore contract above matches the trap model's trap-time state contract
exactly; the two chapters share one frame layout and one return instruction so a syscall and
a trap are indistinguishable to the return path.

### Reserved trap classes

The trap model reserves, in the shared cause / vector taxonomy, the numbers that future
protection extensions need, so they slot in without renumbering any v1.0 trap:

- **Cause 4**, privileged operation in user mode (fault): reserved number, enforcement
  mechanism deferred.
- **Cause 5**, segment / bounds violation (fault): reserved for the segment extension;
  reports the faulting address; never fires in the flat model.
- **Cause 6**, stack fault (fault): reserved for the segment extension; reports the faulting
  address; never fires until a stack bound exists.
- **Cause 7**, SYS / syscall entry (trap): reserved as above.
- A **future page-fault** class joins the same taxonomy from the reserved cause range
  (8 through 31), with faulting-address and error-code reporting, when paging lands (section
  7). Reserving the range now means the page-fault number is assigned without disturbing any
  v1.0 trap.

## 4. Thread-pointer convention

**The operand register field is fully allocated.** The 4-bit operand register field encodes
sixteen registers, and all sixteen are assigned: R0 through R9, RT, RV, RF, RB, RP, RS.
There is **no free operand-register encoding** for a new architectural register. A thread
pointer therefore cannot be a new operand-addressable register. It is necessarily one of two
things, and v1.0 pins the first and reserves the second:

### Pinned: R9 is the thread-pointer register by C-ABI convention

v1.0 designates **R9** as the thread pointer by calling-convention agreement. This costs
zero encoding at freeze and keeps the threads door open, exactly as RISC-V designates `tp`
= `x4` by convention rather than by a dedicated opcode. The choice is grounded in the Maize
C calling convention (`toolchain/qbe-maize/CALLING-CONVENTION.md`):

- R9 is a **callee-saved** general register (R6 through R9 are callee-saved), so a thread
  pointer held in R9 survives across calls, which a thread pointer must.
- R9 **never carries an argument** (arguments use R0 through R5) and is not the return
  register (RV), so pinning it does not perturb argument or return lowering.
- It is not one of the fixed-role special registers (RT scratch, RB frame pointer, RS stack
  pointer, RP program counter, RF flags), all of which already have a job.
- R9 is the **highest** general allocatable register. Removing the single highest register
  from the allocatable pool is the least-disruptive choice for the register allocator, which
  fills the general pool from the low end; the QBE Maize back-end would drop R9 from its
  callee-saved allocatable set and treat it as globally reserved, the same treatment RT
  already receives.

Under this convention a kernel or threading runtime loads the current thread's control block
pointer into R9 on context switch, and thread-local-storage access reads through R9. Because
this is an ABI convention and not an instruction, the reference VM, `mazm`, and `mzdis` need
no change; the convention lives in the C calling convention and binds only code that opts
into threading.

### Reserved: the thread-pointer system-register path

v1.0 also reserves the option of a future dedicated thread-pointer **system register**, set
by the kernel and read through the reserved control-register mechanism (section 5) rather
than through the operand field. If a v1.x extension wants a thread pointer that does not
consume a general register, it allocates one control-register number for it. Reserving this
path now means the machine can move the thread pointer off R9 later without a new opcode.

## 5. Control-register mechanism reservation (the linchpin)

Every new privileged register the protection ladder needs (the segment base and limit
registers, the page-table-base register, the paging-enable bit, and the optional thread-
pointer system register) is architectural state that **cannot live in the full 4-bit operand
field**. v1.0 therefore reserves a single privileged **control-register access mechanism**
and its register-numbering space, defining nothing yet:

- **The access mechanism** is a privileged move-to-control-register and move-from-control-
  register pair, MOVTCR / MOVFCR at base slot `$26` (section 1). **Landed:**
  executing either with the privilege bit clear raises the cause-4 privileged-operation fault.
- **The numbering space** is a flat control-register index. The first
  three are assigned: CR0 `SATP` (address-translation control), CR1 `FAULT_VA`, CR2 `FAULT_ERR`. Indices
  above 2 stay reserved for privileged control registers defined by later v1.x extensions
  (segment base/limit, thread pointer); a write to an unassigned index is discarded and a read
  yields 0, mirroring the unpopulated-port convention.

This mechanism is the linchpin shared by base-and-bounds segments, paging, and the thread-
pointer-as-system-register option: none of them fits the operand field, and all of them
reach their state through this one reserved door. Reserving it now, cheaply and defining
nothing, is what lets each downstream extension add state without a new access path and
without touching v1.0.

## 6. Base-and-bounds segment reservation

The protection ladder's second rung is base-and-bounds segmentation: a privileged base and
limit register pair (with a considered upgrade to two pairs, code and data, for execute
protection), an effective address computed as segment base plus offset and limit-checked,
and a bounds trap on violation. v1.0 reserves the space for this rung regardless of when it
ships:

- The segment base and limit registers are reserved control-register numbers in the section
  5 namespace.
- The **segment / bounds violation** trap is reserved as **cause 5** and the **stack fault**
  trap as **cause 6** in the trap taxonomy, both reporting the faulting address.

Whether the rung-2 registers and traps land inside the v1.0 freeze or arrive as the first
v1.x extension is the segment extension's decision; this chapter does not pre-empt that
timing. It reserves the register-numbering and trap-number space either way, so the segment
extension can define the rung inside or outside v1.0 without any renumbering.

## 7. Paging and MMU headroom, and the versioning hook

So a future Sv48-style paging extension can arrive without touching v1.0 binaries, v1.0
reserved the following. The paging MMU foundation landed the opcode and
control-register pieces; the Sv48 translation itself (the four-level walk, the software TLB
behaviour, the page-fault trap) has since landed on that foundation:

- **Opcode headroom** for a paging-enable control and a TLB-invalidate instruction, drawn
  from base slot `$28` (section 1). **Landed:**
  TLBINV `$28` (flush all) / TLBINVA `$68` (flush one), live under Sv48. Paging-enable needs no
  dedicated opcode; it is a MOVTCR write to CR0.MODE.
- **A privileged control-register namespace** for the page-table-base register and the
  paging-enable bit, allocated from the section 5 control-register numbering space alongside
  the segment base and limit registers. **Landed:** CR0 `SATP` carries both
  the MODE (paging-enable) field and the root page-table PPN.
- **A page-fault trap class**, reserved from the trap taxonomy's future range (causes 8
  through 31), reporting the faulting address and an error code, joining the reserved bounds
  (cause 5) and stack (cause 6) classes. **Landed:** cause 8, with the
  faulting VA in CR1 FAULT_VA and a PRESENT / ACCESS_KIND / USER error code in CR2 FAULT_ERR.
- **A versioning and capability hook.** v1.0 reserves a read-only way for software to detect
  the machine's extension level: an ISA major and minor version plus a supported-extension
  bitmap covering floating point, atomics and SMP, segments, and paging, analogous to
  RISC-V `misa` or x86 CPUID. The carrier is not fixed here (it may be a read-only capability
  control register in the section 5 namespace, a `SYS` query function, or an `IN` port); base
  slot `$38` is held as the opcode carrier if the hook is realized as an instruction rather
  than through the control-register namespace. The versioning policy chapter defines what a
  version number promises and what may change across a v1.x bump; this chapter only mandates
  that the hook be reserved.
- **Paging-off is the reset state and a permanent first-class mode.** The entire v1.0 world
  (flat images, hosted mode) runs unchanged with any future MMU disabled at reset, mirroring
  the real-hardware boot arc. Stating it in the freeze is what makes the forward-
  compatibility guarantee concrete: a v1.0 binary is guaranteed to run on every conforming
  v1.x VM, whatever extensions that VM adds, because they are all off until privileged code
  turns them on.

The paging address model (48-bit canonical virtual addresses, 4 KB pages, four-level tables,
a PTE carrying valid / R / W / X / user-supervisor / accessed / dirty and a physical frame
number) is v1.x design work owned by the paging extension, not v1.0 content. v1.0 reserves
only the opcode, control-register, and trap-number space that extension will consume.

## 8. Cross-references and the no-break statement

- **The ISA specification document** consumes this chapter as its forward-compatibility and
  reserved-space chapter.
- **The versioning and freeze policy chapter** consumes the reservation inventory in sections
  1 through 7 as the definition of what may be added from reserved space in a v1.x bump
  versus what is frozen.
- **The Trap Model chapter** owns the trap taxonomy and the trap-time saved state. The
  syscall save and restore contract (section 3) and the reserved trap classes (causes 4, 5,
  6, 7, and the future page-fault class) are consistent with it; the two chapters share one
  vector table, one frame layout, and one return instruction.
- **The base-and-bounds segment extension** owns whether the rung-2 registers and traps land
  inside v1.0 or as the first v1.x extension (section 6). This chapter reserves the space
  either way.
- **The floating-point set** fixes the residual free base-slot count reconciled in section 1.

**No v1.0 binary-compatibility break, and no code change.** This chapter adds no v1.0
instruction, register, or semantic. Every encoding it names as reserved already decodes as
`reserved` in the VM and in `mzdis` and already appears as `reserved` in the README opcode
tables. The only surfaces it fixes are this freeze document and the README reserved-slot and
privilege notes. No change to `src/`, `mazm`, or `mzdis` is required.
