# Appendix C: The Syscall Surface

This appendix is normative. It exists so that a reader arriving from the v1 specification's
syscall appendix finds the answer quickly, and the answer is that the machine contributes a
trap and a register convention while everything a syscall means is operating-system policy
that lives outside this specification.

Where this appendix restates a rule another chapter owns, that chapter governs. The operand
forms and the number space of `sys` belong to the control instruction reference, the frame and
the cause belong to the trap-model chapter, the argument and result registers belong to the
calling-convention chapter, and the provider-selection register belongs to the
privileged-architecture chapter.

## What carries over from v1

The `sys` instruction keeps its v1 shape. It is user-callable rather than privileged, because
it is the deliberate entry from user code into the kernel, it carries a syscall number, and it
raises the syscall cause the trap-model chapter names. Both operand forms exist: `sys #imm`
takes the number as an 8-bit literal in the instruction, and `sys rs` takes it from the low
byte of a register, which is what a program does when the number is computed. The number space
is `$00` through `$FF` in both forms, exactly as in v1.

Arguments and results travel in registers across the trap boundary in both directions. The
argument registers are the ones the calling-convention chapter names for an ordinary call, the
result lands where that chapter puts a call's result, and no register is clobbered by the
trap entry itself, because the trap model pushes a four-word frame and saves no general
register. A quesOS syscall trampoline written for v1 therefore ports as a mechanical rewrite
rather than a redesign.

## What the machine contributes, and what it does not

The machine's entire contribution to a system call is the trap mechanism plus the argument
convention. The trap-model chapter fixes the frame, the cause, the vector, and the
`trap_return` that resumes the interrupted context. The calling-convention chapter fixes which
registers carry the arguments and the result, and which of them survive a call.

Nothing else about a system call is architecture. Syscall numbers are operating-system policy,
the meaning of each number is operating-system policy, the error convention is
operating-system policy, and the set of syscalls a program may call is operating-system
policy. A conforming Maize machine runs an operating system that numbers its calls any way it
likes, and no conformance test asserts that any particular number does any particular thing.

This is the same boundary the v1 appendix drew, stated more sharply. The v1 appendix listed
the numbers the reference implementation happened to serve, and readers took the list for a
machine contract. The list belongs to whichever operating system a machine is running, and its
documentation is where that list lives.

## Provider selection

A machine may offer more than one syscall provider, for instance a native provider and a
compatibility provider that follows another system's numbering. Selecting between them is a
mode bit in the control and status register space rather than a pair of instructions. The v1
opcodes that set and cleared the selection do not exist in v2, because a mode toggle does not
earn opcode space when a register bit expresses it, and because the selection is set once at
startup rather than in a hot path.

The privileged-architecture chapter numbers the register and states the bit's position, and
the bit is clear at reset. Which providers a machine offers, and what the bit's values select
among them, is a property of that machine and its operating system rather than of the
architecture.

## Block memory has no provider surface

The v1 syscall surface offered bulk memory copy and bulk memory set as provider calls. Neither
exists as a syscall in v2, because `block_copy`, `block_copy_forward`, and `block_set` are base
instructions that any program executes directly at any privilege level. A v1 reader looking for
those syscall numbers finds their replacement in the instruction inventory's block-memory
family, and a compiler that emitted the syscalls emits the instructions instead.

The change is worth stating explicitly, because it is the one place where a v1 syscall did not
become a v2 syscall with a different number. Bulk memory movement became an instruction, and no
operating system mediates it.
