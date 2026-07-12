# Chapter 7: Instruction Reference

This chapter is normative and is the core of the specification. It documents every
instruction in the opcode map, grouped by function. Floating-point instructions are
documented in Chapter 8; section 7.10 cross-references them.

## 7.0 How to read an entry

Every entry pins the same five items, and none is left implementation-defined:

- **Operation:** the one-line semantics.
- **Forms:** each addressing-mode opcode (`$xx`) and its operand shape. The mode-bit
  scheme is Chapter 5 section 5.1; `regVal`/`immVal`/`regAddr`/`immAddr` are the four source
  forms. Forms not listed for an instruction are reserved encodings.
- **Flags:** the effect on each of C, N, V, Z, and P, by operand width where it differs.
- **Encoding:** the opcode byte plus operand-byte layout (Chapter 6).
- **Traps:** which trap cause the instruction can raise, or "none" (Chapter 10).

**Flag notation.** The five arithmetic/logic flags are C (carry/borrow), N (negative), V
(signed overflow), Z (zero), and P (parity/unordered). The flag layout is Chapter 2 section
2.4. **P is written by exactly one instruction, FCMP** (Chapter 8); every other instruction
in this chapter leaves P unaffected, and each entry states so explicitly. "unaffected"
means the bit keeps its prior value. Unless an entry's Flags line says otherwise, the flags
are computed over the **destination operand's selected subregister width** (byte / qword /
hword / word), and the sign bit N, the overflow test V, and the zero test Z are taken at
that width.

**Common trap facts.** An undefined opcode byte is the illegal-instruction trap (cause 0).
Decoded-but-undefined operand fields (subregister `$F`, immediate-size 4..7) do not trap
(Chapter 6 section 6.6). Where an entry says "Traps: none", the instruction raises no
synchronous trap for any operand or value.

---

## 7.1 Data movement

These instructions move data without arithmetic. **None of them affects any flag**
(C/N/V/Z/P all unaffected), matching x86 MOV / ARM / RISC-V, so a compare and its dependent
branch may be separated by data moves. CP / CPZ / LD / ST are the enforced memory boundary
(Chapter 5 section 5.5).

### CP (copy)
- **Operation:** copy the source value into the destination register, **sign-extended** to
  the destination subregister width (Chapter 5 section 5.6). Never touches memory.
- **Forms:** `$01` CP regVal reg; `$41` CP immVal reg. (No address forms: a copy never
  reads memory. Base slot `$01` is shared with LD, which owns the address forms.)
- **Flags:** C/N/V/Z/P unaffected.
- **Encoding:** opcode byte, source operand byte, destination operand byte, then immediate
  bytes for the immVal form.
- **Traps:** none.

### CPZ (copy, zero-extended)
- **Operation:** copy the source value into the destination register, **zero-extended** to
  the destination width. Never touches memory.
- **Forms:** `$13` CPZ regVal reg; `$53` CPZ immVal reg. (The former address forms `$93` /
  `$D3` were the removed LDZ and are reserved.)
- **Flags:** C/N/V/Z/P unaffected.
- **Encoding:** as CP.
- **Traps:** none.

### LD (load)
- **Operation:** read from a memory address into the destination register. The number of
  bytes read is fixed by the destination subregister width, landing in that field and
  preserving the rest of the register (Chapter 3 section 3.4). A load never over-reads past
  its source address and never has a narrower value to extend, which is why there is no
  zero-extending load (no LDZ).
- **Forms:** `$81` LD regAddr reg (address in a register); `$C1` LD immAddr reg (immediate
  address).
- **Flags:** C/N/V/Z/P unaffected.
- **Encoding:** opcode byte, source operand byte (the address register, or immediate-size
  for the immAddr form), destination operand byte, then immediate address bytes for immAddr.
- **Traps:** none. A read of never-written memory returns 0 (Chapter 4 section 4.3);
  misaligned reads are defined-allow (section 4.4).

### ST (store)
- **Operation:** write a register value or immediate to a memory address. The number of
  bytes written is the source's selected width.
- **Forms:** `$02` ST regVal regAddr (register value to address in a register); `$42` ST
  immVal regAddr (immediate value to address in a register).
- **Flags:** C/N/V/Z/P unaffected.
- **Encoding:** opcode byte, source operand byte, destination (address) operand byte, then
  immediate bytes for immVal.
- **Traps:** none. A store to never-written memory allocates a zero-filled block (section
  4.3); misaligned stores are defined-allow.

### CLR (clear)
- **Operation:** set the destination to zero. A bare register destination writes the full
  W0 (a clean 0 with no stale upper bits); an explicit subregister writes only that field
  and preserves the rest.
- **Forms:** `$32` CLR regVal (register-only; row 0 of base `$32`).
- **Flags:** C/N/V/Z/P unaffected.
- **Encoding:** opcode byte, one register operand byte.
- **Traps:** none.

### LEA (load effective address)
- **Operation:** compute operand1 + operand2 and store the sum in operand3. Address
  arithmetic; no memory is dereferenced for the value forms. `LEA $-08 BP RT` puts `BP - 8`
  in RT.
- **Forms (three operands):** `$12` LEA regVal reg reg; `$52` LEA immVal reg reg; `$92` LEA
  regAddr reg reg; `$D2` LEA immAddr reg reg. Operand1 is the addend (value / immediate /
  value-at-address), operand2 the base register, operand3 the destination.
- **Flags:** C/N/V/Z/P unaffected (address computation never sets flags).
- **Encoding:** opcode byte, operand1 byte, operand2 byte, operand3 byte, then immediate
  bytes for the immediate forms.
- **Traps:** none.

### XCHG (exchange)
- **Operation:** atomically exchange the values of two registers.
- **Forms:** `$E0` XCHG reg reg (register-only; single encoding).
- **Flags:** C/N/V/Z/P unaffected.
- **Encoding:** opcode byte, two register operand bytes.
- **Traps:** none.

### PUSH
- **Operation:** push the source onto the stack. The stack is full-descending: RS (SP) is
  pre-decremented by the operand's size, then the value is written at RS.
- **Forms:** `$20` PUSH regVal; `$60` PUSH immVal.
- **Flags:** C/N/V/Z/P unaffected.
- **Encoding:** opcode byte, source operand byte, then immediate bytes for immVal.
- **Traps:** none. (See section 7.8 for the stack model.)

### POP
- **Operation:** pop the top of the stack into the destination register: copy the value at
  RS into the register, then increment RS by the register's size.
- **Forms:** `$72` POP regVal (register-only; row 1 of base `$32`).
- **Flags:** C/N/V/Z/P unaffected.
- **Encoding:** opcode byte, one register operand byte.
- **Traps:** none.

---

## 7.2 Integer arithmetic

Unless an entry says otherwise, these set C/N/V/Z over the destination width and **leave P
unaffected**. Integer overflow wraps two's-complement and is observed through C and V, never
a trap (Chapter 10). All four addressing-mode source forms exist for the binary ALU ops
(regVal `$0x`, immVal `$4x`, regAddr `$8x`, immAddr `$Cx`) unless noted.

### ADD
- **Operation:** destination = destination + source.
- **Forms:** `$03` regVal reg, `$43` immVal reg, `$83` regAddr reg, `$C3` immAddr reg.
- **Flags:** C = unsigned carry-out; N = sign of result; V = signed overflow; Z = result is
  zero; P unaffected.
- **Encoding:** opcode byte, source operand byte, destination register byte, immediate bytes
  for immediate forms.
- **Traps:** none.

### SUB
- **Operation:** destination = destination - source.
- **Forms:** `$04` regVal reg, `$44` immVal reg, `$84` regAddr reg, `$C4` immAddr reg.
- **Flags:** C = unsigned borrow (set when destination was unsigned-less-than source); N;
  V = signed overflow; Z; P unaffected.
- **Traps:** none.

### MUL
- **Operation:** destination = low half of (destination * source). Keeps only the low half
  of the product; for the full double-width product use MULW / UMULW.
- **Forms:** `$05` regVal reg, `$45` immVal reg, `$85` regAddr reg, `$C5` immAddr reg.
- **Flags:** V = signed overflow (product does not fit the destination width); C = V
  (C mirrors the overflow flag); N = sign of the low-half result; Z = result is zero; P
  unaffected.
- **Traps:** none.

### MULW (signed wide multiply)
- **Operation:** full double-width signed product of operand2 by the source; low half to
  operand2, high half to operand3.
- **Forms (three operands):** `$3D` regVal reg reg, `$7D` immVal reg reg, `$BD` regAddr reg
  reg, `$FD` immAddr reg reg.
- **Flags:** C = high half nonzero (the product spilled out of the low register); N = sign
  bit of the whole double-width product; V = true signed overflow (the product does not fit
  the low register's signed range); Z = the entire product is zero; P unaffected.
- **Traps:** none.

### UMULW (unsigned wide multiply)
- **Operation:** full double-width unsigned product of operand2 by the source; low half to
  operand2, high half to operand3.
- **Forms:** `$3E` regVal reg reg, `$7E` immVal reg reg, `$BE` regAddr reg reg, `$FE`
  immAddr reg reg.
- **Flags:** C = high half nonzero; V = C; N = sign bit of the whole product; Z = entire
  product is zero; P unaffected.
- **Traps:** none.

### DIV (signed divide)
- **Operation:** destination = destination / source, signed, truncated toward zero.
- **Forms:** `$06` regVal reg, `$46` immVal reg, `$86` regAddr reg, `$C6` immAddr reg.
- **Flags:** C = 0 (cleared); V = 0 (cleared); N = sign of result; Z = result is zero; P
  unaffected.
- **Traps:** **divide error (cause 2)** on a zero divisor (subcode 0) or the signed
  `INT_MIN / -1` quotient overflow (subcode 1). Fault-class; captures the DIV instruction's
  PC. Unhandled, the machine halts deterministically with cause 2 surfaced.

### MOD (signed remainder)
- **Operation:** destination = destination mod source, signed; the remainder takes the sign
  of the dividend.
- **Forms:** `$07` regVal reg, `$47` immVal reg, `$87` regAddr reg, `$C7` immAddr reg.
- **Flags:** C = 0; V = 0; N = sign of result; Z = result is zero; P unaffected.
- **Traps:** divide error (cause 2), as DIV.

### UDIV (unsigned divide)
- **Operation:** destination = destination / source, unsigned.
- **Forms:** `$35` regVal reg, `$75` immVal reg, `$B5` regAddr reg, `$F5` immAddr reg.
- **Flags:** C = 0; V = 0; N = sign of result (the high bit of the unsigned quotient at the
  destination width); Z = result is zero; P unaffected.
- **Traps:** divide error (cause 2) on a zero divisor (subcode 0). Unsigned division has no
  quotient-overflow case.

### UMOD (unsigned remainder)
- **Operation:** destination = destination mod source, unsigned.
- **Forms:** `$36` regVal reg, `$76` immVal reg, `$B6` regAddr reg, `$F6` immAddr reg.
- **Flags:** C = 0; V = 0; N; Z; P unaffected.
- **Traps:** divide error (cause 2) on a zero divisor.

### INC
- **Operation:** destination = destination + 1.
- **Forms:** `$31` regVal (register-only; row 0 of base `$31`).
- **Flags:** C = unsigned carry-out; N; V = signed overflow; Z; P unaffected. (Add-family
  flags.)
- **Traps:** none.

### DEC
- **Operation:** destination = destination - 1.
- **Forms:** `$71` regVal (row 1 of base `$31`).
- **Flags:** C = unsigned borrow; N; V = signed overflow; Z; P unaffected. (Sub-family
  flags.)
- **Traps:** none.

### NEG (two's-complement negate)
- **Operation:** destination = 0 - destination.
- **Forms:** `$F1` regVal (row 3 of base `$31`).
- **Flags:** sub-family flags computed as `0 - value`: C is set when the value is non-zero
  (a borrow occurs for any non-zero operand); N = sign of the result; V is set only for the
  width's most-negative value (whose negation overflows); Z = result is zero; P unaffected.
- **Traps:** none.

### ADC (add with carry)
- **Operation:** destination = destination + source + C. The low word of a multi-word add
  runs ADD (which sets C); each higher word runs ADC.
- **Forms:** `$3B` regVal reg, `$7B` immVal reg, `$BB` regAddr reg, `$FB` immAddr reg.
- **Flags:** C = unsigned carry-out of the full `dst + src + C`; N; V = signed overflow; Z
  reflects only the current word (a full-width zero test ANDs the per-word Z results across
  the chain); P unaffected.
- **Traps:** none.

### SBB (subtract with borrow)
- **Operation:** destination = destination - source - C. The low word runs SUB; each higher
  word runs SBB.
- **Forms:** `$3C` regVal reg, `$7C` immVal reg, `$BC` regAddr reg, `$FC` immAddr reg.
- **Flags:** C = unsigned borrow of the full `dst - src - C`; N; V = signed overflow; Z
  reflects only the current word; P unaffected.
- **Traps:** none.

---

## 7.3 Logic

Bitwise operations. All set **C = 0 and V = 0** (cleared), compute N and Z over the
destination width, and **leave P unaffected**. Full four source forms for the binary ops.

### AND
- **Operation:** destination = destination AND source.
- **Forms:** `$08` regVal reg, `$48` immVal reg, `$88` regAddr reg, `$C8` immAddr reg.
- **Flags:** C = 0; V = 0; N = sign of result; Z = result is zero; P unaffected.
- **Traps:** none.

### OR
- **Operation:** destination = destination OR source.
- **Forms:** `$09` regVal reg, `$49` immVal reg, `$89` regAddr reg, `$C9` immAddr reg.
- **Flags:** C = 0; V = 0; N; Z; P unaffected.
- **Traps:** none.

### XOR
- **Operation:** destination = destination XOR source.
- **Forms:** `$0C` regVal reg, `$4C` immVal reg, `$8C` regAddr reg, `$CC` immAddr reg.
- **Flags:** C = 0; V = 0; N; Z; P unaffected.
- **Traps:** none.

### NAND
- **Operation:** destination = NOT (destination AND source).
- **Forms:** `$0B` regVal reg, `$4B` immVal reg, `$8B` regAddr reg, `$CB` immAddr reg.
- **Flags:** C = 0; V = 0; N; Z; P unaffected.
- **Traps:** none.

### NOR
- **Operation:** destination = NOT (destination OR source).
- **Forms:** `$0A` regVal reg, `$4A` immVal reg, `$8A` regAddr reg, `$CA` immAddr reg.
- **Flags:** C = 0; V = 0; N; Z; P unaffected.
- **Traps:** none.

### NOT (one's complement)
- **Operation:** destination = NOT destination (bitwise one's complement).
- **Forms:** `$B1` regVal (register-only; row 2 of base `$31`).
- **Flags:** C = 0; V = 0; N; Z; P unaffected.
- **Traps:** none.

---

## 7.4 Shift

Shift counts are defined for every value (Chapter 10): a count of 0 leaves all flags
unaffected; the over-width behavior is stated per instruction. An out-of-range count never
invokes host undefined behavior. All shifts leave **P unaffected**. Full four source forms.

### SHL (shift left)
- **Operation:** destination = destination << source.
- **Forms:** `$0D` regVal reg, `$4D` immVal reg, `$8D` regAddr reg, `$CD` immAddr reg.
- **Flags:** for count `n` over operand width `bits`: `n == 0` leaves all flags unaffected;
  `1 <= n <= bits` shifts normally with C = the last bit shifted out, N = sign of result, Z,
  and V defined only for `n == 1` (V = the sign bit changed); `n > bits` yields result 0
  with C = 0, N = 0, V = 0, Z = 1. P unaffected throughout.
- **Traps:** none.

### SHR (logical shift right)
- **Operation:** destination = destination >> source (zero-fill).
- **Forms:** `$0E` regVal reg, `$4E` immVal reg, `$8E` regAddr reg, `$CE` immAddr reg.
- **Flags:** `n == 0` leaves flags unaffected; `1 <= n <= bits` shifts with C = last bit
  shifted out, N = sign of result, Z, and V defined only for `n == 1` (V = the prior sign
  bit); `n > bits` yields result 0 with C = N = V = 0, Z = 1. P unaffected.
- **Traps:** none.

### SAR (arithmetic shift right)
- **Operation:** destination = destination >> source with sign replication (the sign bit
  fills the vacated high bits).
- **Forms:** `$2E` regVal reg, `$6E` immVal reg, `$AE` regAddr reg, `$EE` immAddr reg.
- **Flags:** `n == 0` leaves flags unaffected; `1 <= n <= bits-1` shifts right with sign
  replicated, C = the last bit shifted out (bit `n-1` of the operand), **V = 0 always** (an
  arithmetic shift replicates the sign and can never flip it), N = sign, Z; `n >= bits`
  saturates to the sign fill: all-ones (the width's -1) for a negative operand or 0 for a
  non-negative one, with C = the operand's sign bit, N = the operand's sign, V = 0, and Z
  set only for a non-negative operand. P unaffected. This over-width behavior diverges from
  SHR, whose over-width count returns 0.
- **Traps:** none.

---

## 7.5 Compare and test

These compute flags without writing a general register. Full source forms as noted.

### CMP
- **Operation:** set flags by computing destination - source, discarding the difference.
- **Forms:** `$0F` regVal reg, `$4F` immVal reg, `$8F` regAddr reg, `$CF` immAddr reg.
- **Flags:** identical to SUB: C = unsigned borrow, N, V = signed overflow, Z; P unaffected.
  The C borrow convention is what makes JB/JA the correct unsigned branches and V the signed
  ones (Chapter 2 section 2.4).
- **Traps:** none.

### CMPIND (compare indirect)
- **Operation:** set flags by computing (value at the address in the destination register)
  minus the source. Compares against a value in memory.
- **Forms:** `$2F` regVal regAddr; `$6F` immVal regAddr.
- **Flags:** SUB-family: C = unsigned borrow, N, V, Z; P unaffected.
- **Traps:** none.

### TEST
- **Operation:** set flags by computing destination AND source, discarding the result.
- **Forms:** `$10` regVal reg, `$50` immVal reg, `$90` regAddr reg, `$D0` immAddr reg.
- **Flags:** logic-family: C = 0, V = 0, N = sign of the AND, Z = the AND is zero; P
  unaffected.
- **Traps:** none.

### TSTIND (test indirect)
- **Operation:** set flags by ANDing the source with the value at the address in the
  destination register.
- **Forms:** `$30` regVal regAddr; `$70` immVal regAddr.
- **Flags:** logic-family: C = 0, V = 0, N, Z; P unaffected.
- **Traps:** none.

### CMPXCHG (compare-and-swap)
- **Operation:** the frozen compare-and-swap primitive. Three operands
  `CMPXCHG new target expected` (operand1 = new value, operand2 = target, operand3 =
  expected). Compare the target (operand2) against the expected value (operand3) over the
  selected subregister width. **On equality (success):** set Z = 1 and copy the new value
  (operand1) into the target (operand2). **On inequality (failure):** set Z = 0 and copy the
  current target (operand2) into the expected register (operand3), handing the caller the
  observed value for a retry loop.
- **Forms:** `$11` regVal reg reg; `$51` immVal reg reg; `$91` regAddr reg reg; `$D1`
  immAddr reg reg (operand1 varies; target and expected are always registers).
- **Flags:** **Z is the sole and authoritative success indicator** (1 = swapped, 0 = not
  swapped). C, N, V, and P are unaffected: the equality test writes no flag and the copy is
  flag-neutral. A caller tests success with JZ / JNZ.
- **Encoding:** opcode byte, operand1 byte, operand2 byte, operand3 byte, then immediate
  bytes for the immediate forms.
- **Traps:** none. Under single-hart v1.0 the operation is trivially atomic; a conforming
  multi-hart v1.x implementation must keep it an atomic CAS with exactly these effects
  (Chapter 12 section 2).

### FCMP (floating-point compare) -- the only writer of P
- **Operation:** compare the destination floating-point value against the source and set the
  integer flags per the IEEE-754 outcome. Documented fully in Chapter 8; summarized here
  because it is the sole instruction that writes the P flag.
- **Forms:** `$2A` regVal reg, `$6A` immVal reg, `$AA` regAddr reg, `$EA` immAddr reg.
- **Flags:** FCMP computes C, Z, and P together and forces **N = 0 and V = 0**. The mapping
  (x86 UCOMISD convention): `a > src` -> C=0 Z=0 P=0; `a < src` -> C=1 Z=0 P=0; `a == src`
  -> C=0 Z=1 P=0; **unordered (either operand NaN)** -> C=1 Z=1 **P=1**. **P is the
  unordered / NaN indicator and is written only here**; JP and SETP consume it. After FCMP
  the ordered branch idioms JB/JBE/JA/JAE/JZ work directly, and JP/SETP test the unordered
  case (Chapter 8).
- **Traps:** none for arithmetic (FP exceptions are sticky, not trapping); illegal FP
  operand encodings trap cause 0. See Chapter 8.

---

## 7.6 Conditional set (SETcc)

SETcc materializes a condition as 0 or 1 in a destination register. It **reads** the flags
and is **flag-neutral**: it writes no flag (C/N/V/Z/P all unaffected). A bare register
destination writes the full W0 (a clean 0/1, no stale upper bits); an explicit subregister
writes only that field and preserves the rest, exactly like CLR. Each predicate is
identical to the matching Jcc branch (section 7.7), so a SETcc and its branch can never
disagree for the same flag state. Each is a register-only instruction, one register operand.

| Mnemonic | Opcode | Condition | Predicate |
|:---------|:------:|:----------|:----------|
| SETZ | `$2B` | equal / zero | Z == 1 |
| SETNZ | `$2C` | not-equal / nonzero | Z == 0 |
| SETLT | `$2D` | signed < | N != V |
| SETGE | `$AB` | signed >= | N == V |
| SETGT | `$6C` | signed > | Z == 0 and N == V |
| SETLE | `$AC` | signed <= | Z == 1 or N != V |
| SETB | `$6B` | unsigned < | C == 1 |
| SETAE | `$EB` | unsigned >= | C == 0 |
| SETA | `$6D` | unsigned > | C == 0 and Z == 0 |
| SETBE | `$AD` | unsigned <= | C == 1 or Z == 1 |
| SETP | `$EC` | unordered (FCMP NaN) | P == 1 |

- **Traps:** none for the defined encodings. An unallocated condition encoding in the SETcc
  family (for example the reserved `$ED`) is the illegal-instruction trap (cause 0, subcode
  1). SETP (`$EC`) is defined (it claims one of the reserved spare condition encodings and
  reads P); the remaining spare `$ED` stays reserved.

`mazm` also accepts assembler-only synonyms that emit the identical opcode as their
canonical form (byte-identical after assembly): SETE=SETZ, SETNE=SETNZ, SETL/SETNGE=SETLT,
SETNL=SETGE, SETG/SETNLE=SETGT, SETNG=SETLE, SETC/SETNAE=SETB, SETNC/SETNB=SETAE,
SETNBE=SETA, SETNA=SETBE.

---

## 7.7 Control flow

Control-transfer targets are always full 64-bit addresses. Jumps and calls **do not affect
any flag** (C/N/V/Z/P unaffected); the conditional branches *read* flags but write none.

### JMP (unconditional jump)
- **Operation:** set PC to the target and continue. JMP targets the full 64-bit width
  regardless of any operand subregister selection; `mazm` rejects a JMP operand carrying a
  subregister suffix.
- **Forms:** `$16` regVal (address in a register), `$56` immVal (immediate address), `$96`
  regAddr (address pointed to by a register), `$D6` immAddr (address pointed to by an
  immediate).
- **Flags:** C/N/V/Z/P unaffected.
- **Traps:** none.

### Jcc (conditional branches)
- **Operation:** if the condition holds, set PC to the immediate target; else fall through.
  Immediate target only (the register/indirect conditional forms are synthesized as an
  inverted Jcc over JMP). Each shares its predicate with the matching SETcc (section 7.6).
- **Forms and conditions** (each `immVal`):

| Mnemonic | Opcode | Branch taken when |
|:---------|:------:|:------------------|
| JZ | `$17` | Z == 1 (equal / zero) |
| JNZ | `$18` | Z == 0 |
| JLT | `$19` | N != V (signed <) |
| JGE | `$97` | N == V (signed >=) |
| JGT | `$58` | Z == 0 and N == V (signed >) |
| JLE | `$98` | Z == 1 or N != V (signed <=) |
| JB | `$57` | C == 1 (unsigned <) |
| JAE | `$D7` | C == 0 (unsigned >=) |
| JA | `$59` | C == 0 and Z == 0 (unsigned >) |
| JBE | `$99` | C == 1 or Z == 1 (unsigned <=) |
| JP | `$D8` | P == 1 (FCMP unordered / NaN) |

- **Flags:** read, none written. C/N/V/Z/P unaffected.
- **Encoding:** opcode byte, immediate-size source operand byte, immediate target bytes.
- **Traps:** none for the defined encodings. An unallocated condition encoding in the Jcc
  family (for example the reserved `$D9`) is the illegal-instruction trap (cause 0, subcode
  1). JP (`$D8`) is defined and reads P; `$D9` stays reserved.

### CALL
- **Operation:** push the return address (the address of the following instruction) onto the
  stack (pre-decrementing RS), then jump to the target. Execution continues until a RET.
- **Forms:** `$1D` regVal, `$5D` immVal, `$9D` regAddr, `$DD` immAddr.
- **Flags:** C/N/V/Z/P unaffected.
- **Traps:** none.

### RET
- **Operation:** pop the return address from the stack into PC and continue there
  (incrementing RS). Returns from CALL.
- **Forms:** `$27` (zero-operand; row 0 of base `$27`).
- **Flags:** C/N/V/Z/P unaffected.
- **Traps:** none.

### JP / SETP
JP (`$D8`) is listed in the Jcc table above and SETP (`$EC`) in the SETcc table; both read
the P flag set by FCMP (Chapter 8). JNP / SETNP are synthesized by branch inversion (no
dedicated opcode).

---

## 7.8 Stack

The stack is **full-descending** with **pre-decrement** on push. RS (SP) always points at
the last value pushed. The stack instructions are PUSH and POP (section 7.1) and CALL and
RET (section 7.7):

- **PUSH src:** RS -= size(src); write src at RS.
- **POP dst:** read dst from RS; RS += size(dst).
- **CALL target:** RS -= 8; write the return address at RS; PC = target.
- **RET:** PC = value at RS; RS += 8.

Return addresses are 8-byte (full 64-bit). At process start RS points at the process-start
block's argc slot; the first push moves RS just below the block (Chapter 4 section 4.5).
There is no stack-limit check in the v1.0 flat model; a stack-fault trap (cause 6) is
reserved for the future segment/bounds work (Chapter 12).

---

## 7.9 System and privileged instructions

Instructions marked **(privileged)** require the RF privilege bit set. Executed in user mode
(privilege bit clear) they raise the **privileged-operation fault (cause 4)**. The privilege
bit exists and the machine starts privileged; the enforcement gate is a reserved
mechanism whose code lands with the interrupt/port work (Chapters 10, 11). The candidate
privileged set is IN / OUT / OUTR, SETINT / CLRINT, IRET, HALT, INT, and future
control-register and segment writes.

### SYS (system call)
- **Operation:** enter the kernel syscall dispatcher with the syscall index in the operand.
  A deliberate synchronous software trap; trap-class (captures the following-instruction PC,
  so IRET resumes after SYS). The syscall number and arguments travel in registers per the
  syscall ABI (Appendix C). Today the reference VM dispatches SYS directly to the
  BIOS/syscall surface; routing it through the shared trap table at vector 7 is the future
  path (Chapter 10). Privileged.
- **Forms:** `$34` regVal (index in a register), `$74` immVal (immediate index). The index
  is a single byte (`operand.b0`).
- **Flags:** the instruction itself sets no arithmetic/logic flag (a syscall's effects on RV
  and errno are the ABI's, not a flag effect). C/N/V/Z/P unaffected by the dispatch.
- **Traps:** reserved as **cause 7** (SYS / syscall entry); privileged-operation fault
  (cause 4) if executed in user mode once enforcement lands.

### INT (software interrupt)
- **Operation:** push FL and PC and raise a software interrupt at the given vector index.
  Privileged. The dispatch is deferred until the vector-table format exists (co-authored
  with the interrupt card); INT has no active dispatch case in v1.0.
- **Forms:** `$24` regVal (index in a register), `$64` immVal (immediate index).
- **Flags:** C/N/V/Z/P unaffected.
- **Traps:** privileged-operation fault (cause 4) in user mode once enforced.

### IRET (interrupt return)
- **Operation:** pop FL and PC from the stack and continue at PC. The shared return path for
  both traps and interrupts (there is no TRET). Restores the pre-interrupt RF, including the
  interrupt-enable bit, so a normal handler return re-enables interrupts. Privileged.
- **Forms:** `$67` (zero-operand; row 1 of base `$27`).
- **Flags:** RF (and thus C/N/V/Z/P) is **restored** from the saved frame, not computed.
- **Traps:** privileged-operation fault (cause 4) in user mode once enforced.

### SETINT / CLRINT
- **Operation:** SETINT sets the RF interrupt-enable bit (enabling maskable external
  interrupts); CLRINT clears it. Privileged.
- **Forms:** SETINT `$29` (row 0 of base `$29`); CLRINT `$69` (row 1). Zero-operand.
- **Flags:** the arithmetic/logic flags C/N/V/Z/P are unaffected (these write RF.H1).
- **Traps:** privileged-operation fault (cause 4) in user mode once enforced.

### SETCRY / CLRCRY
- **Operation:** SETCRY sets the Carry flag to 1; CLRCRY clears it to 0. The direct
  software controls over C used to seed a multi-word ADC/SBB chain.
- **Forms:** SETCRY `$A9` (row 2 of base `$29`); CLRCRY `$E9` (row 3). Zero-operand.
- **Flags:** C = 1 (SETCRY) or C = 0 (CLRCRY); N, V, Z, P unaffected.
- **Traps:** none. (Not privileged; C is a user-mode arithmetic flag.)

### IN (port input, privileged)
- **Operation:** read a value from the device on the selected port into the destination
  register. The port id is the low 16 bits (`.q0`) of the port operand. An IN from an
  unpopulated port yields 0 (Chapter 11).
- **Forms:** `$1F` regVal reg, `$5F` immVal reg, `$9F` regAddr reg, `$DF` immAddr reg (the
  first operand names the port; the destination is always a register).
- **Flags:** C/N/V/Z/P unaffected.
- **Traps:** privileged-operation fault (cause 4) in user mode (enforcement reserved,
  Chapter 11).

### OUT (port output, immediate port, privileged)
- **Operation:** write the source value (register / immediate / value-at-address) to the
  device on the port named by the trailing immediate. Writing to an unpopulated port is
  discarded (Chapter 11).
- **Forms:** `$14` regVal imm, `$54` immVal imm, `$94` regAddr imm, `$D4` immAddr imm.
- **Flags:** C/N/V/Z/P unaffected.
- **Traps:** privileged-operation fault (cause 4) in user mode (enforcement reserved).

### OUTR (port output, register port, privileged)
- **Operation:** OUT with the port named by a register operand rather than an immediate. The
  port id is the port register's `.q0`.
- **Forms:** `$1E` regVal reg, `$5E` immVal reg, `$9E` regAddr reg, `$DE` immAddr reg.
- **Flags:** C/N/V/Z/P unaffected.
- **Traps:** privileged-operation fault (cause 4) in user mode (enforcement reserved).

### HALT
- **Operation:** halt the clock, stopping execution pending an interrupt. With no interrupt
  source a halted core has nothing to wake it, so the run loop returns and the host process
  exits 0 with no recorded status (distinct from the status-carrying SYS $3C exit; Appendix
  C). Pinned at opcode `$00` so a run of zeroed memory halts. Privileged.
- **Forms:** `$00` (zero-operand).
- **Flags:** C/N/V/Z/P unaffected (the running bit in RF.H1 clears).
- **Traps:** privileged-operation fault (cause 4) in user mode once enforced.

### BRK (breakpoint)
- **Operation:** a defined breakpoint trap. Trap-class: captures the following-instruction
  PC, so a debugger that resumes lands after the BRK. Pinned at `$FF`, the value that fills
  erased/uninitialized memory, so a run of `$FF` bytes reached as code raises a breakpoint
  rather than wandering, mirroring HALT at `$00`.
- **Forms:** `$FF` (zero-operand).
- **Flags:** C/N/V/Z/P unaffected.
- **Traps:** **breakpoint (cause 3)**. Unhandled, the machine halts deterministically with
  cause 3 surfaced and the following instruction does not execute.

### NOP
- **Operation:** no operation; an instruction placeholder.
- **Forms:** `$A7` (zero-operand; row 2 of base `$27`).
- **Flags:** C/N/V/Z/P unaffected.
- **Traps:** none.

---

## 7.10 Floating-point (cross-reference to Chapter 8)

The floating-point instructions are documented in Chapter 8 (Zfinx, FCSR, NaN/rounding, the
FCMP flag mapping): the arithmetic ops **FADD / FSUB / FMUL / FDIV**; the unary ops **FSQRT
/ FNEG / FABS**; the fused multiply ops **FMADD / FMSUB**; the min/max ops **FMIN / FMAX**;
the compare **FCMP** (the sole writer of the P flag, summarized in section 7.5); the
conversions **FCVTFF / FCVTFS / FCVTFU / FCVTSF / FCVTUF**; the FCSR-access ops **FGETCSR /
FSETCSR**; and the unordered predicates **JP / SETP** (listed in sections 7.7 and 7.6). FP
arithmetic exceptions are sticky in the FCSR FFLAGS and never trap; only illegal FP
encodings/operands trap (cause 0). The integer flags C/N/V/Z are untouched by every FP
instruction except FCMP, and P is written only by FCMP.

## Sourcing

- Opcode map and forms: README "Instructions" and "Opcodes Sorted Numerically"; Appendix A;
  `src/maize_cpu.h` `instr` namespace (lines ~442-843).
- Flag effects: README "Per-instruction flag effects" table and its Notes; `src/cpu.cpp`
  `run_alu` and its width-case blocks (ADD/SUB/ADC/SBB/MUL/MULW/UMULW/DIV/MOD/shift/logic,
  lines ~1022 onward), `do_fcmp` for P (lines ~1001-1019). The zero-flag-only CMPXCHG
  contract is `docs/spec/reservations.md` section 2 and the reference dispatch.
- Condition predicates: `src/cpu.cpp` `eval_condition` (lines ~781-800), shared by Jcc and
  SETcc; README SETcc/Jcc tables.
- Trap behavior: `docs/spec/trap-model.md` (Chapter 10); README "Trap Model". Divide error:
  `raise_divide_error` (cpu.cpp ~921). Breakpoint: `raise_breakpoint` (~935). Privileged and
  port I/O: `docs/spec/device-surface.md` (Chapter 11).
