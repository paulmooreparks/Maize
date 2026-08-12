# Errata

This file records every correction issued against the ratified text of Maize base `2.0`. It
is the companion to the erratum component of the specification's header: the header says
which text a reader holds, and this log says what changed to get there.

An erratum corrects a passage that is ambiguous, self-contradictory, or wrong about what the
reference implementation and the conformance suite already agree on, and it changes no
conformance test's expected result. The versioning chapter states the rule and its bound. A
change that would alter what a conforming machine does is not an erratum, whatever its size.

## What an entry records

Each entry carries six things, and the last two are the ones that make it useful rather than
merely traceable.

- The erratum level it introduced, and the date it issued.
- The chapter, and the section heading within it that contains the corrected passage. A
  heading is used rather than a line number, because a correction moves the lines around
  itself and a log that cited them would be wrong about its own subject.
- What the passage said before.
- What it says now.
- What the corrected text requires of an implementation, stated as a requirement rather than
  as a description of the edit.

The before-and-after pair is what lets a reader who last read an earlier level find out
whether anything they relied on moved, without diffing two documents. The requirement is what
lets an implementer decide whether they have work: an erratum states something that was
already binding but recorded badly, so a machine that guessed right needs no change and one
that guessed wrong was already non-conforming. An entry that leaves a reader unable to answer
either question has not done its job.

Entries appear in issue order, oldest first. No entry is removed or rewritten, and a
correction to a correction is a new entry.

## Entries

None. No erratum has issued since ratification on 2026-08-12.

That will not stay true. A specification meets its first implementer, and the places where it
was silent or over-precise become visible in a way that review before implementation does not
reveal.
