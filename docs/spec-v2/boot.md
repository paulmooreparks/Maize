# Boot

This chapter is normative. It fixes what the machine looks like at the instant it begins
executing guest instructions, what the machine has done for the guest before that instant,
and what the guest is then responsible for. Everything here is observable by the first
instruction of a program, so a conformance binary linked at the reset address can check the
whole chapter.

Booting a Maize v2 machine is deliberately short. The machine places an artifact in memory,
builds the boot-information block, sets a defined register and control-register state, and
jumps to a fixed address in supervisor mode with paging off. There is no firmware phase, no
mode transition sequence, and no protocol negotiation between the machine and the software it
loads.

## The reset address

Execution begins at physical address `$0000000000001000`. This value is architectural, it is
identical on every conforming machine, and it does not depend on the artifact, on the loader,
or on any option the implementation offers.

Placing the reset address one page above zero leaves the first page free, which keeps a null
pointer dereference in early guest code out of the code it is dereferencing from. Fixing the
address rather than reading an entry point out of the artifact keeps the boot contract
identical for a raw memory image and for a structured executable, since a structured artifact
that wants to start elsewhere places a jump at the reset address.

## Privilege and translation at reset

The machine begins in supervisor privilege with paging off, and every address the guest
forms is a physical address until the guest turns translation on.

Supervisor privilege at reset is the only workable choice, because the first instruction has
to be able to install a trap vector, program the timer, and read a device port, and all three
are privileged. Paging off at reset is likewise the only workable choice, because there is no
page table until the guest builds one, and a machine that required a page table before its
first instruction would have no way to acquire one.

Paging off means translation is not performed at all rather than performed against an
identity mapping. No page table is consulted, no translation cache entry is created, and no
page fault is possible. Paging off removes the page fault and nothing else, so an access to a
physical address the boot-information block does not report as populated still raises the
physical-memory fault the trap-model chapter numbers as cause 11. The
privileged-architecture chapter fixes how the guest turns translation on and what the address
space looks like afterward.

## Initial register state

Every general register holds zero when the first instruction executes. That covers all
thirty-two registers, including r30 and r31, whose stack-pointer and link-register roles are
calling-convention roles that no hardware mechanism establishes.

A zero r30 is deliberate. The machine does not choose a stack for the guest, because the guest
knows where its stack belongs and the machine does not, and a machine-chosen stack would be a
guess that every real operating system immediately discards. Guest software establishes its
own stack before it executes a call, and until it does, the calling convention's stack rules
simply do not apply yet.

A zero r31 means a `return` executed before any `call` transfers to address zero, which is
below the reset address and therefore not code the guest placed. This is a defined outcome
rather than a special case: the machine transfers to address zero and executes whatever is
there, which on a freshly loaded machine is a page the artifact did not fill.

## Initial control-register state

Every control and status register the base defines has a defined value at reset, and the
privileged-architecture chapter, which owns the numbering, states the reset value with each
register. This chapter fixes the values that determine whether the machine runs at all.

- The paging-root register holds zero and translation is disabled, per the section above.
- The trap-vector register holds zero, so the guest installs a vector table before it can
  service a trap; the trap-model chapter defines what the machine does with a trap taken
  before a table is installed.
- The trap-stack register holds zero, so the guest sets it before it enables anything that
  can trap.
- The interrupt-enable bit is clear, so no device interrupt is delivered until the guest
  enables interrupts, whatever the devices are doing.
- The floating-point rounding mode is round-to-nearest-ties-to-even and every sticky exception
  flag is clear.
- The syscall-provider selection bit is clear, so the machine starts on whichever provider a
  clear bit 0 selects. Which providers a machine offers, and what the bit's two values select
  between, is a property of that machine and its operating system rather than of the
  architecture.
- The feature bitmap reports exactly the extensions the machine implements, and it is
  read-only, so it holds the same value at reset that it holds forever.
- The boot-information register holds the physical address at which the machine
  placed the boot-information block.

The reset value of a control and status register an extension defines belongs to that
extension, and nothing here constrains it beyond the requirement that it be defined.

## What the machine has already done

Four things are complete before the first instruction executes, and guest software may rely on
all four without checking.

The machine has placed the loaded artifact in physical memory. The bytes of the artifact are
readable at the addresses the artifact designates, and for a raw image that means the image
sits at the reset address.

The machine has built the boot-information block somewhere in physical memory and has written
its address into the boot-information register. The block's layout, its fields,
and the way its entries are walked belong to the memory-model chapter, which fixes them by
name; this chapter fixes only that the block exists, that it is complete, and that its address
is discoverable through the register rather than through a fixed address or a probe. The
memory map in the block is the guest's only sanctioned source for how much physical memory the
machine has and which of it is usable, and the extension list in the block is the versioned
companion to the feature bitmap.

The machine has placed every device in its reset state. A device present in the port space
answers its identification port, holds its interrupt-enable bit clear, has no pending status
condition, and has no registered bulk-transfer buffer. The device-surface chapter states each
class's reset condition as part of its contract.

The machine has zeroed every byte of physical memory the boot-information block reports as
usable and that the artifact did not itself fill. Guest software therefore does not have to
clear its own uninitialized data, and a machine cannot leak a previous run's contents into a
new one.

## What is the loaded software's problem

Everything else belongs to the software the machine loads, and the list below is exhaustive
for the base machine.

- Establishing a stack, by writing a stack pointer into r30 before executing a call.
- Installing a trap vector table and writing its address into the trap-vector register.
- Setting the trap-stack register before enabling anything that can trap.
- Building page tables and turning translation on, if it wants translation.
- Reading the boot-information block to discover how much memory exists and which extensions
  are present.
- Initializing, configuring, and driving every device it intends to use, including the timer
  it needs for any notion of elapsed time.
- Enabling interrupts, once it has a vector table and a trap stack.
- Deciding what user-level execution means, since the machine provides the privilege
  transition and nothing above it.

The machine offers no service the guest can call, because there is no firmware resident in the
machine after reset. Nothing in memory belongs to the machine, no address is reserved for a
callback, and no instruction reaches a facility this specification has not described.

## Artifacts

The machine boots whatever artifact the implementation loads. An implementation may load a raw
memory image, a structured executable file, a built-in default image held inside the
implementation itself, or an artifact selected by any means it offers, and the boot contract
in this chapter is identical in every case. Artifact selection, artifact format, and the
loader that parses one are implementation matters, not architecture: the architecture's
obligation begins with bytes already in memory and a machine in the state this chapter fixes.
A conformance binary is therefore an artifact like any other, and a machine that can load one
can be tested regardless of how it usually acquires the software it runs.

## Reaching user level

The machine has exactly one path from supervisor to user privilege, and it is the trap-return
instruction. Guest software builds a trap frame whose saved status word names user privilege
and whose saved program counter names the user entry point, points the trap-stack register
at it, and executes `trap_return`. The trap-model chapter owns the frame's layout and the
instruction's semantics; the point here is that reset privilege is supervisor and there is no
second mechanism for leaving it.

Because the descent to user level runs through the same instruction that returns from a trap,
a kernel writes the transition once and uses it for the first process and for every trap
return afterward.

## A minimal boot sequence

The fragment below is illustrative rather than normative, and it shows the shortest sequence
that turns a freshly reset machine into one that can service a trap. Symbolic names stand for
addresses the assembler resolves in the loaded image, and for the control and status register
numbers the privileged-architecture chapter assigns, which reach the assembler as defined
constants rather than as relocatable symbols; a literal in their place would carry its own
base marker. An immediate move names its width whether the operand is a literal or a
symbol, so the address moves below are `move.w`.

    move.w stack_top r30          ; a stack exists from here on
    move.w vector_table r1
    csr_write r1 trap_vector_base ; traps can now be serviced
    move.w trap_stack_top r1
    csr_write r1 trap_stack
    csr_read boot_info r2         ; r2 now holds the boot-information block address
    call kernel_main

Nothing before the first `csr_write` may trap, since no vector table is installed yet, and the
three instructions above it cannot. That ordering constraint is the whole of early boot
discipline on this machine.

## Conformance notes

The following properties are directly testable by a binary the machine loads, and a conforming
machine exhibits all of them.

- The first instruction executed is the one at physical address `$0000000000001000`.
- Reading each of the thirty-two general registers as the first architectural act of the
  program yields zero.
- The machine is in supervisor privilege, so a privileged instruction executed first raises no
  trap.
- Translation is off, so a load from a physical address the boot-information block reports as
  usable succeeds without any page table existing.
- The boot-information register holds an address at which a well-formed
  boot-information block is readable.
- Every byte of usable physical memory that the artifact did not fill reads as zero.
- Every present device reports its identification value, holds its interrupt-enable bit clear,
  and reports no pending status condition.
- No interrupt is delivered before the guest sets the interrupt-enable bit, even after a timer
  programmed by the guest has expired and set its status bit.
