# Chapter 14: Cycle-Cost Model

**The cycle-cost / performance model is specified separately and is not part of the v1.0
behavioral freeze.**

v1.0 freezes the *behavior* of the machine: every observable result of every instruction.
It deliberately does **not** fix a timing or cycle-cost model. A per-instruction cost model
(how many cycles an instruction takes, and how costs compose) is a separate specification
with its own deliverable and its own lifecycle, so that the performance model can evolve
without touching the behavioral contract, and so that a behavioral conformance claim (Chapter
13) never depends on timing.

Consequently:

- No program's *result* depends on cycle cost. Two conforming implementations produce
  identical observable behavior regardless of how they cost instructions.
- Timing is not observable through the v1.0 ISA. There is no architectural cycle counter in
  the v1.0 instruction set; any such facility, if added, arrives as a reserved-space
  extension (Chapter 12) with its own contract.

When the cycle-cost model is published it will be linked here. Until then, this chapter is a
named deferral: the absence of a timing model in v1.0 is deliberate, not an omission.
