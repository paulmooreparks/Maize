# Instruction Reference: Control, System, and Devices

This chapter is normative. It gives the full per-instruction reference for the families that
move the program counter, enter and leave the kernel, reach the control and status register
space, maintain the translation cache, and touch the port space. The instruction inventory
summarizes these instructions in one line each; this chapter states their complete behavior,
including every trap each one can raise and the outcome of every operand value.

Each entry gives the assembly syntax, the length class with its opcode, the operation in
prose, the traps, and a worked example. Rules that hold for a whole family are stated once in
that family's preamble and are not repeated per entry.

Four rules from other chapters govern every instruction here and are not restated in the
entries. Register r0 reads as zero and discards writes, so naming r0 as a destination still
performs every other effect the instruction has. Every operand byte of every instruction in
this chapter is a plain slot, so an operand byte whose form field is not `%000` raises the
illegal-operand trap; that check happens during decode, before execution, and therefore
before any privilege check the instruction performs. Immediates follow every operand byte in
the encoded instruction regardless of where the assembly syntax writes them, which matters
for the control and status register instructions and for the immediate form of `sys`. Where
an entry names no trap, the instruction raises none for any operand encoding and any operand
value.

## Branches and control transfer

Conditional control flow is a fused compare-and-branch, and the ten branch predicates are
exactly the ten compare predicates of the compare family, evaluated on the same two register
operands in the same order. No condition register exists, nothing is carried between the
comparison and the transfer, and no instruction in this family writes a general register
except `call`, which writes r31.

A displacement in this family is a signed 32-bit count of bytes measured from the address of
the instruction following the transfer. The machine computes the target by adding the
sign-extended displacement to that address modulo 2^64. Targets are byte-granular, so a
target carries no alignment requirement and any byte address is a legal target. The assembler
accepts either a label, which it resolves to the displacement that reaches the labelled
address, or a numeric literal with its mandatory base marker, which is the displacement
itself. The register forms of `jump` and `call` take an absolute 64-bit address from the whole
register rather than a displacement.

Transferring to an address that is unmapped, or to a page without execute permission, raises
a page fault on the fetch of the target instruction rather than on the transfer. The transfer
itself has completed by then, so `call` has already written r31, and the trap frame reports
the target address as the faulting address. The transfer instructions themselves raise no
fault of their own.

The register form of `call` reads its target register before it writes r31. Writing the link
register can therefore never disturb the target, and `call r31` is a well-defined call through
the current link register.

No instruction in this family is privileged, and every one of them executes identically at
user level and at supervisor level.

### branch_eq

**Syntax:**

    branch_eq rs1 rs2 target

**Encoding:** Length class `op r r i4`, seven bytes, opcode `$60` in the opcode-map appendix.

**Operation:** The machine compares the full 64-bit values of rs1 and rs2. When they are
equal it transfers control to the target computed from the displacement, and otherwise it
continues at the following instruction. Neither source register is modified and no other
register is written.

**Traps:** None.

**Example:**

    branch_eq r4 r0 empty_case

Control reaches `empty_case` when r4 holds zero, since r0 supplies the zero operand.

### branch_ne

**Syntax:**

    branch_ne rs1 rs2 target

**Encoding:** Length class `op r r i4`, seven bytes, opcode `$61` in the opcode-map appendix.

**Operation:** The machine transfers control to the target when the 64-bit values of rs1 and
rs2 differ, and otherwise continues at the following instruction. No register is written.

**Traps:** None.

**Example:**

    branch_ne r7 r8 mismatch

Control reaches `mismatch` when the two words differ in any bit.

### branch_lt_signed

**Syntax:**

    branch_lt_signed rs1 rs2 target

**Encoding:** Length class `op r r i4`, seven bytes, opcode `$62` in the opcode-map appendix.

**Operation:** The machine treats both registers as signed 64-bit two's-complement values and
transfers control to the target when rs1 is less than rs2. Otherwise it continues at the
following instruction. No register is written.

**Traps:** None.

**Example:**

    branch_lt_signed r3 r4 needs_growth

Control reaches `needs_growth` when the signed value in r3 is below the signed value in r4.

### branch_le_signed

**Syntax:**

    branch_le_signed rs1 rs2 target

**Encoding:** Length class `op r r i4`, seven bytes, opcode `$63` in the opcode-map appendix.

**Operation:** The machine treats both registers as signed 64-bit values and transfers
control to the target when rs1 is less than or equal to rs2, and otherwise continues at the
following instruction. No register is written.

**Traps:** None.

**Example:**

    branch_le_signed r5 r6 in_range

Control reaches `in_range` when the signed value in r5 does not exceed the one in r6.

### branch_gt_signed

**Syntax:**

    branch_gt_signed rs1 rs2 target

**Encoding:** Length class `op r r i4`, seven bytes, opcode `$64` in the opcode-map appendix.

**Operation:** The machine treats both registers as signed 64-bit values and transfers
control to the target when rs1 is greater than rs2, and otherwise continues at the following
instruction. No register is written.

**Traps:** None.

**Example:**

    branch_gt_signed r2 r0 positive

Control reaches `positive` when r2 holds a signed value above zero.

### branch_ge_signed

**Syntax:**

    branch_ge_signed rs1 rs2 target

**Encoding:** Length class `op r r i4`, seven bytes, opcode `$65` in the opcode-map appendix.

**Operation:** The machine treats both registers as signed 64-bit values and transfers
control to the target when rs1 is greater than or equal to rs2, and otherwise continues at
the following instruction. No register is written.

**Traps:** None.

**Example:**

    branch_ge_signed r2 r0 non_negative

Control reaches `non_negative` when the sign bit of r2 is clear.

### branch_lt_unsigned

**Syntax:**

    branch_lt_unsigned rs1 rs2 target

**Encoding:** Length class `op r r i4`, seven bytes, opcode `$66` in the opcode-map appendix.

**Operation:** The machine treats both registers as unsigned 64-bit values and transfers
control to the target when rs1 is less than rs2, and otherwise continues at the following
instruction. No register is written.

**Traps:** None.

**Example:**

    branch_lt_unsigned r9 r10 within_bounds

Control reaches `within_bounds` when the unsigned index in r9 is below the limit in r10.

### branch_le_unsigned

**Syntax:**

    branch_le_unsigned rs1 rs2 target

**Encoding:** Length class `op r r i4`, seven bytes, opcode `$67` in the opcode-map appendix.

**Operation:** The machine treats both registers as unsigned 64-bit values and transfers
control to the target when rs1 is less than or equal to rs2, and otherwise continues at the
following instruction. No register is written.

**Traps:** None.

**Example:**

    branch_le_unsigned r9 r10 fits

Control reaches `fits` when the unsigned value in r9 does not exceed the one in r10.

### branch_gt_unsigned

**Syntax:**

    branch_gt_unsigned rs1 rs2 target

**Encoding:** Length class `op r r i4`, seven bytes, opcode `$68` in the opcode-map appendix.

**Operation:** The machine treats both registers as unsigned 64-bit values and transfers
control to the target when rs1 is greater than rs2, and otherwise continues at the following
instruction. No register is written.

**Traps:** None.

**Example:**

    branch_gt_unsigned r11 r12 overflowed

Control reaches `overflowed` when the unsigned count in r11 exceeds the capacity in r12.

### branch_ge_unsigned

**Syntax:**

    branch_ge_unsigned rs1 rs2 target

**Encoding:** Length class `op r r i4`, seven bytes, opcode `$69` in the opcode-map appendix.

**Operation:** The machine treats both registers as unsigned 64-bit values and transfers
control to the target when rs1 is greater than or equal to rs2, and otherwise continues at
the following instruction. No register is written.

**Traps:** None.

**Example:**

    branch_ge_unsigned r9 r10 out_of_bounds

Control reaches `out_of_bounds` when the unsigned index in r9 has reached the limit in r10.

### jump

**Syntax:**

    jump target
    jump rs

**Encoding:** The displacement form is length class `op i4`, five bytes, opcode `$70` in the
opcode-map appendix. The register form is length class `op r`, two bytes, opcode `$71`.

**Operation:** The displacement form transfers control unconditionally to the address
obtained by adding the sign-extended 32-bit displacement to the address of the following
instruction modulo 2^64. The register form transfers control unconditionally to the absolute
address held in the whole 64-bit register rs. Neither form writes any register, and in
particular neither form disturbs r31.

**Traps:** None.

**Example:**

    jump loop_top

The assembler resolves `loop_top` to the displacement that reaches it, and control continues
there.

### call

**Syntax:**

    call target
    call rs

**Encoding:** The displacement form is length class `op i4`, five bytes, opcode `$72` in the
opcode-map appendix. The register form is length class `op r`, two bytes, opcode `$73`.

**Operation:** The machine determines the target first and writes the link second. The
displacement form computes the target by adding the sign-extended 32-bit displacement to the
address of the following instruction modulo 2^64. The register form reads the absolute
target from the whole 64-bit value of rs before it writes anything. The machine then writes
the address of the instruction following the call into r31 and transfers control to the
target. Naming r31 as the target register is therefore well defined, because the old link
value is the target and the new link value replaces it only after the target is known. No
memory is touched and no other register is written.

**Traps:** None.

**Example:**

    call checksum

Control reaches `checksum` and r31 holds the address of the instruction after the call.

### return

**Syntax:**

    return

**Encoding:** Length class `op`, one byte, opcode `$74` in the opcode-map appendix.

**Operation:** The machine transfers control to the absolute address held in the whole 64-bit
value of r31. No register is written and no memory is touched, because the return address
lives in the link register rather than on a stack.

**Traps:** None.

**Example:**

    return

Control resumes at the address the matching `call` wrote into r31.

## System and traps

The system family enters the kernel, leaves it, and controls whether the machine keeps
running. It is small because the trap model pushes a four-word frame in hardware and leaves
register saving to the kernel, and because the mode toggles that v1 spent opcodes on are
control and status register bits in v2. The trap-model chapter owns the frame layout, the
cause numbering, and the vector table; this chapter states what each instruction does to
that machinery.

Privilege in this family divides cleanly. The instructions `sys`, `nop`, and `breakpoint`
execute at any privilege level, because the first is the deliberate user-to-supervisor entry
and the other two are inert with respect to privileged state. The instructions `trap_return`,
`halt`, and `wait_for_interrupt` are privileged, and executing any of them while the machine
is at user level raises the privileged-operation trap with no other effect.

Trap class matters for two of these instructions. A fault captures the address of the
faulting instruction so that a handler can correct the condition and re-execute it, while a
trap in the narrow sense captures the address of the following instruction because there is
nothing to retry. Both `sys` and `breakpoint` are trap-class, so both capture the address of
the instruction after themselves and a `trap_return` resumes past them.

### sys

**Syntax:**

    sys #imm
    sys rs

**Encoding:** The immediate form is length class `op i1`, two bytes, opcode `$BA` in the
opcode-map appendix. The register form is length class `op r`, two bytes, opcode `$BB`.

**Operation:** The machine enters the kernel through the syscall trap. The immediate form
takes the syscall number from its unsigned 8-bit literal, and the register form takes it from
the low byte of rs, ignoring the upper 56 bits of that register. The machine raises the
syscall trap with the syscall number in the trap frame's aux word and the address of the
following instruction in the frame's pc word, then vectors to the syscall handler at
supervisor level. No general register is saved by the machine and no general register is
modified, so the calling convention's argument registers carry syscall arguments into the
kernel and the result registers carry the result back out with no copying by the machine.
Executing `sys` at supervisor level is legal and delivers the same trap.

**Traps:** The syscall trap, unconditionally, which is the instruction's purpose rather than
an error condition.

**Example:**

    sys #12

The machine enters the kernel with syscall number 12 in the frame's aux word, and the
arguments already in the calling convention's argument registers.

### trap_return

**Syntax:**

    trap_return

**Encoding:** Length class `op`, one byte, opcode `$BC` in the opcode-map appendix.

**Operation:** The machine pops the four-word trap frame from the trap stack and resumes
the interrupted context. The pc word becomes the new program counter and the status word
becomes the new status, which restores the privilege level the interrupted context ran at.
The cause and aux words are discarded, since they describe an event that is now handled. No
general register is written, because the machine saved none on entry and register restoration
is the kernel's calling-convention-aware business. When the restored status names user level,
execution continues at user level from the restored program counter.

**Traps:** The privileged-operation trap when the machine is at user level, in which case no
word is popped and the trap-stack register is unchanged. The illegal-operand trap with
subcode 6 when the frame's status word names a reserved privilege encoding or sets a reserved
bit, in which case nothing at all changes, including the trap-stack register. A page fault
when a frame word is not accessible, in which case the pop is abandoned, the trap-stack
register is unchanged, and the instruction re-executes cleanly after the fault is serviced.

**Example:**

    trap_return

The handler resumes the context whose frame sits on the trap stack.

### halt

**Syntax:**

    halt

**Encoding:** Length class `op`, one byte, opcode `$BD` in the opcode-map appendix.

**Operation:** The machine stops. It fetches no further instruction, takes no interrupt, and
performs no further architectural state change, and the halted state is terminal for the
duration of the run. Register and memory state at the moment of the halt is the final state
an external observer sees, which is what lets a conformance binary end with `halt` and have
its results inspected.

**Traps:** The privileged-operation trap when the machine is at user level, in which case the
machine does not stop.

**Example:**

    halt

The machine stops with its registers and memory frozen at this point.

### wait_for_interrupt

**Syntax:**

    wait_for_interrupt

**Encoding:** Length class `op`, one byte, opcode `$BE` in the opcode-map appendix.

**Operation:** The machine suspends execution until some interrupt cause has both its pending
bit and its enable bit set, and then continues at the following instruction. A cause whose
pending bit is set while its enable bit is clear does not wake the machine. Whether the
interrupt is then taken at that boundary follows the ordinary delivery rules, including the
global interrupt-disable bit in the status register, so a kernel idling with interrupts
masked wakes and polls the pending registers while a kernel idling with interrupts enabled
enters the handler before the following instruction executes. A cause that is already pending
and enabled when the instruction executes satisfies it immediately, so no interrupt is lost
by racing to sleep. No register is written and no memory is touched. The instruction is a
hint about idleness and never about correctness, so a machine that returns from it
immediately and repeatedly is conforming.

**Traps:** The privileged-operation trap when the machine is at user level, in which case the
machine does not suspend.

**Example:**

    wait_for_interrupt

The idle loop stops burning cycles until a device or the timer has something to report.

### nop

**Syntax:**

    nop

**Encoding:** Length class `op`, one byte, opcode `$BF` in the opcode-map appendix.

**Operation:** The machine advances the program counter past the instruction and changes
nothing else. No register is written, no memory is touched, and no trap is raised at any
privilege level. This is a real assigned opcode rather than a reserved byte that happens to
do nothing, which is what lets a linker, a patcher, or an alignment pass fill space safely.

**Traps:** None.

**Example:**

    nop

The byte occupies space and has no effect.

### breakpoint

**Syntax:**

    breakpoint

**Encoding:** Length class `op`, one byte, opcode `$FF` in the opcode-map appendix.

**Operation:** The machine raises the breakpoint trap at any privilege level. The trap is
trap-class rather than fault-class, so the frame's pc word holds the address of the
instruction following the breakpoint and a debugger that resumes with `trap_return` lands
after it. No register is written and no memory is touched beyond the frame the trap model
pushes. The opcode is pinned at `$FF` because that is the value that fills erased storage, so
a run of erased memory reached as code stops the machine with a named cause at its first
byte.

**Traps:** The breakpoint trap, unconditionally.

**Example:**

    breakpoint

The debugger takes control, and resuming continues at the next instruction.

## Control and status register access

The control and status registers hold every piece of architectural state that is not a
general register, including the trap-model state, the paging root, the floating-point
rounding mode and sticky flags, the feature bitmap, and the syscall-provider selection bit.
Two instructions reach the whole space. The privileged-architecture chapter owns the
numbering and the meaning of each individual register, and this chapter owns the access
mechanism the two instructions implement.

A register number is an unsigned 16-bit value carried as an immediate, and it carries its own
access rules in its high bits so that the machine enforces them arithmetically without a
lookup table. The privileged-architecture chapter fixes the fields of that number, the four
access rules that follow from them, the order in which the machine applies those rules, and
the side effects a write may carry, and this chapter restates none of it. What each entry
below adds is what the instruction does once the access rules permit the access, and which of
those rules it can trip.

The assembly syntax writes the register number first for `csr_read` and last for `csr_write`,
following the source-to-destination rule, while the encoding places the operand byte first
and the 2-byte immediate after it in both cases, following the fixed component order of the
encoding chapter.

### csr_read

**Syntax:**

    csr_read $csr rd

**Encoding:** Length class `op r i2`, four bytes, opcode `$B8` in the opcode-map appendix. The
operand byte names rd and the 2-byte immediate carries the unsigned register number,
little-endian.

**Operation:** The machine applies the four access rules the privileged-architecture
chapter fixes and, when they permit the
access, writes the current full 64-bit value of the named control and status register into
rd. A register narrower than a word reads with its defined bits in place and every undefined
bit zero. The named register is not modified, and no other state changes. When rd is r0 the
access rules still apply in full and the value is discarded.

**Traps:** The illegal-operand trap on a reserved privilege encoding and on a number the
machine does not implement. The privileged-operation trap on an access below the required
privilege level. In every trapping case rd is unmodified.

**Example:**

    csr_read $4001 r5

The supervisor-level register at index 1 is read into r5, and the number's high bits mark it
as requiring supervisor privilege.

### csr_write

**Syntax:**

    csr_write rs $csr

**Encoding:** Length class `op r i2`, four bytes, opcode `$B9` in the opcode-map appendix. The
operand byte names rs and the 2-byte immediate carries the unsigned register number,
little-endian.

**Operation:** The machine applies the four access rules the privileged-architecture
chapter fixes and, when they permit the
access, writes the full 64-bit value of rs into the named control and status register. Bits
the target register does not define at all are ignored rather than stored. A bit the register
reserves is not one of those, and a write that sets a reserved bit raises the illegal-operand
trap with subcode 6, as does a reserved encoding written into a field the register defines;
the privileged-architecture chapter states both per register. Any side effect the target
register carries takes place as part of this instruction, so the instruction following the write
observes the new state. The source register is not modified.

**Traps:** The illegal-operand trap on a reserved privilege encoding, on a number whose
read-only bit is set, and on a number the machine does not implement. The illegal-operand
trap with subcode 6 on a value the target register does not accept, which the
privileged-architecture chapter states per register and which covers a value that sets a
reserved bit as well as a reserved encoding in a defined field. The privileged-operation trap
on an access below the required privilege level. In every trapping case no control and status
register changes value and no side effect occurs.

**Example:**

    csr_write r6 $4002

The word in r6 lands in the supervisor-level register at index 2, together with whatever side
effect that register defines.

## TLB maintenance

Two instructions maintain the translation cache, and both are privileged, so executing either
at user level raises the privileged-operation trap and changes nothing. They exist for the
case where a kernel edits a live page table without changing the paging root, because a write
to the paging-root control and status register already flushes the whole cache implicitly.

Both instructions are architecturally observable only through which translations survive
them. A machine that keeps no translation cache at all satisfies both by doing nothing, and a
conformance binary can only test that a translation changed under it becomes visible, never
that a particular entry was evicted. Neither instruction reads or writes memory, neither
walks a page table, and neither raises a fault for an address that has no cached translation
or no valid mapping.

### tlb_invalidate_all

**Syntax:**

    tlb_invalidate_all

**Encoding:** Length class `op`, one byte, opcode `$C0` in the opcode-map appendix.

**Operation:** The machine discards every cached address translation, so the next access to
any virtual address walks the page table afresh. No register is written, no memory is
touched, and the page tables themselves are unchanged. Execution continues at the following
instruction, whose own fetch already observes the flushed state.

**Traps:** The privileged-operation trap when the machine is at user level, in which case no
translation is discarded.

**Example:**

    tlb_invalidate_all

Every cached translation is dropped after the kernel rewrites a range of page-table entries.

### tlb_invalidate_address

**Syntax:**

    tlb_invalidate_address rs

**Encoding:** Length class `op r`, two bytes, opcode `$C1` in the opcode-map appendix.

**Operation:** The machine discards any cached translation for the page containing the
virtual address held in rs. The low bits of the address within the page are ignored, so any
address in the page names the page. An address with no cached translation, an address with no
valid mapping, and an address outside the translatable range all leave the machine unchanged
rather than faulting, because the instruction manipulates a cache and never consults a page
table. No register is written and no memory is touched. A machine may discard more
translations than the instruction names, up to and including all of them.

**Traps:** The privileged-operation trap when the machine is at user level, in which case no
translation is discarded.

**Example:**

    tlb_invalidate_address r7

The cached translation for the page containing the address in r7 is dropped.

## Port input and output

Devices live in a port space that is disjoint from the memory space, so no device register is
reachable through a load, a store, or a block-memory instruction, and no port access touches
ordinary memory. The device-surface chapter owns which ports exist and what each one means, and this
chapter owns the two instructions that reach them.

Three rules govern the whole family. Both instructions are privileged, so executing either at
user level raises the privileged-operation trap and performs no port access at all, which
keeps device access a kernel responsibility. The port identifier is the low quarter-word of
the port register, giving 65536 ports, and the upper 48 bits of that register are ignored
rather than checked, so no port register value is invalid. A read from an unpopulated port
yields zero and a write to an unpopulated port is discarded, so probing for a device is
defined on every machine regardless of which devices it carries.

No operand of either instruction carries the `@` sigil, because that sigil marks an operand
through which the instruction touches memory and a port operand touches no memory. The port
register holds an identifier rather than an address, and a reader scanning for memory traffic
correctly finds none here.

Port accesses are performed in program order with respect to each other and with respect to
memory accesses, and the machine never merges, splits, elides, or speculates a port access.
Reading a port may change device state, so a read is not safe to repeat unless the device
says it is.

### port_in

**Syntax:**

    port_in rp rd

**Encoding:** Length class `op r r`, three bytes, opcode `$C2` in the opcode-map appendix.

**Operation:** The machine reads one word from the port named by the low quarter-word of rp
and writes that word into rd. A populated port supplies whatever the device-surface chapter defines
for it, and a device register narrower than a word supplies its value in the low bytes with
every undefined bit zero. An unpopulated port supplies zero. The port register is not
modified. When rd is r0 the port access still happens, including any side effect the device
attaches to being read, and the value is discarded.

**Traps:** The privileged-operation trap when the machine is at user level, in which case no
port access occurs and rd is unmodified.

**Example:**

    port_in r4 r5

The kernel reads the port whose identifier sits in the low quarter-word of r4 into r5.

### port_out

**Syntax:**

    port_out rs rp

**Encoding:** Length class `op r r`, three bytes, opcode `$C3` in the opcode-map appendix. The
operand byte for rs precedes the operand byte for rp, matching the source-to-destination
order of the syntax.

**Operation:** The machine writes the full 64-bit word in rs to the port named by the low
quarter-word of rp. A populated port takes whatever the device-surface chapter defines for it, and a
device register narrower than a word takes the low bytes and ignores the rest. A write to an
unpopulated port is discarded with no other effect. Neither register is modified. Any side
effect the device attaches to the write takes place before the following instruction
executes.

**Traps:** The privileged-operation trap when the machine is at user level, in which case no
port access occurs.

**Example:**

    port_out r6 r4

The word in r6 goes to the port whose identifier sits in the low quarter-word of r4.

## Conformance notes

The properties below are directly testable by a binary, and a conforming machine exhibits all
of them.

- Each of the ten branch predicates agrees with the compare instruction of the same predicate
  on every pair of operand values, including the boundary pairs where the signed and unsigned
  answers differ.
- A branch, a jump, and a call with a displacement of zero fall through to the following
  instruction, and a taken branch whose displacement is -7 transfers to itself, since a
  branch is seven bytes long.
- A `call r31` transfers to the address r31 held before the call and leaves r31 holding the
  address of the instruction after the call.
- A transfer to an unmapped address raises a page fault whose faulting address is the target,
  and after that fault r31 already holds the link when the transfer was a call.
- A `sys` executed at user level and a `sys` executed at supervisor level both deliver the
  syscall trap with the same aux word and the same pc word.
- Each of `trap_return`, `halt`, `wait_for_interrupt`, `tlb_invalidate_all`,
  `tlb_invalidate_address`, `port_in`, and `port_out` raises the privileged-operation trap
  when executed at user level, and none of them has any other effect in that case.
- A `csr_read` and a `csr_write` naming a number with privilege field `%10` both raise the
  illegal-operand trap at supervisor level as well as at user level.
- A `csr_write` at supervisor level to a number whose read-only bit is set raises the
  illegal-operand trap, and a following `csr_read` of that number returns the value it held
  before the attempt. At user level the same write to a supervisor-privilege number raises
  the privileged-operation trap instead, because the privilege check is ordered first.
- A read of any unpopulated port yields zero, and a write to any unpopulated port followed by
  a read of it yields zero again.
- A `breakpoint` followed by a `trap_return` from its handler resumes at the instruction after
  the breakpoint, while a fault-class trap resumed the same way re-executes its instruction.
