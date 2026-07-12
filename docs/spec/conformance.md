# Chapter 13: Conformance

This chapter is normative in stating *what* conformance means; the concrete conformance
**test suite** is a separate deliverable, referenced at the end.

## 13.1 The conformance contract

A conforming Maize v1.0 implementation reproduces the observable behavior this
specification fixes, for every instruction and every input, bit for bit with the reference
VM. Conformance is defined against **behavior**, not timing (the cycle-cost model, Chapter
14, is out of the behavioral freeze).

The property that makes conformance decidable is the no-undefined-behavior rule (Chapter
10): every condition is either a named trap with a stable cause or an explicitly enumerated
defined, non-trapping result, with no third category. Because nothing is
implementation-defined, two conforming implementations cannot diverge on any input, and a
divergence is always a defect in one of them, never a permitted latitude.

## 13.2 What a conforming implementation must reproduce

- **Instruction semantics.** Every instruction's operation, operand forms, and result, per
  Chapter 7 (integer) and Chapter 8 (floating-point), over every subregister width.
- **Flag effects.** The C / N / V / Z / P effects of every instruction, exactly as Chapter
  7 and Chapter 8 pin them, including the FCMP-only production of P and the explicit
  "unaffected" for every other instruction.
- **Encoding.** The opcode and operand-byte decode, the immediate placement, and the
  defined defaults for decoded-but-undefined operand fields (subregister `$F` to B0,
  immediate-size 4..7 to the value-initialized default), per Chapters 5 and 6.
- **Memory model.** Flat 64-bit, little-endian, sparse read-zero / allocate-on-write,
  misaligned defined-allow, per Chapter 4.
- **Traps.** The trap taxonomy, cause / subcode numbering, precise delivery, the fault vs
  trap PC capture, the saved-state frame, and the deterministic-halt-when-unhandled
  behavior, per Chapter 10.
- **Devices and interrupts.** The port-I/O model, the unpopulated-port read-0 /
  write-discard outcome, and the shared-table external-interrupt vectoring, per Chapter 11.
- **Reset and process start.** The reset register/flag state and the process-start block,
  per Chapters 2, 4, and 9.

## 13.3 Testability shape

Each contract in this specification is stated so it is directly testable from outside the
implementation. For example (Chapter 10): a signed DIV by zero enters cause 2 subcode 0 with
the faulting-instruction PC, or halts deterministically with cause 2 surfaced when no handler
is installed; an undefined opcode enters cause 0 subcode 0 with the offending byte as aux;
BRK enters cause 3 with the following-instruction PC. The same observable shape generalizes
to every entry in the taxonomy and to every instruction's documented result and flags.

## 13.4 The conformance suite

The concrete, executable conformance suite (the corpus of programs and expected results a
third-party VM runs to demonstrate conformance) is a separate deliverable and is not
reproduced here. The reference VM's own regression corpus under `asm/`, driven by
`scripts/run-tests.sh`, exercises a substantial subset today (including the breakpoint-trap
regression named in Chapter 10). This chapter defines the conformance contract; the suite
provides the vectors.
