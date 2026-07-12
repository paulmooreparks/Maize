# Chapter 15: Versioning and Freeze Policy

This chapter states the versioning intent for the Maize ISA. The full versioning and freeze
policy (what a version number promises and precisely what may change across a bump) is a
separate deliverable; this chapter fixes the shape it commits to and points to it.

## 15.1 v1.0 is the behavioral freeze point

Version 1.0 freezes the behavior of the machine: the register and subregister model, the
instruction encoding, every instruction's operation / flags / traps, the memory model, the
execution and reset model, the trap and interrupt taxonomy, the device-facing port surface,
and the floating-point contract. A conforming v1.0 implementation reproduces all of it (see
the Conformance chapter). Once frozen, none of these change within the v1.x series except by
the additive, reserved-space rules below.

## 15.2 What a v1.x bump may and may not do

The forward-compatibility guarantee (Chapter 12) is the substance of the versioning policy:

- **A v1.x bump may add capability only from reserved space.** New instructions come from
  reserved opcode encodings; new architectural state comes through the reserved
  control-register mechanism. Every reserved extension is disabled at reset, so a v1.0 binary
  sees the v1.0 machine on any conforming v1.x VM. This covers paging, base-and-bounds
  segments, atomics/SMP ordering primitives beyond CMPXCHG, the thread-pointer system
  register, the escape-prefix second opcode plane, and the versioning/capability hook.
- **A v1.x bump may not break a v1.0 binary.** It may not repurpose a defined encoding,
  narrow a defined behavior, widen the operand register field, or change any observable
  result of a v1.0 instruction. Any change that would alter an encoding or a defined result
  is a major-version break, not a v1.x bump, and must be flagged as such.

## 15.3 Capability detection

So software can detect the extension level of the machine it runs on, v1.0 reserves a
read-only versioning and capability hook: an ISA major/minor version plus a
supported-extension bitmap (floating point, atomics/SMP, segments, paging), analogous to
RISC-V `misa` or x86 CPUID. The carrier (a capability control register, a `SYS` query, or an
`IN` port) is reserved but not fixed in v1.0; see Chapter 12 section 7.

## 15.4 The full policy

The complete versioning and freeze policy, including exactly what a version number promises
across a bump and the process for ratifying a reserved-space extension into a v1.x release,
is a separate deliverable and will be linked here when published. This chapter fixes the
guarantee (sections 15.1 and 15.2) that policy must uphold.
