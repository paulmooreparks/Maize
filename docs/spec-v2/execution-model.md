# Execution Model

This chapter is normative. It fixes the order in which a Maize v2 machine executes
instructions, the status of the program counter as state that is not a register, the
atomicity and restartability contract that every instruction in the book inherits, the
determinism guarantee that makes a run reproducible, and the points at which an interrupt
enters the instruction stream.

Nothing in this chapter is specific to one instruction. Where an individual instruction
narrows a rule stated here, the instruction inventory says so in that instruction's entry,
and the block-memory family is the only place in the base where such a narrowing exists.

## The instruction cycle

A Maize machine executes instructions strictly in order, one at a time. It commits every
architectural effect of an instruction before it fetches the next one, and no program can
observe a partially executed instruction, an instruction executed early, or two instructions
in flight together. The machine exposes no pipeline, no speculation, and no reordering.

Each instruction passes through four steps.

1. **Fetch.** The machine reads the instruction bytes beginning at the address in the program
   counter. A fetch is an ordinary access to instruction memory and is subject to the same
   translation and permission rules as any other access, so a fetch from an unmapped or
   non-executable page raises a page fault.
2. **Decode.** The machine interprets those bytes exactly as the instruction-encoding chapter
   specifies: it reads the opcode byte and any escape byte, obtains the instruction's length
   from the length table before it reads any operand, reads the operand bytes in order and
   checks each form field against its declared slot class, and reads the immediates in order.
   A reserved opcode, an unimplemented extension page, or an undefined operand form ends the
   instruction here with a fault.
3. **Execute.** The machine performs the operation the inventory entry defines, reading the
   architectural state the instruction names and computing every value it writes.
4. **Advance.** The machine commits the instruction's writes to registers and to memory, and
   the program counter takes the address of the next instruction. For an instruction that
   transfers control, that address is the transfer target. For every other instruction, it is
   the address of the instruction's first byte plus its length.

Step 4 is a single indivisible transition from one architectural state to the next. Either
all of an instruction's effects are visible or none of them are, with the block-memory
exception named below.

## The program counter

The program counter holds the address of the instruction the machine is executing. It is
architectural state, and it is not a register: no register number names it, no operand byte
can reach it, and none of `move`, `csr_read`, `csr_write`, and `csr_swap` reads or writes
it. The register model is the whole of the register file, and the program counter is outside
it.

Software reads the program counter through exactly one instruction. The instruction
`pc_add $imm rd` writes the address of the following instruction plus a sign-extended 32-bit
displacement into rd, so `pc_add #0 r5` places the address of the instruction after the
`pc_add` into r5. That instruction is the whole of the read interface, and position-independent
code is built on it.

Software writes the program counter only by transferring control. The branches, `jump`,
`call`, `return`, `sys`, `trap_return`, and the taking of a trap or an interrupt are the
complete list of ways the program counter takes a value other than the address of the next
sequential instruction. There is no instruction that adds to the program counter in place and
no instruction that copies a register into it, because `jump rs` already expresses that with a
name a reader can find.

Three properties of the program counter hold everywhere.

- The value the program counter holds during the execution of an instruction is the address of
  that instruction's first byte, which is the address a fault reports and the address to which
  a restart returns.
- The phrase "the address of the following instruction", which the inventory uses for `pc_add`,
  for `call`, for the branch and jump displacement base, and for `breakpoint`, always means the
  address of the instruction's first byte plus its encoded length, computed before execution
  and unaffected by anything the instruction does.
- Program counter arithmetic wraps modulo 2^64, so advancing past the highest address yields
  address zero rather than raising a distinct fault. Whether the resulting address is
  accessible is a question for the memory model, and a fetch from an inaccessible address
  raises the fault the memory model assigns it, a page fault under translation or the
  physical-memory fault outside populated memory.

Instructions carry no alignment requirement. Any byte address is a legal instruction address,
a branch displacement is a signed byte count, and the machine never masks the low bits of a
control-transfer target.

## Instruction atomicity and restartability

Every instruction in the base is atomic with respect to faults. When an instruction raises a
fault, that instruction has either completed every one of its effects or performed none of
them, and the machine takes the trap in the state that precedes the instruction. No
instruction leaves a register half-written, a memory region half-stored, or a destination
updated while a source access is still outstanding.

This rule is what makes fault handling a service rather than a recovery. A page fault raised
by `load @r9+$20 r4` leaves r4 and all of memory as they were, the kernel maps the page, and
`trap_return` re-executes the same instruction, which decodes identically because its bytes
and its length are unchanged. The same holds for a store, for a division that raises the
divide-error trap, for an instruction whose fetch faults partway through its own bytes, and
for an instruction that raises the illegal-operand trap on its second operand byte after its
first operand byte decoded cleanly.

A faulting instruction reports the address of its own first byte, meaning the escape byte when
the instruction is on an extension page and the opcode byte otherwise. A machine never reports
the address of the operand byte or the immediate at which decoding stopped, and never reports
the address of the following instruction, because the reported address is the address a
handler resumes at. Trap-class events differ, and the trap-model chapter states which events
report the following instruction instead; `breakpoint` and `sys` are the two in the base.

The block-memory family is the single exception to the all-or-nothing rule, and its exception
is exact rather than open-ended. `block_copy`, `block_copy_forward`, and `block_set` may be
interrupted or may fault after transferring some of their bytes, and when that happens their
three named registers describe the remaining work: the count register holds the number of
bytes not yet transferred and each pointer register holds the lowest address in its region
not yet transferred, as the memory reference chapter fixes. The instruction reports its own
address, and re-executing it completes the
operation with no byte transferred twice and none skipped. Those registers are the entire
observable mid-operation state, and the inventory entry for each instruction fixes it; a
machine holds no hidden progress state that survives a trap.

Restartability is a property of the machine rather than of a particular handler, so it holds
whether or not a kernel is installed and whether or not the fault is serviced. A conformance
binary tests it by arranging a fault at every instruction that can raise one and comparing the
full architectural state at trap entry against the state before the instruction.

## Determinism

A Maize machine executing a given image from a given initial state produces one and only one
sequence of architectural states. Two conforming machines, given the same image, the same
initial state, and the same device inputs delivered at the same instruction boundaries, are
indistinguishable to the program running on them.

The base machine has a single hart. There is no second instruction stream, no concurrent agent
that modifies memory behind the program, and therefore no instruction whose result depends on
timing. The base spends no opcode on atomic read-modify-write or on ordering, and the atomic
extension owns both, because an ordering instruction on a single-hart machine would be an
instruction with nothing to order.

Every instruction in the base is a pure function of architectural state, meaning the register
file, the control and status registers, the program counter, and memory. Given identical state
and identical instruction bytes, an instruction produces an identical result and an identical
next state on every conforming machine. The specification defines no operation whose value is
approximated, rounded at the implementation's discretion, taken from uninitialized storage, or
otherwise left to the machine to choose. Floating-point arithmetic is bit-exact under the
IEEE 754 rules the inventory states, including the rounding mode the control and status
register holds, so a floating-point program is as reproducible as an integer one.

Three categories of behavior sit outside the pure-function rule, and each is defined rather
than open.

- Device interactions enter through `port_in`, `port_out`, and interrupts, and they are the
  only channel by which anything outside the machine influences a run. The device-surface
  material fixes what each port yields, an unpopulated port reads zero and discards writes, and
  a machine carrying only the classes the device-surface chapter requires is still a conforming
  machine that runs deterministically.
- The order in which `block_copy` transfers bytes is chosen by the implementation, which is why
  its result is defined as though the source were read in full before any write, and why its
  mid-operation state is defined as the remaining-work description rather than as a direction
  of travel. Software that needs a specific direction uses `block_copy_forward`, whose order is
  fixed.
- A program that edits a live page table and does not invalidate may observe the old
  translation, the new one, or each on different accesses, within the bound the
  privileged-architecture chapter states for the translation cache. A program that
  invalidates as that chapter directs never observes this latitude.

No fourth category exists. There is no undefined behavior anywhere in this specification: every
byte string the machine can fetch either executes with a defined result or raises a defined
trap, and every operand value, including every value a compiler would call erroneous, has a
stated outcome.

## Interrupts and the instruction boundary

An interrupt is taken between instructions. The machine completes the instruction it is
executing, commits its effects, and then, before it fetches the next instruction, checks
whether an interrupt is pending and deliverable. When one is, the machine takes it at that
boundary with the program counter naming the instruction it has not yet fetched, so the
interrupted program resumes on `trap_return` exactly where it stopped, having neither repeated
nor skipped an instruction. An interrupt never lands in the middle of an instruction's effects.

The block-memory instructions are interruptible partway rather than only at their own
boundaries, and the memory reference chapter states the rule that governs the granularity.
That is what keeps a long copy from delaying an interrupt for an unbounded time. When an
interrupt is taken during one of them, the machine
records the address of the block instruction itself, leaves the three named registers holding
the remaining-work description, and the interrupted instruction resumes by re-executing on
`trap_return`. That is the same mechanism restartability defines for a fault, used for a
different reason.

Priority at a boundary is fixed and needs no arbitration policy. A fault or a trap raised by an
instruction belongs to that instruction and is delivered when the instruction raises it, which
is before the boundary at which a pending interrupt could be taken; the interrupt remains
pending and is taken at the next boundary at which it is deliverable. When several interrupts
are pending at the same boundary, the trap-model chapter fixes which one the machine takes and
what happens to the rest.

Whether an interrupt is deliverable at all depends on state the privileged-architecture chapter
owns, and this chapter fixes only the timing. The base has no instruction that enables or
disables interrupts, because a mode toggle does not earn an opcode when a control and status
register bit expresses it, so software changes deliverability with `csr_write` and the change
takes effect no later than the boundary following that instruction.

`wait_for_interrupt` is the one instruction whose completion depends on an interrupt arriving.
A machine executing it suspends at that instruction until some interrupt cause has both its
pending bit and its enable bit set, then continues at the following instruction. A cause that
is pending but not enabled does not wake the machine. Whether the interrupt is taken at the
boundary following the `wait_for_interrupt` follows the ordinary delivery rules, including the
global interrupt-disable bit in the status register, so a handler that runs there returns to
the instruction after the wait. When some cause is already pending and enabled at the moment
the machine executes the instruction, it completes immediately, which means a program never
loses an interrupt by racing to sleep.

## Execution states

A machine is in one of three execution states, and the state is observable in the sense that a
conformance binary can distinguish all three by their effect on subsequent instructions.

- **Running.** The machine repeats the instruction cycle, taking interrupts at boundaries.
- **Waiting.** The machine has executed `wait_for_interrupt` and is suspended at it, taking no
  further instruction until some interrupt cause is both pending and enabled. A waiting machine leaves every
  register, every control and status register, and all of memory exactly as the
  `wait_for_interrupt` found them.
- **Halted.** The machine has executed `halt` and takes no further instruction. A halted
  machine does not resume, not for an interrupt and not for any device event, and only a reset
  starts it again.

An unhandled trap is not a fourth state. The trap-model chapter fixes what a machine does with
a trap it has no handler for, and its answer is stated in terms of these three states.

This chapter does not fix the state a machine holds at reset. The initial contents of the
control and status registers, the address at which execution begins, and the boot-information
block through which a program discovers the machine's configuration belong to the
boot chapter and the memory-model chapter.

## Conformance notes

The following properties are directly testable by a binary, and a conforming machine exhibits
all of them.

- An instruction that raises a fault leaves every general register, every control and status
  register the instruction would have written, and every byte of memory the instruction would
  have written unchanged, for every base instruction that can raise a fault.
- The address a fault reports equals the address of the faulting instruction's first byte, and
  executing `trap_return` against that frame re-executes the same instruction.
- A page fault taken partway through a `block_copy` leaves the two pointer registers and the
  count register describing the untransferred remainder, and re-executing the instruction
  completes the copy with a result byte-identical to the uninterrupted copy.
- `pc_add #0 rd` writes the address of the instruction following the `pc_add`, for a `pc_add`
  placed at any alignment.
- An interrupt taken at a boundary records the address of the instruction not yet executed, and
  `trap_return` resumes at that instruction with no instruction repeated and none skipped.
- The same image run twice from the same initial state with the same device inputs produces the
  same architectural state at every instruction boundary.
- A machine suspended in `wait_for_interrupt` holds every register and every byte of memory
  unchanged until some cause is both pending and enabled, and a machine that executes
  `wait_for_interrupt` with such a cause already present proceeds without losing it.
