# Errata

This file records every correction issued against the ratified text of Maize base `2.0`. It
is the companion to the erratum component of the specification's header: the header says
which text a reader holds, and this log says what changed to get there.

An erratum corrects a passage that is ambiguous, self-contradictory, wrong about what the
reference implementation and the conformance suite already agree on, or silent where this
specification promises no silence, and it changes no conformance test's expected result. The
versioning chapter states the rule and its bound. A change that would alter what a conforming
machine does is not an erratum, whatever its size.

## What an entry records

Each entry carries the following. The before-and-after pair, the requirement, and the
provenance are what make an entry useful rather than merely traceable.

- The erratum level it introduced, and the date it issued.
- The chapter, and the section heading within it that contains the corrected passage.
- What the passage said before.
- What it says now.
- What the corrected text requires of an implementation, stated as a requirement rather than
  as a description of the edit.
- Whether that requirement restates something the text already bound, or supplies one that an
  operator ruling settled where the text was silent.

An entry cites a heading rather than a line number, because a correction moves the lines
around itself and a log that cited them would be wrong about its own subject.

The before-and-after pair is what lets a reader who last read an earlier level find out
whether anything they relied on moved, without diffing two documents. The requirement is what
lets an implementer decide whether they have work: an erratum states something that was
already binding but recorded badly, so a machine that guessed right needs no change and one
that guessed wrong was already non-conforming. An entry also says which of two cases it is,
since a correction can restate a requirement the text already bound or supply one that an
operator ruling has newly settled where the text was silent, and the version number carries
no trace of that difference. An entry that leaves a reader unable to answer any of those
questions has not done its job.

Entries appear in issue order, oldest first. No entry is removed or rewritten, and a
correction to a correction is a new entry.

## Entries

### `2.0.1`, 2026-08-12. The faulting data address of an interrupted `block_copy`

Chapter and section: `instruction-reference-memory.md`, "Block memory", "The restartability
contract".

Before:

> A page fault during a block-memory instruction reports the address of the block instruction
> itself as the faulting instruction address, not an address inside a host memory routine, and
> it reports the inaccessible guest address as the faulting data address.

Now:

> A page fault during a block-memory instruction reports the address of the block instruction
> itself as the faulting instruction address, not an address inside a host memory routine, and
> it reports the inaccessible guest address as the faulting data address. That address is the
> byte the resumed execution touches first, because the restart-invariant register state above
> already commits an implementation to retrying there, whichever direction it travels, so the
> reported address and the retry point are the same byte by construction. Two conforming
> implementations that choose opposite directions for the same `block_copy` therefore report
> different addresses for what is, from the program's view, the same fault, exactly as their
> pointer and count registers already differ afterward; neither report is wrong.

Requirement: an implementation reports, as the faulting data address of an interrupted
`block_copy`, the byte its resumed execution will touch first. That value is
direction-dependent, matching the direction-dependence the register state already carries.

Provenance: the passage said no more than the sentence quoted above, so which inaccessible
address is reported when a fault interrupts a transfer whose direction the implementation
chooses was left unstated. This requirement was supplied by an operator ruling on that silence,
recorded on card maize-431, and it does not restate anything the frozen text already compelled.
An earlier ruling on the same silence named the region's lowest inaccessible address instead;
it was checked against the running implementation, found to change a conformance result, and
withdrawn before it shipped.

### `2.0.1`, 2026-08-12. Block-memory completion state after a fault and resume

Chapter and section: `instruction-reference-memory.md`, "Block memory", "The restartability
contract".

Before:

> On normal completion the count register holds zero and each pointer register holds its
> original value plus the original count, so it points just past the last byte of its region.
> That final state is the same for every implementation and every direction of travel, and it
> leaves a repeated execution of the same instruction harmless, since a count of zero transfers
> nothing.

Now:

> On normal completion of a single, uninterrupted execution the count register holds zero and
> each pointer register holds its original value plus the original count, so it points just past
> the last byte of its region. That final state is the same for every implementation and every
> direction of travel, for such an execution, and it leaves a repeated execution of the same
> instruction harmless, since a count of zero transfers nothing. It does not describe a
> descending pass that faults and resumes. The descending-progress rule above leaves the pointer
> registers unmoved throughout, so a resumed execution begins from the pointers' original value
> and the count remaining at the fault, and completes with each pointer at that starting value
> plus only the count that remained, never the instruction's original count. An ascending pass
> that faults and resumes needs no separate rule, because its pointers advance to the failing
> address and its count holds what remains, so the resumed execution completes at the
> instruction's original pointer values plus its original count, exactly as an uninterrupted one
> does.

Requirement: an implementation whose descending `block_copy` faults and resumes completes with
each pointer at the value it held when the resumed execution began plus only the bytes remaining
at that point, never the instruction's original count.

Provenance: this requirement restates what the frozen text already compelled. The restartability
contract states that the three named registers describe exactly the work that remains, so no
other state survives the fault, and the descending-progress rule leaves the pointer registers
unmoved throughout a descending transfer. A resumed execution therefore holds no record of its
pre-fault count, and the sentence corrected here demanded a final state no implementation could
produce. This is the item on which the issuance's claim to be an erratum rests, since it is the
only one of the three that textual compulsion alone settles. The ascending clause the
correction adds imposes nothing further, since it states what the same two rules already give
for the other direction of travel.

### `2.0.1`, 2026-08-12. The auxiliary word for an invalid bitfield immediate pair

Chapter and section: `trap-model.md`, "The cause enumeration".

Before, in the auxiliary-word cell for cause 1:

> The offending byte or value, zero-extended. Under subcode 6 the offending value is the value
> the write supplied, not the register number

Now:

> The offending byte or value, zero-extended. Under subcode 6 the offending value is the value
> the write supplied, not the register number. Under subcode 1, when a bitfield instruction's
> immediate packs an invalid width together with an invalid position, the offending value is the
> width field's raw value when the width is zero, and the position field's raw value otherwise

Requirement: an implementation reports, in the auxiliary word for cause 1 subcode 1, the width
field's raw value when a bitfield instruction's invalid immediate carries an invalid width of
zero, and the position field's raw value otherwise.

Provenance: this requirement was supplied by an operator ruling on a silence, recorded on card
maize-431. It does not restate anything the frozen text already compelled, because neither
reading contradicted anything the frozen text said. No conformance test's expected result moves,
since no conformance binary could assert on this field while the answer did not exist.
