# Extensions and Extension Governance

This chapter is normative. It fixes how Maize v2 grows after the base is frozen: what an
extension is, where its opcodes and its architectural state live, how a program discovers
which extensions the machine in front of it implements, what an extension is permitted to
do and forbidden to do, and what has to happen before an extension is part of the
architecture at all.

The governing rule is short. The base freezes once, forever. Every capability added to
Maize v2 after that freeze arrives as a named, independently versioned, optional extension
occupying its own opcode page and its own control-and-status-register range. There is no
mechanism in this architecture for revising the base, and this chapter does not describe
one, because the absence of that mechanism is the guarantee the rest of the design rests
on.

## What the freeze means

Freezing the base means that the encoding, the register model, the instruction inventory,
the trap model, the memory model, the privileged architecture, and the calling convention
described by the rest of this specification stop changing at ratification and stay stopped.
A binary assembled against the frozen base runs on every conforming Maize v2 machine that
will ever exist, whatever extensions that machine also implements, and it observes the same
architectural state at every step.

The freeze runs in the other direction too. A machine that implements the base and nothing
else is a complete Maize v2 machine, not a subset or a profile or a reduced configuration.
The conformance chapter states the same thing from the testing side: the base-only suite is
a complete certifiable claim.

Maize v1 needed a thaw ceremony because its growth path ran through the base. Extensions
exist so that no future capability, however desirable, ever creates a reason to reopen the
frozen text.

## What an extension is

An extension is a named, versioned, optional unit of architecture. Each one is ratified as
a whole and implemented as a whole; there is no partial implementation of an extension, and
a machine that implements some of an extension's opcodes and not others is not conforming.
Every extension carries the following, all of them fixed at ratification.

- A name, written as a short lowercase identifier in the mnemonic alphabet, for example
  `cap` or `vec`.
- A version, independent of the base version and of every other extension's version.
- Exactly one allocated opcode page, reached through one of the seven escape bytes the
  encoding chapter reserves.
- A contiguous allocated range within the control-and-status-register space, which may be
  empty when the extension adds no architectural state.
- A specification chapter of its own, written to the same normative standard as this text,
  in which every behavior is pinnable by a conformance binary.
- A conformance-suite section of its own, which a machine claiming the extension runs and
  passes.

The name `base` refers to the frozen base architecture. It is not an extension, it cannot
be omitted, and it takes no escape byte.

### The anticipated first extensions

Four extensions are anticipated and none of them is specified here. Naming them in advance
fixes their names and nothing else, so that the registry does not later hand the name `vec`
to something that is not a vector extension.

- `cap`, capability-shaped addressing with bounds and permissions carried in the pointer
  and unforgeable tags on memory.
- `vec`, length-agnostic vector operations.
- `meter`, execution metering for cycle budgets, scheduling, and accounting.
- `atomic`, atomic read-modify-write and the memory ordering primitives that go with them.

Each of these becomes real when it is ratified, at which point it receives its escape byte,
its register range, its chapter, and its first version number. Until then a conforming
machine implements none of them, and a binary that uses one of them traps on every machine.

## Opcode space

An extension's instructions live on one 256-entry opcode page, reached through one escape
byte. The encoding chapter fixes the mechanism, and this chapter fixes the policy around
it: the primary page reserves seven escape bytes, `$F8` through `$FE`, an extension is
allocated exactly one of them at ratification, and no two extensions share a page.

Every regularity invariant in the encoding chapter binds an extension page exactly as it
binds the primary page. Length is a pure function of the escape byte plus the page opcode
byte, no instruction reads state to determine its own length, operands occupy whole bytes
one operand to a byte, immediates are whole and little-endian and follow every operand
byte, and unassigned entries on the page raise the illegal-instruction trap. An extension
page may define length classes the base does not use, and may define new form-field values
for its own operand slots, subject to the sixteen-byte maximum instruction length the
encoding chapter fixes for the base and for all extensions.

Seven pages is the ceiling, and it is a deliberate one. When the seventh page is allocated
the architecture is out of opcode space, and the correct response at that point is a
different architecture rather than a second escape level bolted onto this one.

## Extension state

Architectural state an extension adds lives in that extension's own range of the
control-and-status-register space. The privileged-architecture chapter owns the numbering
of that space, including the access-rule bits carried in each register number; this chapter
fixes only the allocation rule, which is that an extension writes to no register outside its
allocated range and reads no register outside its allocated range for its own purposes.

Two consequences follow, and both are testable. An extension cannot change the reset value,
the access rules, or the meaning of any base register, so base software behaves identically
whether or not the extension is present. An extension also cannot collide with another
extension's state, so any two ratified extensions are implementable together without
negotiation between their authors.

An extension that adds no state receives no range. Nothing in the architecture requires an
extension to add state in order to add instructions.

## Discovery

Software discovers which extensions a machine implements through two mechanisms, and both
are present on every conforming machine including a base-only one.

The feature bitmap is a read-only control and status register in which each allocated
escape page has one bit, set when the machine implements the extension owning that page and
clear when it does not. A program reads it with `csr_read` and tests a bit, which is a
two-instruction check cheap enough to sit on a hot path. The privileged-architecture chapter
fixes the register's number and its behavior at every privilege level; the bit index of each
extension is allocated by the extension registry at ratification, alongside the extension's
escape page and its control-and-status-register block, so the registry entry is the single
place a bit assignment lives.
On a base-only machine the bitmap reads as all zeroes in the allocated bits, and the
register itself is still implemented, so the check costs the same everywhere.

The extension list in the boot-information block carries the full detail the bitmap cannot:
for each implemented extension, its name and its version. The memory-model chapter owns the
block's placement and layout, and the register that reports the block's address. Firmware
and operating-system startup code read the list once, and everything after that reads the
bitmap.

The division of labor between them is deliberate. A bitmap bit answers "is `vec` here", and
that is the question a fast path asks. A version answers "which `vec`", and that is a
question asked once at startup, where the cost of walking a structure in memory does not
matter.

Nothing in the architecture requires a program to probe for an extension by executing one
of its instructions and catching the trap. That technique works, because the trap is
defined, but the discovery mechanisms above make it unnecessary, and a program that relies
on it in place of the bitmap gives up nothing except clarity.

## Unknown opcodes trap

An opcode this specification does not assign raises the illegal-instruction trap. An escape
byte whose extension the machine does not implement raises the illegal-instruction trap on
the escape byte itself, and the machine does not fetch, examine, or skip the byte after it.
An unassigned entry on an implemented extension page raises the illegal-instruction trap.
Nothing in reserved or unimplemented space executes as a no-operation, returns a defaulted
result, or is silently tolerated in any other way.

This is the property that makes an extension's absence observable rather than inferred. A
conformance binary can execute an instruction from an unimplemented page and assert that
the trap arrived, that its cause is illegal-instruction, that the reported faulting address
is the address of the escape byte, and that the reported offending byte is the escape byte.
A machine that quietly ignored the instruction instead would be indistinguishable, from
inside the guest, from a machine that executed it and produced a zero, and the difference
between those two machines is exactly what a conformance suite exists to detect.

The same property is what lets a program fall back safely. Code that uses `vec` where it is
present and scalar code where it is not is code whose two paths are both reachable and both
testable, because the machine tells the truth about which one it can run.

## What an extension may do

An extension may do all of the following, and a ratified extension that stays inside this
list cannot break any conforming base binary.

- Assign instructions to entries on its own opcode page.
- Define length classes on its own page beyond those the base uses.
- Define form-field values for the operand slots of its own instructions.
- Implement control and status registers within its allocated range, with any access rules
  the register-number encoding can express.
- Define new trap causes drawn from the cause space the trap-model chapter reserves for
  extensions.
- Add architectural state reachable only through its own instructions and its own
  registers.
- Depend on another named extension, provided the dependency is declared at ratification
  and the depended-on extension is ratified first.

## What an extension may not do

An extension may not do any of the following, and a proposal that requires one of them is
rejected rather than accommodated.

- Change the semantics, the operand forms, the length, or the traps of any base
  instruction.
- Assign, reassign, or reinterpret any opcode byte on the primary page, including the bytes
  the opcode map lists as reserved.
- Alter the frozen encoding rules, including component order, the derivation of length from
  the leading bytes, one operand per byte, whole little-endian immediates, and the
  sixteen-byte maximum instruction length.
- Change the register model, including the count of registers, the hardwired-zero behavior
  of r0, the width of the register-number field, or the set of positional slices.
- Change the trap frame's shape, the meaning of any base trap cause, or the restartability
  contract.
- Read or write any control and status register outside its allocated range as part of its
  own defined behavior.
- Change the meaning of any base control and status register, including its reset value and
  its access rules.
- Make any base behavior conditional on whether the extension is present or enabled.
- Introduce a mode bit that changes how base instructions behave.

The last two carry the most weight, because they are the failure mode that destroys the
value of everything above. An extension that adds a mode in which `add` means something
else has not extended the architecture; it has forked it, and the base binary that runs
correctly on one conforming machine and incorrectly on another is the precise outcome the
freeze exists to prevent.

## The registry

The extension registry is the document that records, for every ratified extension, its
name, its escape byte, its control-and-status-register range, its feature-bitmap bit, its
current version, and its declared dependencies. It is part of the architecture, and it is
the sole authority on which allocations are taken.

Allocation is part of ratification and never precedes it. An extension under design holds
no escape byte, no register range, and no bitmap bit, and its draft chapter names none of
those. The reason is arithmetic: the architecture has seven pages, several proposed
extensions never ship, and a page held by a proposal that dies is a page lost forever.

Ratifying an extension therefore requires all of the following to land together.

1. The extension's specification chapter is complete and written to this document's
   normative standard, with every stated behavior pinnable by a conformance binary and
   every input, valid or invalid, given a defined outcome.
2. The extension's conformance-suite section exists and passes against a reference
   implementation.
3. The registry records the name, the escape byte, the register range, the bitmap bit, the
   initial version, and any declared dependencies.
4. The allocations do not collide with any existing allocation, and the extension's chapter
   contains nothing on the forbidden list above.

Deallocation does not exist. A ratified extension's escape byte, register range, and bitmap
bit belong to it permanently, even if the extension is later superseded by a newer version
of itself or falls out of use, because a binary assembled against it must keep trapping
predictably on machines that do not implement it rather than silently decoding as something
new.
