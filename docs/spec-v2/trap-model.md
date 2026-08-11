# Trap Model

This chapter is normative. It fixes the cause numbering, the four-word frame the machine
pushes on every trap, the contents of the status word, what the auxiliary word carries for
each cause, how the machine finds a handler, how a handler returns, and how external
interrupts share all of that machinery. A machine that follows this chapter delivers the
same trap, with the same frame contents, at the same instruction, as every other conforming
machine.

Two properties govern everything below. Every condition that a conventional machine leaves
undefined has a named outcome here, so a program either completes an instruction or takes a
trap with a cause number and nothing else happens. Trap delivery is precise: when a trap
fires, every earlier instruction has taken full effect and no later instruction has taken
any effect at all.

## Fault, trap, and interrupt

Maize v2 sorts every entry into the kernel into three classes, and the class decides which
program counter the frame captures.

- A **fault** is a condition the instruction ran into. The frame captures the address of the
  faulting instruction itself, so a handler that removes the condition returns and the
  instruction runs again.
- A **trap** in the narrow sense is a condition the instruction asked for. The frame captures
  the address of the following instruction, because there is nothing to retry. The syscall
  entry and the breakpoint are the two members of this class.
- An **interrupt** arrives from outside the instruction stream. The frame captures the
  address of the instruction that would have run next, so the interrupted program resumes
  exactly where it stopped.

The class of every cause appears in the cause table below, and it is fixed. No cause is a
fault in one situation and a trap in another.

## The cause enumeration

Every cause has a stable number in the range 0 through 255. The number is the index into the
vector table and it is the value the frame's cause word carries. Causes 0 through 31 are
synchronous, causes 32 through 255 are external interrupts, and the device-surface chapter
assigns interrupt causes to sources.

| Cause | Name | Class | Raised by | Auxiliary word |
|------:|:-----|:------|:----------|:---------------|
| 0 | Illegal instruction | Fault | A reserved opcode byte, a reserved entry on an implemented extension page, or an escape byte for an extension the machine does not implement | The offending byte, zero-extended |
| 1 | Illegal operand | Fault | An operand byte whose form field is undefined for its slot class, an immediate an instruction defines as invalid, a floating-point rounding-mode field holding a reserved encoding, a control-and-status-register number that is well formed but unimplemented, a control-and-status-register number whose privilege field holds a reserved encoding, a write to a read-only control and status register, an encoding of a block-memory instruction that names the same register in more than one of its three operand slots or that names r0 in a pointer or count slot, or a value written to a control and status register that the register does not accept | The offending byte or value, zero-extended. Under subcode 6 the offending value is the value the write supplied, not the register number |
| 2 | Divide error | Fault | An integer divide or remainder by zero, or the signed division of the most negative value by -1 | Zero (the condition is in the subcode) |
| 3 | Breakpoint | Trap | The `breakpoint` instruction | Zero |
| 4 | Privileged operation | Fault | A privileged instruction executed at user level, or an access to a control and status register whose number names a higher privilege level than the current one | The offending opcode byte, or the register number for a control-and-status-register access |
| 5 | (reserved) | n/a | Nothing. The number is reserved and never delivered | n/a |
| 6 | (reserved) | n/a | Nothing. The number is reserved and never delivered | n/a |
| 7 | Syscall | Trap | The `sys` instruction | The syscall number, zero-extended from 8 bits |
| 8 | Page fault on fetch | Fault | An instruction fetch that address translation cannot satisfy | The faulting virtual address |
| 9 | Page fault on load | Fault | A load, a block-memory read, or a page-table read on behalf of one that address translation cannot satisfy | The faulting virtual address |
| 10 | Page fault on store | Fault | A store or a block-memory write that address translation cannot satisfy | The faulting virtual address |
| 11 | Physical-memory fault | Fault | An instruction fetch, a load, a store, a block-memory access, or a page-table read on behalf of translation that touches a physical address outside populated memory | The faulting physical address |
| 12..31 | (reserved) | n/a | Nothing. These numbers are reserved for future synchronous causes and are never delivered | n/a |
| 32..255 | External interrupt | Interrupt | A device or controller that makes the cause pending while it is enabled | Zero, unless the source defines a subcode word |

Causes 5 and 6 are held empty on purpose. Maize v1 spent them on a segment-bounds violation
and a stack fault, neither of which exists in v2, and leaving the numbers dark means a
handler table carried over from v1 cannot silently alias an old cause onto a new one.

Cause 11, the physical-memory fault, reports an access that reached a physical address the
machine does not populate. The address map in the boot-information block is what defines
populated memory, and an access to a physical address that no populated region of that map
covers raises cause 11 with the offending physical address in the auxiliary word. The cause
exists in every translation mode. In bare mode the offending physical address is the address
the program formed, since bare mode maps every virtual address to itself. Under Sv48 the
offending address is the physical address the walk produced, or the address of a page-table
entry the walk read, because a page-table read is a physical access that is never translated.
A page fault and a physical-memory fault never compete for the same access. Translation either
fails, and delivers cause 8, 9, or 10, or it succeeds and hands a physical address on, which is
the address cause 11 then judges. The cause defines one subcode, 0, so its subcode field always
reads zero.

A reserved cause number is never delivered by any input. Reserved numbers exist so that a
future extension can claim one, and an extension that claims a number states the class, the
subcode meanings, and the auxiliary word in its own document.

### Subcodes

Where one cause covers conditions a handler wants to tell apart, the cause word carries a
subcode alongside the cause number. The causes listed below are the ones that define more
than one subcode. Every cause this section does not list writes a subcode of zero, and so
does every condition of a listed cause that the list assigns the value 0 to.

- Cause 1, illegal operand, uses subcode 0 for an undefined operand form field, 1 for an
  invalid immediate, 2 for a reserved floating-point rounding mode, 3 for an unimplemented
  control-and-status-register number, 4 for a write to a read-only control and status
  register, 5 for a block-memory encoding that names the same register in more than one
  of its three operand slots or that names r0 in a pointer or count slot, 6 for an invalid
  value written to a control and status
  register, meaning a value that sets a bit the register reserves or that holds a reserved
  encoding in a field the register defines, and 7 for a control-and-status-register number
  whose privilege field holds a reserved encoding. Subcode 6 covers the status word a
  `trap_return` frame supplies as well, since that word is written into the status register.
- Cause 2, divide error, uses subcode 0 for a division by zero and 1 for quotient overflow,
  which is the signed division of the most negative value by -1.
- Causes 8, 9, and 10, the page faults, use subcode 0 when translation found no valid
  mapping and 1 when translation found a mapping that the access lacks permission for.

An extension that adds a trap condition to a cause this chapter already defines assigns that
condition a new subcode in the extension's own document, taking the next free value for that
cause. The subcode space of a cause is therefore open at the top while every value this
chapter assigns is fixed for all time.

## The frame

On every trap the machine pushes exactly four words, in this order, to the stack named by
the trap-stack control and status register: the program counter, the status word, the cause
word, and the auxiliary word. No general-purpose register is saved, no condition state
exists to save, and the frame is the entire hardware-visible cost of entering a handler.

The trap stack is full-descending. The machine subtracts 32 from the trap-stack register,
writes the four words at ascending addresses from the new value, and leaves the new value in
the register. A handler therefore reads the frame at these offsets from the trap-stack
register's current value:

    +0    pc      the captured program counter
    +8    status  the interrupted context's status word
    +16   cause   the cause number and subcode
    +24   aux     the per-cause auxiliary word

The frame is written with word-width stores to the trap stack, translated under the current
paging root as supervisor-privilege store accesses. A page fault raised by any of those four
stores is a double fault and is handled as the double-fault section below describes.

The trap-stack register carries a 16-byte alignment requirement, so the four-word frame is
always word-aligned and a handler may push its own 16-byte-aligned frames beneath it. A
`csr_write` of a misaligned value to the trap-stack register raises the illegal-operand trap
with subcode 6, which is the same mistake-proofing that makes a reserved opcode trap rather
than execute.

### The captured program counter

The captured program counter is the faulting instruction's own address for a fault, the
following instruction's address for a trap, and the next instruction's address for an
interrupt. For an instruction on an extension page the faulting address is the address of the
escape byte, because the escape byte is where the instruction begins and where a restart
re-decodes from.

### The status word

The status word on the frame is a snapshot of the machine's live status register, taken
before the machine changes anything. Both the register and the frame word use one layout.

    bits  1:0   priv      privilege level: %00 user, %01 supervisor, %10 and %11 reserved
    bit     2   ie        external-interrupt enable
    bits 63:3   reserved, read as zero and written as zero

The privilege field holds two of its four encodings and reserves the other two. Encoding
`%10` is the room the privileged architecture keeps for a third level, and encoding `%11` is
reserved with it. A `csr_write` to the status register whose value names a reserved privilege
encoding, or sets any reserved bit, raises the illegal-operand trap with subcode 6 and changes
nothing.

The interrupt-enable bit governs external interrupts only. No synchronous cause is maskable,
because a masked divide error would be undefined behavior arriving through the back door.

### The cause word

The cause word packs the cause number and the subcode, and the packing is fixed for all
time.

    bits  7:0   cause     the cause number, 0 through 255
    bits 15:8   subcode   the subcode, 0 where the cause defines none
    bits 63:16  reserved, written as zero

A handler recovers the cause with `and r4 $FF r5` and the subcode with a shift of 8 followed
by the same mask. The reserved high bits are written as zero by every conforming machine, so
a handler that tests the whole word against a constant behaves the same everywhere.

### The auxiliary word

The auxiliary word carries the one value the handler needs that is not derivable from the
cause and the program counter, and the cause table above states it for every cause. Four
patterns cover the whole enumeration. A decode-class fault reports the byte or value the
machine objected to. A page fault reports the faulting virtual address exactly as the
instruction computed it, not the page base. The physical-memory fault reports the faulting
physical address, which is the address that missed populated memory rather than the virtual
address the instruction started from. A cause with nothing to report writes zero, and
zero is a real value rather than an unspecified one, so a conformance binary tests for it.

Maize v1 latched a faulting address and a packed error code into two control registers. Maize
v2 has no equivalent registers, because the frame carries the address in the auxiliary word,
the access kind in the cause number, the present-or-absent distinction in the subcode, and
the interrupted privilege level in the status word. A nested fault therefore cannot destroy
the outer fault's description, which was the hazard the latched-register design carried.

## Vectored dispatch

The machine finds a handler through a table of handler addresses indexed by cause number.
The trap-vector-base control and status register holds the virtual address of that table.

The table has 256 entries of 8 bytes each, occupying 2 KiB, and entry `c` lives at the base
address plus `c` times 8. Each entry holds a full 64-bit virtual address. The base is
2 KiB-aligned, so the table never straddles more pages than it has to, and a `csr_write` of a
value whose low 11 bits are not all zero raises the illegal-operand trap with subcode 6.

Delivery proceeds in a fixed order, and every step is observable.

1. The machine determines the cause number, the subcode, and the auxiliary value.
2. The machine reads the handler address from the vector table, as a supervisor-privilege
   load translated under the current paging root.
3. If that entry is zero, no handler is installed and the machine halts as the no-handler
   section below describes.
4. The machine pushes the four-word frame to the trap stack.
5. The machine sets the privilege level to supervisor and clears the interrupt-enable bit in
   the status register, leaving every other status bit unchanged.
6. The machine sets the program counter to the handler address and resumes execution there.

The vector fetch happens before the frame push so that an uninstalled handler is a clean halt
with the machine's state untouched rather than a halt with a half-built frame in memory.

Address translation stays on across the whole sequence, and the paging root does not change,
so the handler runs in the interrupted context's address space. A kernel that wants a
separate address space per handler switches the paging root as its first act, which is
software policy rather than machine behavior.

The v1 table lived at a fixed address of `$1000`. The v2 table is wherever the trap-vector-
base register points, because a relocatable table costs one register and buys a kernel the
freedom to place its own data structures.

## No handler installed

A vector-table entry of zero means no handler is installed for that cause. When such a cause
fires, the machine halts. It pushes no frame, changes no register other than the halt-cause
register, and executes nothing further.

The halt-cause register, which is read-only and readable at supervisor level, records why
the machine stopped:

    bits  7:0   cause     the cause number of the condition that halted the machine
    bits 15:8   subcode   that condition's subcode
    bits 17:16  kind      0 = the halt instruction executed, 1 = no handler installed,
                          2 = double fault
    bits 63:18  reserved, read as zero

A kind of 0 records the `halt` instruction, which has no cause and no subcode, so the cause
and subcode fields read zero in that case and two machines cannot differ in what they leave
there.

Halting on an uninstalled handler is what makes a bare-metal program debuggable. A divide by
zero in a program with no kernel under it stops the machine with cause 2 in the halt-cause
register rather than wandering into whatever follows.

## Nested traps and double faults

A trap taken inside a handler is an ordinary trap. The machine pushes its frame beneath the
outer frame, because the trap-stack register still holds the address of the outer frame's
lowest word, and delivery is otherwise identical. Nesting is bounded only by the memory the
trap stack has beneath it.

A handler that uses the trap stack as its working stack moves the trap-stack register down
past its own working area, so that a nested frame lands beneath that area rather than on top
of it. A handler that runs on a stack of its own leaves the trap-stack register alone. Both
disciplines are software policy, and the machine's only rule is the mechanical one: a frame
is pushed at the trap-stack register's current value minus 32, always.

A **double fault** is a page fault or a physical-memory fault raised by the vector-table
read or by any of the four frame stores. The machine does not attempt to deliver it, because
delivering it would take the same failing path again. The machine halts instead, writes the
original cause and subcode into the halt-cause register with a kind of 2, and executes
nothing further. An unmapped or
read-only trap stack, and equally a trap stack or vector table sitting in unpopulated
physical memory, is therefore a deterministic stop rather than an unbounded recursion. The
physical-memory case matters most in bare mode, where no page fault exists and cause 11 is
the only way a bad trap-stack address can fail.

Nothing else is a double fault. A fault raised by a handler's own instructions, however soon
after entry, is an ordinary nested trap.

## Returning from a trap

The `trap_return` instruction pops the frame and resumes the interrupted context. It is
privileged, so a user-mode program cannot forge a frame and return into supervisor mode with
it. Executing `trap_return` at user level raises the privileged-operation fault.

The instruction reads the four words at the trap-stack register, validates the status word,
then commits. Validation comes first and it is total: if the status word on the frame names a
reserved privilege encoding or sets any reserved bit, `trap_return` raises the illegal-operand
trap with subcode 6 and changes nothing at all, including the trap-stack register. A frame the
machine itself wrote always passes, so only a frame software has edited can fail.

On a valid frame the machine writes the frame's status word into the status register, sets the
program counter to the frame's program-counter word, adds 32 to the trap-stack register, and
resumes. The cause and auxiliary words are not read back into anything; they are the handler's
to consume. Restoring the status word is what returns the machine to user mode and what
re-enables interrupts, since both live in that word.

A page fault or a physical-memory fault raised while popping the frame abandons the pop,
leaves the trap-stack register unchanged, and is delivered as an ordinary fault, so the
`trap_return` re-executes cleanly once the fault is serviced. Neither is a double-fault
condition, because it occurs at `trap_return` rather than at trap entry.

A handler that wants to resume somewhere other than where the trap happened edits the
program-counter word on the frame before executing `trap_return`. That is how a kernel
implements signal delivery, single-stepping, and instruction emulation, and it needs no
separate mechanism.

## Registers across a trap

The machine saves no general-purpose register on a trap, and it restores none on
`trap_return`. Every one of r0 through r31 holds, at the first instruction of the handler,
exactly what it held when the trap fired. What to preserve is the kernel's decision, and the
kernel makes it with the calling convention in hand.

Three consequences follow, and the second one is the whole point of the design.

- A handler that will return to the interrupted context saves the registers it is about to
  clobber and restores them before `trap_return`, exactly as a called function does.
- A syscall handler saves only what its own code uses, which for a short syscall is a handful
  of registers, because the calling convention already tells the interrupted program that the
  caller-saved registers may change across the boundary.
- A context switch saves all 31 general registers, because it is switching to a different
  program rather than returning to this one, and that is the only path that pays the full
  cost.

Maize v1 pushed thirteen registers on the way into every handler whether the handler wanted
them or not. Maize v2 has no equivalent, and there is no instruction that saves a fixed
register set, so the cost cannot creep back in.

A handler's first act is to read the trap-stack register, and that read necessarily lands in
a general register, so entering a handler costs one register before anything can be saved.
Two disciplines cover the two kinds of handler. A syscall handler pays that cost where the
convention has already priced it in: r2 is `a0`, the register the syscall contract rewrites
with the result anyway, so claiming it first loses the interrupted program nothing. A
handler that owes the interrupted program every register, which is what an interrupt handler
owes, banks one instead: the kernel preloads the supervisor scratch register with a pointer
to a per-context save area, the handler's first instruction is `csr_swap r2 scratch r2`,
which yields that pointer while parking the interrupted r2 in the scratch register, the
handler saves what it uses through the pointer, recovers the parked value with a `csr_read`
into a register already saved, stores it, and re-arms the scratch register before
`trap_return`. Nothing is lost at any point, which is the property the swap exists to
provide. A short syscall-handler prologue and epilogue look like this,
with the trap stack doubling as the handler's working stack:

    handler:
        csr_read $4001 r2           ; the frame address, into the one register the
                                    ; syscall contract lets the handler claim outright
        store r10 @r2-$08           ; save the registers the handler goes on to use
        store r11 @r2-$10
        subtract r2 $10 r10
        csr_write r10 $4001         ; nested frames now land beneath the saved pair
        load @r2+$18 r10            ; the aux word, carrying the syscall number
        ...                         ; the body works in r10 and r11, finishes with the
                                    ; frame, and leaves the result in r2
        csr_read $4001 r11
        add r11 $10 r11             ; the frame address again
        csr_write r11 $4001         ; the trap-stack register is back on the frame
        load @r11-$08 r10
        load @r11-$10 r11
        trap_return

## The syscall boundary

The `sys` instruction is the deliberate entry from user code into the kernel, and it is not
privileged. It raises cause 7, which is trap-class, so the captured program counter is the
address of the instruction after the `sys` and `trap_return` resumes there.

The syscall number reaches the kernel two ways at once. It is the instruction's own operand,
either the 8-bit immediate or the low byte of the named register, and the machine copies it
into the auxiliary word of the frame. A dispatcher therefore reads the number from the frame
without decoding the instruction that made the call.

Arguments and results live in registers and cross the boundary untouched. The calling
convention's argument registers r2 through r9 hold the arguments when `sys` executes, and they
still hold them at the handler's first instruction, because the machine saves nothing. The
result lands in r2, with r3 carrying the second half of a two-word result, and it is still
there when the interrupted program resumes, because `trap_return` restores nothing. The
kernel's obligation is the ordinary one a callee has: preserve the callee-saved registers,
and leave the others to the caller's expectations.

This is the v1 shape, preserved on purpose. The instruction, its cause, the number's home in
the instruction, and the arguments-in-registers convention all survive the clean break, so a
guest kernel ports across as a recompile plus a rewritten trampoline rather than a redesign.
The one v1 mechanism that does not survive is the pair of opcodes that selected the syscall
provider, which is now a bit in a control and status register.

### Syscalls and translation

Nothing in this design requires a translator to treat a syscall as a wall. The machine's
behavior at `sys` is a frame push, a status change, and a jump, and none of those depends on
where the instruction sits inside a translated region or on how much work the translator did
around it. A translator that can prove the register state at the boundary is free to treat
`sys` as a call-shaped edge, to keep its translated code resident across it, and to resume the
same region on `trap_return`.

This paragraph is a statement about what the specification does not forbid rather than a
requirement on any implementation. The measured cost of the v1 arrangement, where the syscall
ended a compiled block outright, is the reason it is written down.

## Restartability

A fault leaves the machine in a state from which the faulting instruction can run again and
produce the result it would have produced had the fault never happened. This is the contract
the execution-model chapter states for the whole instruction set, and the trap model is where
it becomes observable.

For a single-step instruction the contract is simple: the instruction has taken no
architectural effect, so its destination register and every byte of memory it would have
written are unchanged, and re-executing it after the handler repairs the condition completes
it once.

For the block-memory instructions, which move an unbounded number of bytes, the contract is
different and equally exact. Those instructions define their visible mid-operation register
state: at every point where the machine can stop, the count register holds the bytes not yet
transferred and each pointer register holds the lowest address in its region not yet
transferred, which is the direction-neutral invariant the memory reference chapter fixes. A
fault therefore captures the block instruction's own address, leaves the registers describing
precisely the remaining work, and re-executing the instruction after the handler runs finishes
the job with no byte copied twice and none skipped.

An external interrupt taken during a block-memory instruction follows the same rule. The
captured program counter is the block instruction's own address rather than the following
instruction's, and the registers describe the remaining work, so `trap_return` resumes the
transfer. The block-memory family is the only place in the base where an interrupt is taken
part-way through an instruction, and the reason it is allowed there is that the architecture
defines the intermediate state.

## External interrupts

An external interrupt is a cause in the range 32 through 255 that a device or controller makes
pending. Interrupts and synchronous traps share the vector table, the frame layout, and
`trap_return`, so a kernel writes one entry sequence and one exit sequence.

This chapter owns the mapping from an interrupt line to a cause number, and the mapping is
fixed for all time. The cause number of a device interrupt is 32 plus the device's interrupt
line index, and the line index equals the device class code the device-surface chapter
assigns. A machine that does not carry a class never makes that class's cause pending.

### Pending and enable state

Two arrays of control and status registers hold the interrupt state, and each array covers all
256 cause numbers in four 64-bit registers. Register `n` of an array holds causes `64n`
through `64n + 63`, with the cause's number modulo 64 selecting the bit.

- The **pending** registers are read-only. A device sets a bit by raising its interrupt, and
  the machine clears the bit when it delivers that cause. Software reads them to see what is
  waiting and cannot write them, because a pending bit is a report of the world rather than a
  request to the machine.
- The **enable** registers are readable and writable at supervisor level. A cause is
  deliverable only while its enable bit is set. Bits 0 through 31 of the first enable register
  correspond to synchronous causes, which are never maskable, so those bits read as zero and a
  write to them is required to be zero; a write that sets any of them raises the
  illegal-operand trap with subcode 6.

The status register's interrupt-enable bit sits above both arrays as the global gate. An
interrupt is delivered only when its pending bit is set, its enable bit is set, and the status
register's interrupt-enable bit is set.

### Delivery

The machine tests for a deliverable interrupt between instructions, after one instruction has
taken full effect and before the next has taken any, and additionally at the defined
mid-operation boundaries of the block-memory instructions. When more than one cause is
deliverable, the machine takes the lowest-numbered one, which makes the choice deterministic
and therefore testable.

On delivery the machine clears that cause's pending bit first, so the same interrupt is not
delivered twice for one assertion, then follows the ordinary delivery sequence: vector fetch,
frame push, privilege raised to supervisor, interrupt-enable bit cleared, jump to the handler.
The frame's status word carries the interrupted context's interrupt-enable bit as it was, so
`trap_return` re-enables interrupts without the handler doing anything.

Clearing the pending bit is the machine's acknowledgement, and it is not the device's. A
device that latches a condition of its own re-raises the cause until the handler clears that
latch through the device's own registers, which the device-surface chapter specifies per
device.

### Waiting

The `wait_for_interrupt` instruction suspends the machine until some cause has both its
pending bit and its enable bit set, then continues at the following instruction. A cause
whose pending bit is set while its enable bit is clear does not wake the machine. It is
privileged, and executing it at user level raises the privileged-operation fault. When some
cause is already pending and enabled at the moment the instruction executes, the instruction
completes immediately, so no interrupt is lost by racing to sleep.

The instruction's completion does not depend on the status register's interrupt-enable bit,
and this matters. A kernel that runs its idle loop with interrupts masked wakes on the pending
bit, polls the pending registers, and services the source itself. A kernel that runs with
interrupts enabled takes the interrupt at the boundary immediately following the
`wait_for_interrupt`, so the handler runs and the instruction after the wait executes on
return. Both cases are defined, and neither leaves the machine asleep with work outstanding.

## Conformance notes

Every property below is directly testable by a binary, and a conforming machine exhibits all
of them.

- Each of causes 0, 1, 2, 3, 4, 7, 8, 9, 10, and 11 can be provoked, and each delivers the
  cause number, subcode, and auxiliary word this chapter assigns.
- A store to an address above the highest address the boot-information block's address map
  covers raises cause 11, the auxiliary word holds that physical address, and the store leaves
  memory unchanged.
- A fault's captured program counter equals the faulting instruction's first byte, and
  returning from the handler without repairing the condition delivers the identical trap
  again.
- The breakpoint and syscall traps capture the address of the following instruction, and
  `trap_return` resumes there.
- The four frame words appear at offsets 0, 8, 16, and 24 from the trap-stack register's value
  on handler entry, and the register's value on entry is 32 below its value before the trap.
- Every general-purpose register holds, at the handler's first instruction, the value it held
  when the trap fired.
- `trap_return` executed at user level raises cause 4, and `trap_return` on a frame whose
  status word sets a reserved bit raises cause 1 with subcode 6 without altering the trap-stack
  register.
- A cause whose vector-table entry is zero halts the machine with that cause and a halt kind
  of 1, and no frame is written.
- A trap whose frame push meets an unwritable trap stack halts the machine with the original
  cause and a halt kind of 2, and the handler does not run.
- A nested trap's frame lies 32 bytes below the outer frame when the handler has not moved the
  trap-stack register.
- With two interrupt causes pending and enabled, the lower-numbered one is delivered first, and
  its pending bit is clear at the handler's first instruction while the other's remains set.
- An interrupt raised during a long `block_copy` is delivered before the copy completes, the
  captured program counter is the `block_copy` instruction's own address, and `trap_return`
  finishes the copy with every byte transferred exactly once.
