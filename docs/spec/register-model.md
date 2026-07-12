# Chapter 2: Register Model

This chapter is normative. It fixes the register set, the operand register encoding, the
special-purpose register roles, and the flag-bit layout.

## 2.1 The register file

Maize has **sixteen operand-addressable 64-bit registers**. Every register is a full
word (64 bits) and is sliced into subregisters by Chapter 3. The 4-bit operand register
field (Chapter 5) encodes all sixteen; there is no unallocated register encoding.

| Field | Register | Role |
|:-----:|:---------|:-----|
| `$0` | R0 | General purpose |
| `$1` | R1 | General purpose |
| `$2` | R2 | General purpose |
| `$3` | R3 | General purpose |
| `$4` | R4 | General purpose |
| `$5` | R5 | General purpose |
| `$6` | R6 | General purpose |
| `$7` | R7 | General purpose |
| `$8` | R8 | General purpose |
| `$9` | R9 | General purpose (thread pointer by ABI convention, section 2.5) |
| `$A` | RT | Temporary register |
| `$B` | RV | Return-value register |
| `$C` | RF | Flag register |
| `$D` | RB | Base-pointer register (alias **BP**) |
| `$E` | RP | Program-counter register (alias **PC**) |
| `$F` | RS | Stack-pointer register (alias **SP**) |

R0 through R9 are the ten general-purpose registers. RT, RV, RF, RB, RP, and RS are
special-purpose (section 2.3). The operand register field value is the low nibble of the
operand byte; the value in the table is that nibble.

## 2.2 The instruction register RI (not operand-addressable)

There is a seventeenth register, **RI**, the instruction register. The decoder writes RI
as it reads each opcode byte and its operand bytes from memory. RI is **decoder-internal**:
it has no operand-field encoding and cannot be named as an instruction operand. It is
listed here only so the count is complete; software cannot read or write it.

## 2.3 Special-purpose registers

- **RT (temporary).** A general scratch register. The C calling convention reserves it as
  back-end scratch (not register-allocatable); see Appendix C. It is otherwise an ordinary
  64-bit register.
- **RV (return value).** Function and syscall results are placed here by convention.
  Ordinary register in every other respect.
- **RF (flags).** `RF.H0` (the low 32 bits) is aliased **FL** and holds the
  arithmetic/logic status flags (section 2.4). `RF.H1` (the high 32 bits) holds the
  privileged status flags and may only be written in privileged mode. The
  arithmetic/logic instructions never touch RF.H1.
- **RB / BP (base pointer).** The frame (base) pointer for the current stack frame, a full
  64-bit address. `BP` is an assembler alias for `RB`.
- **RP / PC (program counter).** The full 64-bit address of the next instruction to be
  decoded. `PC` is an alias for `RP`. JMP always targets the full 64-bit width and ignores
  any subregister selection on its operand; CALL, by contrast, honors the operand
  subregister and zero-extends the selected field into PC (Chapter 7 sections 7.7).
- **RS / SP (stack pointer).** The full 64-bit address of the top of the stack. `SP` is an
  alias for `RS`. The stack is **full-descending**: PUSH and CALL pre-decrement RS before
  writing, so RS always points at the last value pushed. See section 2.6 and Chapter 9 for
  the reset value and the process-start block.

An earlier register enum in the reference header (`reg_enum`) carries the legacy internal
names `fl`/`in`/`pc`/`sp` at nibbles `$C`..`$F`; that enum is a decoder-internal artifact.
The operand-addressable mapping that the ISA freezes is the table in section 2.1
(`opflag_reg_*` in `src/maize_cpu.h`, cross-checked against the README "Register bit
field"): `$C`=RF, `$D`=RB/BP, `$E`=RP/PC, `$F`=RS/SP.

## 2.4 The arithmetic/logic flags (FL = RF.H0)

`RF.H0`, aliased **FL**, holds five live status flags and two reserved bits. The bit
positions are frozen ISA contract:

| Bit | Symbol | Name | Meaning |
|:---:|:------:|:-----|:--------|
| 0 | C | Carry | Unsigned carry-out (ADD/ADC/INC) or borrow (SUB/SBB/CMP/CMPIND/DEC/NEG). For shifts, the last bit shifted out. Set by SETCRY, cleared by CLRCRY. |
| 1 | N | Negative | The sign bit of the result. |
| 2 | V | Overflow | Signed overflow: the signed result does not fit the operand width. |
| 3 | P | Parity / unordered | Set by FCMP when a floating-point compare is **unordered** (either operand is NaN). Read by the JP and SETP predicates. **No integer instruction computes P.** |
| 4 | Z | Zero | The result is zero. |
| 5 | - | reserved | Reserved. (The reference VM declares an unused sign-flag bit here that is never read or written.) |
| 6 | - | reserved | Reserved. |

**P (bit 3) is a live, allocated flag, not spare bits.** It occupies allocated encoding
space, is set only by the FCMP instruction on an unordered (NaN) compare, and is consumed
only by the JP (jump-if-parity) and SETP (set-if-parity) predicates. Every non-FCMP
instruction leaves P unaffected. FCMP itself computes C, Z, and P together and forces
N = V = 0 (Chapter 7 section 7.5 and Chapter 8). The two **actually** reserved flag bits
are bit 5 and bit 6.

**C and V are distinct.** C is the unsigned carry/borrow flag and uses the x86 borrow
convention: after SUB or CMP, C is set exactly when the destination was unsigned-less-than
the source. This is what makes JB ("below") and JA ("above") the correct unsigned branches
directly off a compare. V is the signed-overflow flag and drives the signed branches JLT
and JGT. Each has a complement: JGE/JLE (signed) and JAE/JBE (unsigned).

Data movement and address computation (CP, CPZ, LD, ST, CLR, LEA) leave C/N/V/Z (and P)
unchanged, matching x86 MOV / ARM / RISC-V, so a compare and its dependent branch may be
separated by register shuffling. The exact per-instruction flag effects, by operand width
and including the explicit statement of P for every entry, are in Chapter 7.

## 2.5 The privileged status flags (RF.H1)

`RF.H1` holds four status bits that may be written only in privileged mode and are
unaffected by arithmetic/logic instructions. In the reference VM they occupy the low bits
of the high half (bit positions within the full 64-bit RF word given for grounding):

| RF word bit | Symbol | Meaning |
|:-----------:|:-------|:--------|
| 32 | privilege | Set in privileged (supervisor) mode; cleared in user mode. |
| 33 | interrupt-enabled | Maskable external interrupts are enabled. Toggled by SETINT / CLRINT. |
| 34 | interrupt-set | An external interrupt is pending (the raise latch signalling the run loop). |
| 35 | running | Set once execution begins; cleared by HALT. |

The privilege bit gates the privileged instructions (Chapter 7 section 7.9; the candidate
privileged set is finalized with the interrupt and segment work). The interrupt-enable bit
governs only maskable external interrupts; synchronous traps are unmaskable (Chapter 10).

## 2.6 Reset and process-start register state

At process start the register and stack state is a guaranteed contract, not incidental
defaults (crt0 and the C calling convention depend on it from the first instruction):

- **RP / PC** = the program entry: the recorded entry point for a `.mzx` executable, or
  address `$0` for a flat `.mzb` image.
- **RS / SP** = the base of the process-start block, so RS points at argc (Chapter 4 section
  4.5). The block occupies the top of the address space and ends at `$FFFF_FFFF_FFFF_FFF8`
  (the top of the block, not RS). The stack grows downward; the first guest push
  pre-decrements RS into the free region just below the block.
- **RB / BP** = 0.
- **R0..R9, RT, RV** = 0.
- **RF**: the arithmetic/logic flags (RF.H0) are clear; the privilege bit is set (execution
  starts privileged); interrupts are disabled; the running bit is set once execution begins.

Full reset semantics, including paging-off and the process-start block layout, are in
Chapter 9 (execution model) and Chapter 4 (memory model).

## 2.7 The thread pointer (R9, by convention)

Because all sixteen operand-register encodings are allocated, a thread pointer cannot be a
new operand-addressable register. v1.0 designates **R9** as the thread pointer by C-ABI
convention (callee-saved, never an argument, the highest general register), the same way
RISC-V designates `tp` by convention. This costs no encoding and is an ABI agreement, not
an instruction; the machine, `mazm`, and `mzdis` are unchanged. See Chapter 12 section 4
and Appendix C.

## Sourcing

- Register set and operand encoding: `src/maize_cpu.h` `opflag_reg_*` constants (lines
  ~31-47) and the `regs` extern declarations (~846-866); README "Registers", "Special-purpose
  Registers", and "Register bit field".
- Flag-bit layout: `src/cpu.cpp` flag-bit constants `bit_carryout`/`bit_negative`/
  `bit_overflow`/`bit_parity`/`bit_zero`/`bit_sign`/`bit_reserved` (lines 18-24) and the
  RF.H1 constants `bit_privilege`/`bit_interrupt_enabled`/`bit_interrupt_set`/`bit_running`
  (lines 25-28); README "Flags" table. P is set by `do_fcmp` (cpu.cpp ~1001-1019) and read
  by `eval_condition` case 10 (cpu.cpp ~793).
- Reset / process-start contract: README "Process start" and "Execution"; Chapter 9.
