# Versioning

This chapter is normative. It states what a Maize v2 version number is, who assigns it, what
it promises to a program and to an implementer, and what it deliberately does not promise.
Two independent version spaces exist, one for the base and one for each extension, and this
chapter defines both.

Version numbers in this architecture carry less weight than they do in most, and that is by
design. The base has exactly one version for its whole lifetime, so a base version number is
an identity rather than a moving target. The interesting versioning happens per extension,
where the numbers are small, monotonic, and discoverable at runtime.

## The base version

The base version of this specification is `2.0`. A machine implementing this text implements
Maize base `2.0`, and there is no base version after it; `2.0` names the one architecture
this text will ever describe, whether the copy a reader holds is a release candidate or the
frozen text that follows one.

Maize v1 remains a separate architecture with its own version line. The `2` is a lineage
marker, not a compatibility statement, and no v1 binary runs on a v2 machine.

### Release candidate, and the freeze that follows it

The thirteen decisions and the terminology ruling this text implements were ratified on
2026-08-12. Ratification governs how the text changes from that point on: a change goes
through a ratified decision and a reviewed change, and nobody edits the text to win an
argument with their own code. It does not by itself mean the text has stopped changing, and
this document set has not: it is a release candidate, and it stays one until Maize base
`2.0` ships.

A release candidate exists because a review pass does not find every defect a specification
can carry. DOOM, quesOS running on this architecture, and a second implementer building from
this text without the reference machine at hand are still ahead of it, and each is expected
to surface a passage review already read as fine and an implementation could not follow, the
way the first implementation of the trap model already has. Declaring the text frozen before
that work happens would freeze whatever it has not yet found.

The freeze happens once, at the release of Maize base `2.0`. At that release the corrections
this candidate accumulated fold into the base text, so a reader who receives the frozen text
already receives everything an erratum established along the way, and the errata log gains a
divider marking every entry above it as already applied. A second implementer working from
the frozen text therefore starts from a text that has already absorbed what this release
candidate found wrong with itself, needs to do nothing about the entries above the divider,
and reads below it only what a reader found wrong with the frozen text. The erratum level
keeps counting across the divider and loses its suffix there, which the errata section below
states in full.

### The base does not revise

Once frozen, base `2.0` does not revise. No change to a behavior this text already specifies
is possible within Maize v2, before the release or after it: there is no `2.1`, there is no
mechanism to produce one, and this specification defines no process by which the text could
be amended to alter a behavior it already gives. A passage this text leaves silent is a
different case, bounded by the errata section below, and that bound applies during the
release-candidate period exactly as it will afterward. Every capability added to Maize v2
arrives as a named extension under the rules the extensions chapter fixes.

The consequence for a program is direct. A binary that uses only base instructions and only
base architectural state behaves identically on every conforming Maize v2 machine that will
ever be built, and a machine on which it does not is defective rather than differently
versioned.

The consequence for an implementer is equally direct. An implementation of the frozen base
`2.0` is never obsoleted by a later base, and the work of building one is bounded by a
document that has stopped moving. The weekend-reimplementable budget the design set for
itself assumes a target that holds still, which is exactly what a release candidate does
not yet promise and the frozen text does.

### Errata

A ratified specification can still contain a passage that is ambiguous, self-contradictory,
wrong about what the reference implementation and the conformance suite already agree on, or
silent where this specification promises no silence.
Correcting such a passage is an erratum, and an erratum carries a third version component:
`2.0.1`, `2.0.2`, and so on. That component counts publications monotonically for the life of
this text: one number for every batch of corrections issued together, whether the batch holds
one correction or several. It never restarts, and a number once issued is never reused or
withdrawn, so a citation to erratum `2.0.2` names the same publication for as long as this
architecture exists.

The component also carries a suffix while the base is unreleased. A level issued against this
release candidate is written with `-dev`, as in `2.0.3-dev`; should the base ever run a beta
line before its release, a level issued on that line is written with `-beta` instead; and a
level on the released base carries no suffix at all. The suffix names the line the text is
on and says nothing about the correction itself, which is why it can change while the number
does not. Semantic versioning orders a prerelease suffix ahead of the bare version, so every
suffixed level precedes the bare level of the same number, and comparing two levels needs no
rule this specification has to supply.

At the release the suffix drops and nothing else moves. Whatever level the header carries at
that moment keeps its number and loses its suffix, the erratum published after it takes the
next number whether it corrects one passage or several, and the log keeps the entries it had.
No number is skipped at the freeze and none is reissued. The counter does not restart at the
release, so the number of a level is the number of publications that have issued since this
text existed, before the freeze and after it alike, not the number of corrections those
publications carried.

An erratum is bounded by one rule. It changes the text so that it says what the architecture
already required, and it changes no conformance test's expected result. If correcting a
passage would change what a conforming machine does, the passage is not an erratum
candidate; the behavior stands as the conformance suite pins it, and the desired change is
extension work or it does not happen.

That bound presupposes a behavior the earlier text constrained. A passage that is silent
constrains nothing, so every value a machine could report conformed under it, and a correction
filling the silence narrows the set of conforming machines instead of moving an answer this
specification had already given. An erratum may do that much, and it may oblige an
implementation to change what a guest observes, where the earlier text bore on the behavior
not at all. What must be shown is the silence itself. The absence of a conformance test does
not establish it, and a behavior this specification constrains but no binary happens to assert
stands as written. Erratum `2.0.2` is the first correction to reach this case, and its entry
in `errata.md` records both the silence it filled and the narrowing it cost.

The release-candidate period is when this exception matters most, because a silence is
cheapest to fill before anyone outside this project has built against it. Nothing in this
rule confines the exception to that period, and the same bound applies to an erratum issued
after the freeze, but an erratum issued then is filling a silence a third-party
implementation may already have chosen its own answer for, which is a cost this rule does not
waive and a future erratum in that position should weigh accordingly.

The erratum component is documentation for readers of this specification rather than
information for a program. It appears in two places and nowhere else.

The first is the specification's header, the bold line that opens the status section of
`README.md`. That line states the level this text carries, suffix included, so a reader learns
from the header both which publication the text has reached and whether the base is still a
release candidate; the release-candidate status a later chapter's rule keys on is carried
there and nowhere else. The header is the only place any document in this set states the
current level. No chapter quotes it, because a quotation is a copy that goes stale the moment
the level advances.

The second is the errata log, `errata.md`. Each entry there names the level that erratum
introduced, so the log holds the history of levels while the header holds the current one. An
entry names the number without the suffix, since the number is what a citation uses and the
suffix describes the state of the text rather than the correction; which side of the release
an entry sits on is what the log's divider records.

Both destinations exist because a bare version component is useless alone. A level of `2.0.3`
tells a reader that three publications have issued, not how many corrections they held between
them, and nothing about whether any of them touched the chapter in front of them. The header
answers which text this is, and the log answers what changed; a reader who wants the
correction count finds it by reading the log's entries, not by reading the level.

The component is reported by no control and status register, does not appear in the
boot-information block, and is not named in a conformance claim, which names the base version
alone. Nothing a program can observe distinguishes `2.0.0` from `2.0.7`.

Issuing an erratum therefore has three parts: the corrected passage, the header incremented,
and an entry appended to the log. A correction that changes the text without the other two is
incomplete, because a reader has no way to tell that the text moved under them.

## Extension versions

Each extension carries its own version, written `major.minor`, assigned in that extension's
registry entry and reported in the boot-information block's extension list. Extension
versions are independent of the base version and of every other extension's version, so a
machine might implement base `2.0` with `vec` at `1.3` and `meter` at `2.0` and no other
extension at all.

An extension's first ratified version is `1.0`. Versions increase monotonically within an
extension, and a number once issued is never reused or withdrawn.

### What a minor increment means

A minor increment adds capability to an extension and takes none away. Version `1.3` of an
extension implements everything version `1.2` implemented, with the same encodings, the same
semantics, the same register numbers, and the same traps, plus some number of newly assigned
entries on the same opcode page or newly implemented registers within the same allocated
range.

Two properties follow, and a conformance binary can check both. A binary written against
version `1.2` of an extension runs unchanged on a machine implementing version `1.3` of it.
A binary written against version `1.3` that uses an entry `1.2` did not assign traps with
illegal-instruction on a `1.2` machine, exactly as it would on a machine implementing no
version of the extension at all, because the unassigned entry raises the same trap either
way.

A minor increment is the only kind of change an extension makes to itself. Removing an
instruction, changing an instruction's semantics, renumbering a register, and changing a
trap cause are all outside what a minor increment may do.

### What a major increment means

A major increment is a different extension wearing a familiar name. When an extension's
design has to change incompatibly, the result takes a new name, a new feature-bitmap bit,
and fresh allocations for whatever escape byte and register range it needs, and it enters
the registry as a new extension while the old one keeps everything it was allocated.

The naming convention makes the relationship legible: the successor of `vec` version `1.x`
is `vec2` at version `1.0`. A machine may implement both, one, or neither, and software
discovers which through the ordinary bitmap check. Nothing is inherited implicitly, and the
successor's chapter restates every behavior it keeps.

This is stricter than the usual reading of a major version number, and the strictness buys
the property the whole architecture is organized around. An instruction encoding means one
thing forever. A byte sequence that decodes to a defined operation on some conforming
machine decodes to that same operation or to a trap on every conforming machine, and never
to a second, incompatible operation.

## What the numbers promise

A version number in Maize v2 carries exactly the following promises, and a program or an
implementer relying on more than this is relying on something the architecture does not
guarantee.

- Base `2.0` names one fixed behavior, identical on every conforming machine, frozen for
  good once this text reaches its release.
- An erratum component on the base names a publication of text corrections, and names a
  behavior change only where a corrected passage was silent and the errata section's bound
  permits the change.
- Extension version `M.n` names one fixed set of assigned encodings and behaviors for that
  extension.
- Extension version `M.n` implements everything `M.k` implemented for every `k` less than
  `n`, with identical encodings and identical semantics.
- Extension names differing in their trailing digit, such as `vec` and `vec2`, name
  unrelated extensions with separate allocations and no compatibility relationship.
- Every number a program can act on is discoverable at runtime through the feature bitmap
  and the boot-information block's extension list, so no program needs to be told which
  machine it is running on.

Three things the numbers do not promise are worth stating outright, because each is a
promise other architectures make. A version number says nothing about performance, and a
higher extension version is not a faster machine. A version number says nothing about
timing, since behavioral conformance in this architecture never depends on timing and the
base carries no cycle counter. A version number says nothing about which optional
extensions a machine implements, because that is what the extension list is for, and a
machine at base `2.0` may implement all seven extension pages or none.

## Versioning and conformance claims

A conformance claim names the base version and the exact set of extensions with their
versions, and the conformance chapter fixes the claim's form and what backs it. The
relationship between the two chapters is that this one defines what the numbers mean and
that one defines what it takes to be entitled to write them down.

A conformance claim as that chapter defines it is a claim against the frozen base, and no
release candidate is entitled to make one: conformance in this architecture presumes a fixed
target that two implementations cannot diverge against, and a release candidate is, by what
this chapter has just spent several sections establishing, not yet fixed. What a
release-candidate test run supports instead, and what the conformance chapter names, is a
suite result: a statement that a machine passed the suite as it stood at a named erratum
level. A suite result is not portable the way a conformance claim is, because the suite
itself can gain a test the next erratum needs, and a result recorded against one erratum
level says nothing about whether the same machine would pass against the next.
