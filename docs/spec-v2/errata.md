# Errata

This file records every correction issued against the ratified text of Maize base `2.0`. It
is the companion to the erratum component of the specification's header: the header says
which text a reader holds, and this log says what changed to get there.

Every entry below was issued during the release-candidate period the versioning chapter
describes, against a text nobody outside this project had yet built a second implementation
of. At the release of Maize base `2.0`, the corrections these entries record fold into the
base text and this log gains a divider at that point. Every entry above the divider has
already been applied to the text a reader holds, so there is nothing for that reader to do
about it, and every entry below it corrects a text someone else had already read. The log is
not emptied at the release and no entry is renumbered, because the erratum level counts
corrections continuously across the divider and a citation has to keep meaning what it meant.

An erratum corrects a passage that is ambiguous, self-contradictory, wrong about what the
reference implementation and the conformance suite already agree on, or silent where this
specification promises no silence, and it changes no conformance test's expected result. The
versioning chapter states the rule and its bound, including the one case the bound admits: a
passage that was silent constrained nothing, so a correction filling the silence narrows the
set of conforming machines rather than moving an answer this specification had already given.
Outside that case, a change that would alter what a conforming machine does is not an erratum,
whatever its size.

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
recorded on card maize-431, and it does not restate anything the ratified text already compelled.
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

Provenance: this requirement restates what the ratified text already compelled. The restartability
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
maize-431. It does not restate anything the ratified text already compelled, because neither
reading contradicted anything the ratified text said. No conformance test's expected result moves,
since no conformance binary could assert on this field while the answer did not exist.

### `2.0.2`, 2026-08-13. The class contract version has no numbering scheme

Chapter and section: `device-surface.md`, "The common class skeleton" and "Conformance notes".

Before, in "The common class skeleton":

> Offsets 3 through 15 are class-specific, and the sections below give them. An offset a class
> does not define is reserved within a populated block.
>
> Writing a reserved bit of a defined port is discarded rather than trapped, and reading a
> reserved bit yields zero. Discarding rather than trapping is the deliberate choice for
> device-register bits, because a device is not the instruction stream: a driver that writes a
> bit the machine does not implement then degrades to the behavior of a machine without that
> bit, instead of faulting.
>
> ## Reset state

No text anywhere in the chapter said what values the class contract version named at offset 0
of the common class skeleton takes, how it increments, what a guest does with a version it
does not recognize, or whether the number is per-class or machine-wide.

Now, in "The common class skeleton", the same passage with a new subsection inserted between
the two paragraphs quoted above and the "Reset state" heading:

> ### The class contract version
>
> The class contract version at offset 0 is a single 16-bit counter, assigned separately for
> each class rather than shared across them. It starts at 1 for a class's first ratified
> contract and increases by one for each additive change made to that class's contract
> afterward. A number once issued for a class is never reused or withdrawn.
>
> An additive change is the only kind of change that increments the counter. A change is
> additive when it defines something the class's contract left undefined, such as an offset the
> class reserved or a status bit it left unnamed, and leaves everything the contract already
> defined meaning what it meant before. A change to a class's contract that is not additive,
> such as one that would alter the meaning of an existing port or remove one, does not
> increment the counter at all: it retires the class code and assigns a new one, and the new
> class's counter starts again at 1. This mirrors the rule the versioning chapter fixes for an
> incompatible extension, where the successor takes a new name and a fresh `1.0` rather than a
> new major number on the old name.
>
> A guest that reads a contract version higher than the one it was written against proceeds
> rather than refuses. A higher number can only mean added capability behind ports the guest
> already understands, since an incompatible change never reaches this field at all: it
> arrives, if it ever does, as a class code the guest does not recognize, and an unrecognized
> class code is absent as far as that guest is concerned. There is no case in which this field
> entitles a guest to reject a class it does recognize.
>
> The increment rule also answers a contract version lower than the one a guest was written
> against. A lower number means the ports a later version of the contract added are not
> present, while every port the lower version does define means what it has always meant. A
> guest that uses only what the lower version defines runs unchanged, and a guest that needs a
> port added later reads the version before it uses that port.
>
> The version is advisory to software. The machine populates the field and behaves identically
> whether the guest inspects it, acts on it, or ignores it entirely.
>
> Every class this specification defines holds its counter at 1 permanently. Base 2.0 does not
> revise, and no extension can reach a device class either. What an extension carries is fixed
> at ratification, and the extensions chapter's list of those items includes no port
> allocation. An extension that assigned or altered a class anyway would be reaching into the
> range this chapter reserves for the classes later specification work assigns, or into the
> offsets a class leaves reserved, and this chapter fixes both as reading zero on a base
> machine, so they would read zero without the extension and read otherwise with it. That is
> base behavior made conditional on whether an extension is present, which the extensions
> chapter forbids outright. The implementation range above `$7FFF` is different in kind and
> leaves this argument intact, since this chapter fixes nothing about what a machine puts
> there and no portable program reads it, so a machine that populates it makes no base
> behavior conditional on anything.
> Nothing in this architecture as specified can therefore increment the field for any of the
> seven classes above. The counter is provision for a class that later specification work might
> assign; where such work would be recorded and by what process it would happen is not fixed by
> this chapter.

Before, in "Conformance notes":

> - The identification port of every present class reads its class code in the low quarter-word,
>   and the identification port of every absent class reads zero.

Now, in "Conformance notes":

> - The identification port of every present class reads its class code in the low quarter-word
>   and its class contract version, which is 1 for every class this specification defines, in
>   the second quarter-word, and the identification port of every absent class reads zero.

Requirement: an implementation gives every device class it carries a contract version of 1, in
the second quarter-word of the identification word that class's port at offset 0 reads, since no
class this specification defines is permitted to hold any other value; and a guest that reads a
contract version higher than the one it expects for a class it recognizes proceeds rather than
refuses.

Provenance: this requirement was supplied by an operator ruling on a silence, recorded on card
maize-452, D-1. It does not restate anything the ratified text already compelled, because the
ratified text named the field and said nothing about what values it could carry. No conformance
test's expected result moves: no conformance binary could assert a value for this field while
the specification defined none, so filling the silence changes no test that existed before it.
The property the conformance notes gain is new for the same reason, and it narrows what
conforms, since a machine that chose some other value was conforming under the earlier text and
is not conforming under this one. An operator ruling on the erratum bound, recorded on card
maize-452, D-6, permits that narrowing where the earlier text constrained the behavior not at
all, and accepts its cost here because no such machine exists and because the alternative is a
named field left undefined for the life of the architecture.

The same ruling changes the class contract version constant in `src/v2/device_v2.h` from `$0100`
to `$0001` and renames it from `kClassContractVersionPlaceholder` to
`kClassContractVersionInitial`, since the old name recorded that the value was an unchosen
placeholder rather than an answer, which is also why no fixture asserted it. A fixture asserts
it now.
