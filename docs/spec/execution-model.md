# Chapter 9: Execution Model

This chapter is normative. It fixes the fetch/decode/execute cycle, the reset state, the
process-start contract, and the stop conditions.

## 9.1 The fetch/decode/execute cycle

Maize is a **strictly in-order** interpreter. It fully retires one instruction, with all of
its architectural effects, before it decodes the next. There is no speculation, no
out-of-order retirement, and no pipeline visible to the program. Each cycle:

1. **Fetch/decode.** Read the opcode byte at PC (RP) into the decoder-internal instruction
   register RI, determine the addressing-mode form and how many operand bytes and immediate
   bytes follow (Chapters 5 and 6), read them, and advance RP past the whole instruction.
2. **Execute.** Perform the instruction's operation: register and memory effects, flag
   updates, and any control transfer (which overwrites RP). An ALU instruction routes
   through the shared arithmetic/logic unit, which computes the result and the flags for the
   destination width.
3. **Retire.** All effects are committed before the next fetch. A synchronous trap raised
   mid-instruction is delivered precisely: prior instructions are fully retired and no later
   instruction takes effect (Chapter 10).

Because retirement is complete before the next decode, PC always names the next instruction
to run, a fault captures the faulting instruction's entry PC, and a trap (BRK, SYS) captures
the following instruction's PC (Chapter 10).

## 9.2 The instruction register RI

RI is the decoder-internal register that holds the instruction bytes as they are read. It
drives the addressing-mode and condition decode (the two high opcode bits select the
addressing-mode form or the condition row; Chapter 6). RI is not operand-addressable:
software cannot read or write it.

## 9.3 Reset state

At power-on / reset the machine comes up in a fully defined state:

- **Privileged.** The RF privilege bit is set; execution starts in supervisor mode.
- **Interrupts disabled.** The RF interrupt-enable bit is clear; maskable external
  interrupts are masked until SETINT.
- **Flags clear.** The arithmetic/logic flags (RF.H0 = FL): C, N, V, P, Z are all 0.
- **Paging off.** No MMU is armed; the machine runs in the flat 64-bit model. Paging-off is
  the permanent reset state and a first-class mode: every future MMU extension is disabled at
  reset, so a v1.0 image sees the v1.0 machine on any conforming VM (Chapter 12).
- **Stack pointer at top of space.** RS (SP) = `$FFFF_FFFF_FFFF_FFF8`, the highest
  8-byte-aligned address, the base of the process-start block.
- **Entry point.** RP (PC) = the recorded entry point of a `.mzx` executable, or `$0` for a
  flat `.mzb` image.
- **Registers zero.** RB (BP) = 0; R0..R9, RT, RV = 0.
- **Running.** The running bit (RF.H1) is set once execution begins.

These values are a guaranteed contract, not incidental defaults; crt0 and the C calling
convention depend on a usable stack pointer and a well-formed process-start block from the
first instruction (Chapter 2 section 2.6, Chapter 4 section 4.5).

## 9.4 Process start

A fresh VM invocation builds the System V-style process-start block at the top of the
address space, points RS at its base (argc), and enters at the program entry. The block
carries argc, the argv pointer array (NULL-terminated), the envp pointer array (always
present and NULL-terminated), and the packed argument and environment strings; each slot is
8 bytes, little-endian. The full layout is Chapter 4 section 4.5. The C runtime's crt0 reads
argc / argv / envp off this block into the argument registers and calls `main`; a
`main(void)` ignores them.

A program's environment is built only from what the invocation passes on the command line;
the VM never inherits the host shell's environment, so a run is deterministic. Command-line
arguments after the image become argv; explicitly passed environment entries become envp.

## 9.5 Stop conditions

Execution stops in one of these defined ways:

- **HALT (`$00`).** Halts the core pending an interrupt. With no interrupt source a halted
  core has nothing to wake it, so the run loop returns and the host process exits 0 with **no
  recorded status**. Because `$00` is HALT, a run of zeroed memory reached as code halts
  cleanly rather than executing garbage.
- **sys_exit (`SYS $3C`).** The status-carrying termination path: it records the low 8 bits
  of R0 as the process exit status and stops the VM, so the host process returns that value.
  Values wrap to the 0..255 range (the host truncates the process status to 8 bits). This is
  distinct from HALT, which records no status. See Appendix C.
- **Unhandled synchronous trap.** With no handler installed, a fired trap halts the VM
  deterministically with the cause surfaced (Chapter 10). No instruction after the trapping
  instruction takes effect.

The `running` bit (RF.H1) reflects the executing state and clears when the core halts.

## 9.6 Determinism

Given the same image and the same declared inputs (command-line arguments, environment
entries, and directory mounts), a Maize run is a pure function of those inputs. There is no
ambient host state, no uninitialized-memory nondeterminism (unwritten memory reads as 0), and
no undefined behavior (Chapter 10). Two conforming VMs produce identical observable behavior
on the same inputs.

## Sourcing

- Fetch/decode/execute and RI: the reference VM `run()` / decode loop and `run_alu` in
  `src/cpu.cpp`; README "Execution".
- Reset and process-start contract: README "Process start" and "Execution"; Chapters 2 and 4.
  Paging-off reset state: the Reserved Space chapter.
- Stop conditions: README "Hello, World!" HALT note and the `sys_exit` (`SYS $3C`)
  description; the deterministic-halt trap behavior in the Trap Model chapter.
