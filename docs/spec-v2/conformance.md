# Conformance

This chapter is normative. It defines what it means to claim conformance to Maize v2, what
such a claim names, how the conformance suite is organized, what discipline a conformance
test binary follows, and how reserved and unimplemented space is tested. The suite itself is
a separate deliverable, a corpus of guest binaries with expected results; this chapter fixes
the contract the suite exists to check.

Conformance in Maize v2 is decidable, and that is a property of how the rest of this
specification is written rather than a claim made here. Every instruction, every operand
encoding, and every input value has a defined outcome, where a defined outcome is either a
stated result or a named trap with a stated cause. No third category exists. Nothing is
implementation-defined and nothing is undefined, so two conforming machines cannot diverge
on any input, and a divergence is a defect in one of them rather than latitude either was
granted.

Conformance is defined over behavior and never over timing. The base carries no cycle
counter, and no conformance test may depend on how long anything takes.

## What a conformance claim names

A conformance claim names the base version and the exact set of implemented extensions with
their versions. Both parts are required, and the extension set is exhaustive rather than
illustrative, so a claim listing `vec 1.2` asserts both that `vec 1.2` is implemented and
that every other ratified extension is not.

A well-formed claim reads like the following two examples.

    Maize base 2.0
    Maize base 2.0, vec 1.2, atomic 1.0

The first is a base-only machine and it is a complete claim. Nothing about it is partial,
provisional, or reduced.

A claim is checkable from inside the guest, which is what keeps it honest. A conformance
binary reads the feature bitmap, walks the extension list in the boot-information block, and
asserts that the two agree with each other and with the claim under test. A machine whose
bitmap advertises an extension its list omits fails conformance, as does a machine that
advertises an extension whose suite section it cannot pass, and as does a machine that
implements an extension's instructions while advertising neither.

## How the suite is factored

The conformance suite is factored the way the architecture is factored, into one base
section plus one section per ratified extension. The base section tests the base and nothing
else, and it runs to completion on a machine that implements no extension. Each extension
section tests that extension's own instructions, registers, and traps, and it runs only on a
machine claiming that extension.

Three rules govern the factoring, and together they keep the sections independent.

1. The base section uses no extension instruction, reads no register outside the base
   range, and passes identically on a base-only machine and on a machine implementing every
   extension. Base behavior does not vary with what else is present, so a base test that
   behaved differently under an extension would be evidence that the extension violated the
   extensions chapter's prohibitions.
2. An extension section uses base instructions freely, since the base is always present, but
   uses no other extension's instructions unless that extension is a declared dependency of
   the one under test.
3. The absence tests are part of the base section rather than of any extension section,
   because absence is a property every machine has for every extension it does not
   implement.

A machine is conforming when it passes the base section plus exactly the extension sections
its claim names, and fails the absence tests for nothing it claims. Running an extension
section a machine does not claim is not a failure of that machine; the section simply does
not apply to it.

## The test-binary discipline

Every normative statement in this specification is pinnable by a guest binary that observes
architectural state. This is a constraint on how the specification is written as much as on
how the suite is built: a sentence stating a behavior no binary could distinguish from its
negation does not belong in this document, and the drafting rule is that such a sentence is
either sharpened until a test can see it or moved into non-normative commentary.

A conformance test binary runs on the machine under test, exercises one behavior, records
what it observed into architectural state, and halts. The observable channel is the
architectural state at the halt: the general registers, the control and status registers the
test is entitled to read, and the contents of guest memory the test wrote. Using
architectural state as the channel keeps the suite independent of the details of any device
surface, so a test states its result the same way whatever devices the machine carries. The
conforming device set is not empty. The device-surface chapter requires the console and the
timer of every conforming machine, and the base section's interrupt-delivery tests need the
timer in order to make a cause pending at all.

Each test observes one thing and states one expected result. A test that exercises six
behaviors and halts with a single pass-or-fail word tells a failing implementer that
something is wrong and nothing about what, which is exactly the information a suite exists
to supply.

Four categories cover the base section, and every extension section is organized the same
way.

### Decode-length checks

A decode-length check asserts that the machine derives the same instruction length this
specification assigns, for every encoding, without executing anything whose result would
confuse the measurement. The straightforward construction places the instruction under test
in front of a known instruction, arranges for the instruction under test to have no effect
the following one could not distinguish, and observes whether control arrived where the
length table places it.

The base section covers 254 of the 256 primary opcode bytes this way. Each assigned byte
yields its assigned length, each reserved byte and each escape byte yields the
illegal-instruction trap, and no byte yields a third outcome. Two bytes fall outside the
construction, because nothing downstream of them can report where the next instruction would
have begun. The `halt` byte stops the machine for the rest of the run, and the
`wait_for_interrupt` byte on a machine with no cause pending and enabled never continues, so
the assigned length of each is architecturally invisible and the suite asserts the behavior of
those two bytes instead of their length. The check is meaningful precisely because length
comes from the leading bytes alone, so a machine that got length right by accident of
execution rather than by table lookup fails somewhere in the 256.

### Trap checks

A trap check installs a handler, executes an instruction that this specification says traps,
and asserts on everything the trap model makes visible: that the trap arrived at all, that
the cause is the one this specification names, that the reported faulting address is the
address this specification names, and that the auxiliary word carries the offending byte or
value where the trap defines one.

Trap checks are the largest category in the base section, because the no-undefined-behavior
rule turns every invalid input into a named trap and every named trap into a testable
statement. Division by zero, an illegal operand form in a plain slot, a quarter-word slice
index of `%100`, a write to a read-only control and status register, an access to an
unimplemented register number, a supervisor-only access from user privilege, a page fault on
an unmapped address, and a breakpoint are each a test, and each asserts a specific cause
rather than merely that something went wrong.

A trap check also asserts what did not happen. An instruction that traps produces no partial
result, so the check reads back the registers and memory the instruction would have written
and asserts that they hold their prior values.

### Boundary-value checks

A boundary-value check exercises an operation at the edges of its input domain, where the
edges are the values a specification sentence distinguishes and the values an implementation
is most likely to handle by accident. The base section covers the signed minimum and maximum
at every width, zero and negative zero, the all-ones pattern, a shift count of zero and of
the width and of one past the width, the largest and smallest displacements a branch can
encode, an address at the last accessible byte of a mapped page, an instruction whose final
byte is the final accessible byte of a mapped page, and the floating-point special values
with the rounding mode set to each of its settings.

Half-word operations get particular attention, because the specification's rule that a `.h`
result zero-extends into the full word is the kind of statement an implementation satisfies
on typical values and violates at the sign boundary. A check that computes a negative
half-word result and reads back the full word catches that directly.

### Restartability checks

A restartability check faults an instruction deliberately, services the fault in the
handler, returns, and asserts that the instruction completed correctly and that the
architectural state along the way was what this specification says it was. The construction
that makes the category tractable is a page that is unmapped when the instruction begins and
mapped by the handler.

Two cases exist and the suite covers both. A single-step instruction that faults resumes
from its first byte and produces exactly the result it would have produced had the page been
mapped all along. A multi-step instruction, meaning the block-memory family, exposes the
mid-operation register state this specification defines, so the check reads the pointer and
count registers inside the handler and asserts they hold the values the instruction's own
entry says they hold at that point, then returns and asserts the operation finished
correctly.

Restartability checks are where an implementation that computes results before checking
faults is caught, and they are the reason the category is separate rather than folded into
trap checks.

## Reserved and unimplemented space

Reserved and unimplemented space is tested positively rather than left alone, because
silence there is indistinguishable from tolerance and tolerance is a conformance failure.

The base section executes each of the 63 reserved primary opcode bytes in turn and asserts
63 illegal-instruction traps differing only in the offending byte and the faulting address.
It executes each of the seven escape bytes for extensions the machine does not implement and
asserts the illegal-instruction trap on the escape byte itself, with the faulting address
equal to the escape byte's own address. It places a byte after each such escape byte that
would fault differently if fetched, and asserts that nothing fetched it. It reads and writes
well-formed control-and-status-register numbers the machine does not implement and asserts
the illegal-operand trap on each.

Three outcomes are conformance failures wherever reserved or unimplemented space is
concerned, and a test that observes any of them reports a failure rather than a warning.

- A reserved or unimplemented encoding that executes without a trap, whatever it does or
  does not compute.
- A trap with a cause, faulting address, or auxiliary value other than the one this
  specification names for that encoding.
- An unimplemented control and status register that reads as zero or discards a write
  instead of trapping.

The third is called out because it was the v1 convention, and a v1 implementation ported
forward is the most likely source of it. Reading zero from an unimplemented register makes a
missing feature look like a present feature holding a default, which defeats discovery
exactly where discovery matters most.

## Reporting a result

A conformance report names the claim under test, the suite version it was run against, and
the outcome of every section. A section outcome is pass or fail, and a failing section
reports each failing test by name with the observed value beside the expected one.

There is no partial pass. A machine that passes the base section and fails one extension
section conforms to a smaller claim, the one that omits that extension, and reporting it
that way is accurate. A machine that fails any test in the base section does not conform to
Maize v2 at all, and there is no reduced claim available to it, because the base is not
divisible.
