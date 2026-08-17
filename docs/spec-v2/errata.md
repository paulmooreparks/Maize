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
publications continuously across the divider and a citation has to keep meaning what it meant.

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

### `2.0.3`, 2026-08-14. The conformance line requiring a clear pending bit at the handler's first instruction cannot hold for a device-owned cause

Chapter and section: `trap-model.md`, "Delivery" and "Conformance notes".

Before, in "Conformance notes":

> - With two interrupt causes pending and enabled, the lower-numbered one is delivered first, and
>   its pending bit is clear at the handler's first instruction while the other's remains set.

Now, in "Conformance notes":

> - With two interrupt causes that no device owns pending and enabled, the lower-numbered one is
>   delivered first, its pending bit is clear at the handler's first instruction, and the other
>   cause's pending bit remains set. A device-owned cause's pending bit is also clear the instant
>   delivery runs, but if the device is still asserting the condition it reports, its latch has
>   already set the bit again by the handler's first instruction. That reassertion is the
>   acknowledgement paragraph's own rule in effect, not a delivery defect, so a binary that
>   expects a device-owned cause's bit to read clear at that point is not exercising this
>   guarantee.

Requirement: an implementation clears a cause's pending bit as part of delivery, for every
cause including one a device owns, and delivery does nothing further about that bit. Whether
the bit still reads clear at the handler's first instruction depends on whether the interrupt's
source is a device that has re-latched its condition in the meantime, which the acknowledgement
paragraph already governs; delivery's own guarantee is directly observable only for a cause no
device owns, because nothing else touches that cause's bit between delivery and the handler's
first instruction.

Provenance: the "Delivery" section already required the machine to clear a cause's pending bit
on delivery, and the acknowledgement paragraph a few lines below it already required a
device that latches a condition of its own to re-raise the cause until the handler clears the
device's own latch. The conformance line stated a third thing, that the bit reads clear at the
handler's first instruction regardless of cause, which the other two passages together make
false for any cause a device owns: delivery clears the bit, the device's latch is unchanged
because the handler has not run, and the device reasserts before the handler's first
instruction. This restates what the delivery and acknowledgement passages already compelled
between them; it supplies nothing an operator ruling settled, and no conformance test's
expected result moves. The fixture this bound was checked against,
`lowest_numbered_deliverable_cause_wins` in `tests/v2/fixtures_interrupts.cpp`, already used
causes 40 and 45, which belong to no device class this machine carries, so its expected result
already exercised only the device-free case the corrected line now states. maize-475 found the
contradiction during implementation of the trap model; maize-466, which implements the pending
and enable state and the delivery sequence this entry corrects the description of, needs no
change. A related question, whether an erratum level counts a correction or an issuance, is
open on card maize-483 and this entry does not depend on its answer.

### `2.0.4`, 2026-08-14. The versioning chapter, the header note, and this log's own front matter said the erratum level counts corrections; it counts publications

Chapter and section: `versioning.md`, "Errata" (four passages) and "What the numbers promise"
(one passage); `README.md`, "Status"; this file's own front matter, above "What an entry
records".

Before, in `versioning.md`, "Errata":

> Correcting such a passage is an erratum, and an erratum carries a third version component:
> `2.0.1`, `2.0.2`, and so on. That component counts corrections monotonically for the life of
> this text. It never restarts, and a number once issued is never reused or withdrawn, so a
> citation to erratum `2.0.2` names the same correction for as long as this architecture
> exists.

Now:

> Correcting such a passage is an erratum, and an erratum carries a third version component:
> `2.0.1`, `2.0.2`, and so on. That component counts publications monotonically for the life of
> this text: one number for every batch of corrections issued together, whether the batch holds
> one correction or several. It never restarts, and a number once issued is never reused or
> withdrawn, so a citation to erratum `2.0.2` names the same publication for as long as this
> architecture exists.

Before, further down the same section:

> At the release the suffix drops and nothing else moves. The level the header carried as
> `2.0.3-dev` becomes `2.0.3`, the next correction is `2.0.4`, and the log keeps the entries it
> had. The counter does not restart at the release, so the number of a level is the number of
> corrections that have issued since this text existed, before the freeze and after it alike.

Now:

> At the release the suffix drops and nothing else moves. Whatever level the header carries at
> that moment keeps its number and loses its suffix, the erratum published after it takes the
> next number whether it corrects one passage or several, and the log keeps the entries it had.
> No number is skipped at the freeze and none is reissued. The counter does not restart at the
> release, so the number of a level is the number of publications that have issued since this
> text existed, before the freeze and after it alike, not the number of corrections those
> publications carried.

The corrected passage also drops the two level numbers the old one named. That paragraph taught
a rule through whichever levels were current when it was written, and issuing this erratum
falsified both: it advanced the header past the level the paragraph said the header carried,
and it is itself the erratum the paragraph called the next one. The same section requires that
no chapter quote the current level, for the reason this paragraph went on to demonstrate, so
the corrected text states the transition without naming a level at all.

Before, in the same section, in the paragraph describing what the header tells a reader:

> That line states the level this text carries, suffix included, so a reader learns from the
> header both which correction the text has reached and whether the base is still a release
> candidate; the release-candidate status a later chapter's rule keys on is carried there and
> nowhere else.

Now:

> That line states the level this text carries, suffix included, so a reader learns from the
> header both which publication the text has reached and whether the base is still a release
> candidate; the release-candidate status a later chapter's rule keys on is carried there and
> nowhere else.

Before, in the worked example further down the same section:

> Both destinations exist because a bare version component is useless alone. A level of
> `2.0.3` tells a reader that three corrections have issued and nothing about whether any of
> them touched the chapter in front of them. The header answers which text this is, and the
> log answers what changed.

Now:

> Both destinations exist because a bare version component is useless alone. A level of
> `2.0.3` tells a reader that three publications have issued, not how many corrections they
> held between them, and nothing about whether any of them touched the chapter in front of
> them. The header answers which text this is, and the log answers what changed; a reader who
> wants the correction count finds it by reading the log's entries, not by reading the level.

Before, in `versioning.md`, "What the numbers promise":

> - An erratum component on the base names a text correction, and names a behavior change only
>   where the corrected passage was silent and the errata section's bound permits the change.

Now:

> - An erratum component on the base names a publication of text corrections, and names a
>   behavior change only where a corrected passage was silent and the errata section's bound
>   permits the change.

Before, in `README.md`, "Status":

> That line is this specification's header, and the versioning chapter refers to it by that
> name. Its first two components are the base version, which is what a conformance claim names
> and what a machine reports; the third is the erratum level, which counts corrections to this
> text and which no claim and no machine ever names.

Now:

> That line is this specification's header, and the versioning chapter refers to it by that
> name. Its first two components are the base version, which is what a conformance claim names
> and what a machine reports; the third is the erratum level, which counts publications of
> corrections to this text, one number for every batch issued together, and which no claim and
> no machine ever names.

Before, in this file's own front matter:

> The log is not emptied at the release and no entry is renumbered, because the erratum level
> counts corrections continuously across the divider and a citation has to keep meaning what
> it meant.

Now:

> The log is not emptied at the release and no entry is renumbered, because the erratum level
> counts publications continuously across the divider and a citation has to keep meaning what
> it meant.

Requirement: a reader learns how many corrections have issued against this text by counting
entries in this log, not by reading the erratum level. The level names a publication, meaning
a batch of corrections issued together, and it is unchanged by how many corrections the batch
held; three of the entries above share level `2.0.1` for exactly this reason. The header
carrying that level therefore tells a reader which publication the text has reached and no
more, so no passage in this specification describes the level, or the header that carries it,
as naming or counting an individual correction. Nothing here obliges an implementation to
distinguish these two counts, since the erratum level is reported by no control and status
register and is not named in a conformance claim; the correction is for a reader of the
specification, not for a machine.

Provenance: this restates what the log has done since its first entries rather than supplying
an answer where the text was silent. Three separate corrections issued on 2026-08-12 all carry
level `2.0.1`, one issued on 2026-08-13 carries `2.0.2`, and one issued on 2026-08-14 carries
`2.0.3`, so the level has counted publications rather than corrections from the start, and the
corrected text only now says so. Operator ruling, recorded on card maize-483: "It counts
publications." No conformance test's expected result moves and no implementation changes,
because the erratum level is documentation for a reader and is reported by no register and
named in no conformance claim.

### `2.0.5`, 2026-08-17. The branch-target relocation deferred its numbering and name to a specification that did not exist

Chapter and section: `assembler.md`, "Branch targets, pc_add, and relocations".

Before:

> that the linker resolves, and the relocation's numbering and name are deferred to a future
> object-format and linking specification, separate from this document set, which leaves the
> assembler's own behavior fully defined because the bytes it emits are the ones the
> instruction inventory fixes.

Now:

> that the linker resolves. The relocation's numbering and name are fixed by the Maize v2 object
> format and linking specification, at `docs/spec-v2-toolchain/object-format.md`, which stands
> outside this specification and carries a version line of its own. That document names this
> relocation `R_MAIZE_PCREL32` and numbers it 3, and the assembler contributes an addend of -4.
> The relocation measures from the first byte of the patched field, the machine measures the
> displacement from the following instruction, and because that field is the last four bytes of
> the instruction, the addend of -4 is what reconciles the two. Nothing in this chapter depends on
> either value, and the assembler's own behavior is fully defined without them because the bytes
> it emits are the ones the instruction inventory fixes.

Requirement: an assembler emits, for a branch, `jump`, `call`, or `pc_add` target that is
`extern` or lies in another section, a placeholder of zero and a relocation of type 3,
`R_MAIZE_PCREL32`, carrying an addend of -4 plus the constant part of the relocatable expression.
The bytes the assembler emits into the instruction stream are unchanged, and the instruction
inventory continues to fix them.

Provenance: this restates a requirement the text already bound rather than filling a silence. The
passage already required a 32-bit program-counter-relative relocation and already stated that its
numbering and name lived outside this specification; the correction replaces a pointer at a
document that did not exist with a pointer at one that does, and it names the values that
document assigns. Operator ruling that the format is an ELF subset, recorded on card maize-417,
and the numbering recorded as decision D-4 on the same card. No conformance test's expected
result moves and no conforming machine behaves differently, because no machine can observe an
object format: a machine sees bytes in memory and the bytes this passage governs are unchanged.

### `2.0.5`, 2026-08-17. The `move.w` relocation deferred its numbering under the same missing specification

Chapter and section: `assembler.md`, "Branch targets, pc_add, and relocations".

Before:

> a 64-bit absolute relocation the linker resolves, under the same deferred numbering as the
> program-counter-relative form.

Now:

> a 64-bit absolute relocation the linker resolves, numbered and named by the same object format
> and linking specification, where it is `R_MAIZE_ABS64`, type number 1, and takes an addend equal
> to the constant part of the relocatable expression, and therefore zero for a bare symbol.

Requirement: an assembler emits, for a `move.w` immediate that names a relocatable expression, an
eight-byte placeholder and a relocation of type 1, `R_MAIZE_ABS64`, carrying an addend equal to
the constant part of the relocatable expression and therefore zero for a bare symbol. The
eight-byte placeholder and the instruction's ten-byte length are unchanged.

Provenance: this restates a requirement the text already bound rather than filling a silence. The
passage already required a 64-bit absolute relocation and already placed its numbering outside
this specification. The relocation numbering is decision D-4 on card maize-417. No conformance
test's expected result moves and no conforming machine behaves differently, for the same reason
the preceding entry gives.

### `2.0.5`, 2026-08-17. The unwind shape for a frame-pointer-less function deferred to a specification that did not exist

Chapter and section: `abi.md`, "The frame pointer and unwinding".

Before:

> A function that omits the
> frame pointer is unwound from the metadata its object file carries, and the shape of that
> metadata is deferred to a future object-format and linking specification, separate from this
> document set, which leaves nothing here undefined because the unwind metadata is a note about
> tooling and every rule this chapter states holds without it.

Now:

> A function that omits the
> frame pointer is unwound from the metadata its object file carries, and the shape of that
> metadata is fixed by the Maize v2 object format and linking specification, at
> `docs/spec-v2-toolchain/object-format.md`, which stands outside this specification and carries a
> version line of its own. That document places the metadata in an `.eh_frame` section and fixes
> what DWARF call-frame information leaves to a processor supplement, including the DWARF register
> number of every register and the return-address column. Nothing here is left undefined by that
> division, because the metadata is a note about tooling and every rule this chapter states holds
> without it.

Requirement: a producer that emits a function without a frame pointer emits DWARF call-frame
information for it in an `.eh_frame` section, under the register numbering, alignment factors,
return-address column, and call-frame instruction subset that the named document fixes. Every
rule this chapter states about register roles, frame layout, and the frame-pointer chain is
unchanged, and a function that does use a frame pointer is still unwound by walking that chain.

Provenance: this restates a requirement the text already bound rather than filling a silence. The
passage already required that such a function be unwound from metadata its object file carries
and already placed the shape of that metadata outside this specification. The format is decision
D-6 on card maize-417. No conformance test's expected result moves and no conforming machine
behaves differently, because call-frame information is read by a debugger or an exception runtime
and is never read by the machine.

### `2.0.5`, 2026-08-17. The chapter index had no way to mention a normative document outside the specification

Chapter and section: `SUMMARY.md`, "Related documents", a section this entry adds.

Before: the file ended with the appendix table and named no document outside this specification.

Now:

> ## Related documents
>
> One normative document sits outside this specification and outside the tables above. The object
> format and linking specification, at
> [`../spec-v2-toolchain/object-format.md`](../spec-v2-toolchain/object-format.md), fixes the ELF
> subset the Maize v2 toolchain emits and consumes: the relocatable object and the linked
> executable, the section and symbol model, the relocation set, the archive, and the call-frame
> information a debugger reads. It is not a chapter of the Maize v2 instruction set architecture,
> and it is versioned on a line of its own, because an object format is a contract between tools
> rather than a property of the machine, and no conformance binary can observe it. A Maize v2
> conformance claim names the ISA base version and never names that document's version. The tables
> above therefore still list every file of the ISA specification, and this document is pointed at
> from outside them rather than added to one.

Requirement: a reader of the chapter index can reach the object format and linking specification
and can tell that it is not part of this specification and not named by a conformance claim. The
index's tables continue to list every file of the Maize v2 instruction set architecture and no
other file, so a tool or a reader that treats those tables as the definitive file list stays
correct.

Provenance: this supplies a pointer where the index was silent, and the silence constrained
nothing, so the addition narrows nothing and moves no answer this specification had already
given. The placement outside the tables is decision D-2 on card maize-417. No conformance test's
expected result moves and no implementation changes, because the chapter index is navigation for
a reader and is named in no conformance claim.
