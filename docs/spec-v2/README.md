# The Maize v2 Instruction Set Architecture

This directory holds the specification of Maize v2, the second instruction set architecture
of the Maize fantasy computer. Maize v2 is a 64-bit, flagless, load-store machine with
thirty-two full-width general registers and a byte-granular, variable-length encoding built
for software decode and for translation to a host processor. It is a clean break from Maize
v1 rather than a revision of it, and it does not execute v1 binaries.

The specification is the product. These chapters exist so that a competent implementer can
build a conforming machine from the prose alone, without reading the reference virtual
machine, and the conformance chapter states what it takes to prove that the result conforms.
Every input has a defined outcome, including every invalid one: an encoding this document
does not assign raises a named trap with a stable numeric cause, and nothing anywhere in the
machine is left undefined or implementation-defined by omission.

## Status

**Maize base `2.0`, erratum level `2.0.2`. Ratified and frozen on 2026-08-12.**

That line is this specification's header, and the versioning chapter refers to it by that
name. Its first two components are the base version, which is what a conformance claim names
and what a machine reports; the third is the erratum level, which counts corrections to this
text and which no claim and no machine ever names. A reader tracking whether they hold the
current text watches the third component. A reader writing a conformance claim ignores it and
writes `Maize base 2.0`.

The base version carries no suffix, and no base version follows it.

Frozen means what the versioning chapter says it means. No instruction is added to the base,
no encoding in the base changes meaning, and no reserved byte in the base is ever assigned.
There is no `2.1` and no mechanism to produce one, so everything that arrives later arrives
as a named, independently versioned extension under the rules the extensions chapter fixes.

The text can still be corrected. A passage that is ambiguous, self-contradictory, wrong about
what the reference implementation and the conformance suite already agree on, or silent where
this specification promises no silence, is an erratum, which carries a third version component
and changes no conforming machine's behavior; the versioning chapter states the rule and the
bound on it. A reader who finds two
chapters describing the same behavior differently has found a defect in this text rather
than a choice the architecture leaves open, and an erratum is how it gets corrected.

## The authority chain

Four sources govern this text, and they rank in this order.

1. **The decision record**, `docs/design/maize2-decisions.md`, carries the thirteen ratified
   decisions and the terminology ruling. It is the authority on what the machine is. A
   chapter that contradicts it is wrong, and the remedy is to correct the chapter, never to
   reinterpret the record. A change to a ratified decision is a new dated ruling in that
   document.
2. **The encoding backbone**, meaning the instruction-encoding chapter, the
   instruction-inventory chapter, and the opcode-map appendix, fixes the mnemonics, the
   operand forms, the length classes, and the opcode bytes. Every other chapter draws its
   spellings and its encodings from these three and invents none of its own.
3. **The rest of the normative chapters** state behavior in full. Where a reference chapter
   and the inventory differ in detail, the reference chapter is the fuller statement and the
   inventory is its summary; where they differ in substance, one of them is defective.
4. **The design brief**, `docs/design/maize2-design-brief.md`, carries the rationale that led
   to the decisions. It explains and it does not govern.

The overview chapter is framing rather than requirement, and so are the sections any chapter
marks as implementation notes. Everything else here is normative: it describes a machine in
the indicative, and every behavioral statement is written so that a conformance binary can
pin it.

## Reading order

The chapters are ordered so that a reader who starts at the front meets nothing before its
foundations, and the chapter index lists them in that order with a line each.

Read the terminology chapter first, whatever else you skip. It fixes the size vocabulary, and
that vocabulary deliberately breaks with the one most readers arrive holding: a word on Maize
is 64 bits, a half-word is 32, a quarter-word is 16, and a byte is 8. A reader who carries the
Intel convention or the RISC-V and ARM convention into the later chapters will misread them.

After that, three routes through the material are worth naming.

- An implementer building a machine reads terminology, the register model, the encoding
  chapter, and the opcode map, then the memory model, the execution model, and the three
  instruction-reference chapters, then the trap model and the privileged architecture, and
  finishes with boot, devices, and conformance.
- A compiler or runtime author reads terminology, the register model, the instruction
  inventory, the calling convention, and the assembly-language chapter, and consults the
  reference chapters for the corners.
- A reader who arrives from a search result and needs one answer can enter anywhere, because
  each chapter names the chapters it depends on and repeats no rule it does not own.

## Maize v1

Maize v1 keeps its own specification under `docs/spec/`, its own toolchain, and its own role
as the machine that teaches the x86 flags lineage. It is a complete, frozen, sixteen-register
CISC machine whose arithmetic instructions take memory operands, and it stays exactly that. No
part of this document amends it.
