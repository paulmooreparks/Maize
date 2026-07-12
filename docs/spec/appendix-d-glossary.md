# Appendix D: Glossary

Terms as used in this specification.

- **Base slot / base opcode.** The low six bits of the opcode byte (0..63), selecting the
  instruction; the two high bits are the mode bits or a condition/unary row. (Chapter 6.)
- **binary32 / binary64.** IEEE-754 single (32-bit) and double (64-bit) floating-point
  formats. Under Zfinx, binary32 occupies an H0/H1 subregister and binary64 the full W0.
  (Chapter 8.)
- **BP.** Alias for RB, the base (frame) pointer. (Chapter 2.)
- **byte / quarter-word / half-word / word.** 8 / 16 / 32 / 64 bits. A register is one word.
- **C, N, V, P, Z.** The five arithmetic/logic flags: carry/borrow, negative, signed
  overflow, parity/unordered, zero. (Chapter 2 section 2.4.)
- **CAS.** Compare-and-swap; the CMPXCHG instruction. (Chapter 7 section 7.5.)
- **CISC.** Complex-instruction-set style: ALU instructions may take a memory operand
  directly. Maize is CISC; only CP/LD/ST/CPZ are held to the strict memory boundary.
- **Fault.** A synchronous trap that captures the faulting instruction's PC (retryable).
  (Chapter 10.)
- **FCSR.** The floating-point control/status register: FRM (rounding mode) + FFLAGS (sticky
  exception flags). Not operand-addressable. (Chapter 8.)
- **FL.** Alias for RF.H0, the arithmetic/logic flags. (Chapter 2.)
- **Flat-64.** A single linear 64-bit address space with no segmentation. (Chapter 4.)
- **FRM / FFLAGS.** The rounding-mode field and the sticky exception-flag field of the FCSR.
- **Full-descending stack.** The stack grows toward lower addresses; PUSH/CALL pre-decrement
  SP before writing. (Chapter 7 section 7.8.)
- **Hart.** A hardware thread / execution context. v1.0 is single-hart. (Chapter 12.)
- **Immediate.** A constant encoded in the instruction stream, little-endian, in a
  power-of-two width selected by the source operand byte. (Chapter 5.)
- **MMIO.** Memory-mapped I/O. Maize has **none**: devices are a disjoint port space.
  (Chapter 11.)
- **Mode bits.** The two high bits of the opcode byte, selecting register/immediate and
  value/address for the source operand. (Chapter 5 section 5.1.)
- **PC.** Alias for RP, the program counter. (Chapter 2.)
- **Port space.** The 16-bit device address space (65,536 ports), reached only by IN / OUT /
  OUTR, disjoint from memory. (Chapter 11.)
- **Precise trap.** A trap delivered after full retirement of all prior instructions and
  before any later instruction takes effect. (Chapter 10.)
- **RF.H0 / RF.H1.** The low half of the flag register (FL, arithmetic/logic flags) and the
  high half (privileged status flags). (Chapter 2.)
- **RI.** The decoder-internal instruction register; not operand-addressable. (Chapter 2,
  Chapter 9.)
- **Sparse memory.** Memory allocated on first write, reading zero where never written; no
  fault on unmapped access. (Chapter 4.)
- **SP.** Alias for RS, the stack pointer. (Chapter 2.)
- **Subregister.** A byte / quarter-word / half-word / word view of a register, selected by
  the operand byte's low nibble. (Chapter 3.)
- **Trap (narrow sense).** A synchronous trap that captures the following instruction's PC
  (BRK, SYS). (Chapter 10.)
- **W+X.** Writable-and-executable; rejected by the linker's hygiene pass.
- **Zfinx.** The floating-point model in which FP values live in the integer registers, with
  no separate FP register bank. (Chapter 8.)
- **`@` (at).** The memory-access marker: `@X` dereferences the address X. (Chapter 5.)
