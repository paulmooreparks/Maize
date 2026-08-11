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

The base version of this specification is `2.0`. While the text is under ratification it is
written `2.0-draft`, and the suffix disappears at ratification with no other change to the
number. A machine implementing the ratified text implements Maize base `2.0`, and there is
no base version after it.

Maize v1 remains a separate architecture with its own version line. The `2` is a lineage
marker, not a compatibility statement, and no v1 binary runs on a v2 machine.

### The base does not revise

Once base `2.0` is ratified, no change to the behavior of the base architecture is possible
within Maize v2. There is no `2.1`, there is no mechanism to produce one, and this
specification defines no process by which the frozen text could be amended to alter a
behavior. Every capability added to Maize v2 after ratification arrives as a named extension
under the rules the extensions chapter fixes.

The consequence for a program is direct. A binary that uses only base instructions and only
base architectural state behaves identically on every conforming Maize v2 machine that will
ever be built, and a machine on which it does not is defective rather than differently
versioned.

The consequence for an implementer is equally direct. An implementation of base `2.0` is
never obsoleted by a later base, and the work of building one is bounded by a document that
has stopped moving. The weekend-reimplementable budget the design set for itself only means
anything if the target holds still.

### Errata

A ratified specification can still contain a passage that is ambiguous, self-contradictory,
or wrong about what the reference implementation and the conformance suite already agree on.
Correcting such a passage is an erratum, and an erratum carries a third version component:
`2.0.1`, `2.0.2`, and so on.

An erratum is bounded by one rule. It changes the text so that it says what the architecture
already required, and it changes no conformance test's expected result. If correcting a
passage would change what a conforming machine does, the passage is not an erratum
candidate; the behavior stands as the conformance suite pins it, and the desired change is
extension work or it does not happen.

The erratum component is documentation for humans reading the specification. It appears in
the specification's own header and in the registry, it is not reported by any control and
status register, it does not appear in the boot-information block, and a conformance claim
does not name it. Nothing a program can observe distinguishes `2.0.0` from `2.0.7`.

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

- Base `2.0` names one fixed, frozen behavior, identical on every conforming machine.
- An erratum component on the base names a text correction and never a behavior change.
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
