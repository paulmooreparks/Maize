# Chapter 8: Floating-Point

This chapter is normative. It fixes the floating-point model: the Zfinx placement of FP
values in the integer registers, the formats, the FCSR, the operation set, the FCMP flag
mapping consumed by JP / SETP, conversions, NaN and rounding behavior, and the sticky
(never-trapping) exception model.

Maize implements IEEE-754-2019 **binary32** (single) and **binary64** (double): both
widths, all five rounding modes, all five sticky exception flags, subnormals (gradual
underflow, no flush-to-zero), and a fused multiply-add. Semantics track the RISC-V F/D
extensions closely, with one deliberate divergence named in section 8.7 (a NaN
float-to-integer conversion yields 0 rather than the RISC-V maximum value).

## 8.1 Zfinx: floats live in the integer registers

Floating-point instructions read operands from and write results to the existing sixteen
integer registers. There is **no separate FP register bank** and no FP load/store/move (LD /
ST / CP already move the bits). Format width comes from the per-operand **subregister
field**, exactly as for integer ops:

- **binary32** occupies a 32-bit subregister view: `H0` or `H1`.
- **binary64** occupies the full 64-bit register: `W0`.

A `B*` or `Q*` subregister on a floating-point operand is illegal: `mazm` rejects it at
assemble time, and the VM raises a deterministic illegal-operand trap (cause 0). There is
**no NaN-boxing**: a binary32 value simply occupies a 32-bit subregister like any 32-bit
integer, and the upper bits follow the ordinary subregister merge semantics (Chapter 3).

The float operands of a same-format op (FADD / FSUB / FMUL / FDIV / FCMP, FSQRT / FNEG /
FABS, FMIN / FMAX, and FMADD / FMSUB) must all be the **same width**: mixing a binary32 and
a binary64 operand (for example `FADD R0.H0 R1.W0`) is a deterministic illegal-operand trap,
and `mazm` rejects it at assemble time. An FP immediate source must likewise be exactly the
destination float width (4 bytes for binary32, 8 for binary64). The conversion ops (FCVTFF
and the integer/float FCVT* family) are the sole exception, since a conversion names two
operands of intentionally different width.

## 8.2 FCSR: rounding mode and sticky exception flags

A dedicated architectural **FP control/status register (FCSR)** holds the rounding mode and
the sticky exception flags, keeping the hot per-instruction integer flags C/N/V/Z/P
uncontaminated. It is **not** one of the sixteen operand-addressable registers; software
reads and writes it with `FGETCSR reg` (`$15`) and `FSETCSR reg` (`$55`). The byte layout is
RISC-V's `fcsr` verbatim:

    Bits 7-5   FRM     rounding mode (3 bits)
    Bits 4-0   FFLAGS  sticky exception flags (5 bits)

`FSETCSR` writes the low 8 bits and thereby also clears the sticky flags (flags are set by
hardware, cleared only by software). Reset default is `$00` (RNE, no flags). The upper bits
are reserved for a future trap-enable extension and are not implemented.

### FRM (rounding mode), RISC-V encoding

    000  RNE  round to nearest, ties to even (default)
    001  RTZ  round toward zero
    010  RDN  round toward -infinity
    011  RUP  round toward +infinity
    100  RMM  round to nearest, ties to max magnitude
    101, 110  reserved
    111  DYN  not supported (Maize has no per-instruction rounding field)

Every rounding op consults the current FRM; there is no per-instruction rounding field. A
rounding op executed while FRM holds a reserved encoding (`101` / `110`) or the unsupported
`DYN` (`111`) is a deterministic illegal-operand trap (cause 0), not a silent
round-to-nearest, matching RISC-V's treatment of a reserved static rounding mode. The
non-rounding ops (FNEG, FABS, FMIN, FMAX, FCMP) never consult FRM and are unaffected.

### FFLAGS (sticky exception flags), RISC-V bit order

    bit 4  NV  invalid operation (sNaN operand, 0*inf, inf-inf, 0/0, inf/inf,
               sqrt of a negative, invalid float->int conversion)
    bit 3  DZ  divide by zero (finite nonzero / 0)
    bit 2  OF  overflow (rounded result exceeds the largest finite value)
    bit 1  UF  underflow (a tiny nonzero result)
    bit 0  NX  inexact (the result is not exactly representable)

These are the 754 arithmetic exceptions and are distinct from the integer RF flags; in
particular FFLAGS.OF (binary overflow to infinity) is not RF.V (integer signed overflow).

## 8.3 Exceptions are sticky, not trapping

FP arithmetic exceptions do **not** trap. A divide by zero yields the correctly signed
infinity and sets DZ; an invalid operation yields the canonical qNaN and sets NV; the
operation always produces its 754-defined result. Only **illegal encodings** trap (a B\* /
Q\* subregister on an FP operand, an operand-width mismatch, a reserved/unsupported FCSR
rounding mode on a rounding op, or a reserved/unallocated FP opcode form), as part of the
illegal-instruction / illegal-operand taxonomy (cause 0; see the Trap Model chapter).
Trapping FP (754 alternate exception handling) is a reserved future extension in the unused
FCSR bits.

## 8.4 NaN handling

A signaling NaN has the significand MSB clear; a quiet NaN has it set. A signaling-NaN
operand to any arithmetic op, to FMIN / FMAX, or to FCMP raises NV; a quiet NaN does not.
Every NaN-producing arithmetic op returns the **canonical qNaN** (binary32 `$7FC00000`,
binary64 `$7FF8000000000000`; positive, quiet, zero payload) rather than preserving an input
payload, matching RISC-V. FNEG and FABS are the sole exceptions: they manipulate the sign bit
only, raise no flags, do not round, and pass NaN payloads through unchanged.

## 8.5 FCMP and the float branch idioms

`FCMP src, a` compares the destination register `a` against the source `src` and writes the
integer flags, mapping the four 754 outcomes onto the x86 `UCOMISD` convention. It is the
**only instruction that writes the P (parity / unordered) flag**, and it forces N = V = 0:

    Outcome              C (bit0)  Z (bit4)  P (bit3)  N,V
    a > src (ordered)    0         0         0         0,0
    a < src (ordered)    1         0         0         0,0
    a == src             0         1         0         0,0
    unordered (a NaN)    1         1         1         0,0

FCMP is the **quiet** compare: a quiet-NaN operand yields unordered without signaling; only
a signaling NaN raises NV. After FCMP the ordered branch idioms work directly off the reused
predicate table (Chapter 7 sections 7.6 and 7.7):

    JB  / SETB   a < src        JA  / SETA   a > src
    JBE          a <= src       JAE          a >= src
    JZ  / SETZ   a == src       JP  / SETP   unordered (either operand NaN)

`JP` / `SETP` is the unordered predicate, reading the P flag; `JNP` / `SETNP` are synthesized
by branch inversion (there is no separate opcode). No integer instruction writes P, so the
P flag survives across integer work between an FCMP and its JP / SETP exactly as C/N/V/Z do.

## 8.6 Operations

- **Arithmetic** (`FADD` `$1A`, `FSUB` `$1B`, `FMUL` `$1C`, `FDIV` `$21`): correctly rounded
  under the current FRM, full four addressing-mode source forms like the integer ALU.
  `FADD src dst` computes `dst = dst + src`.
- **Unary** (`FSQRT` `$22`, `FNEG` `$62`, `FABS` `$A2`): register-only, `MNEMONIC src dst`
  computing `dst = f(src)`. FSQRT is correctly rounded; FNEG / FABS are exact sign-bit ops
  that raise no flags and do not round.
- **Fused multiply-add** (`FMADD`, `FMSUB`): three operands, single-rounded
  `dst = a*b (+/-) c` where the third operand is both the addend `c` and the destination.
  Like the wide-multiply family, operand 1 has all four addressing-mode source forms:
  FMADD `$23` regVal, `$63` immVal, `$A3` regAddr, `$E3` immAddr; FMSUB `$25` regVal, `$65`
  immVal, `$A5` regAddr, `$E5` immAddr (operands 2 and 3 are always registers). `FNMADD` =
  `FNEG(FMADD(...))` and `FNMSUB` = `FNEG(FMSUB(...))` are synthesized via the exact FNEG (no
  dedicated opcode), so they remain single-rounded.
- **Min / max** (`FMIN` `$33`, `FMAX` `$73`): RISC-V semantics. A quiet-NaN operand returns
  the non-NaN operand; both-NaN returns the canonical qNaN; a signaling NaN raises NV;
  `-0 < +0`.
- **Compare** (`FCMP` `$2A`): the quiet compare of section 8.5.
- **Conversions**: `FCVTFF` `$39` (float to float, widths from the two subregisters);
  `FCVTFS` `$79` / `FCVTFU` `$B9` (float to signed / unsigned integer, saturating);
  `FCVTSF` `$3A` / `FCVTUF` `$7A` (signed / unsigned integer to float). All round per FRM.
- **FCSR access**: `FGETCSR` `$15` copies FCSR into the operand register; `FSETCSR` `$55`
  copies the operand register's low 8 bits into FCSR and clears the sticky FFLAGS.

## 8.7 Conversions in detail

The float-to-integer conversions (`FCVTFS`, `FCVTFU`) are **saturating**: overflow yields
the maximum representable integer, underflow the minimum, both setting NV. Note one
**deliberate divergence from RISC-V**: a **NaN** input to a float-to-integer conversion
yields **0** here (matching the common C-cast convention), whereas RISC-V yields the
maximum-magnitude value; both set NV. This is the only point where Maize's float-to-int
conversion departs from the RISC-V F/D behavior it otherwise mirrors. The integer-to-float
conversions (`FCVTSF`, `FCVTUF`) round the integer to the target float width per FRM.

## 8.8 Synthesizing FCLASS / copysign (no dedicated opcode)

Maize spends no opcode on FCLASS or FSGNJ (copysign); under Zfinx these are integer
bit-operations on the register that already holds the float:

- **isnan(x)**: `FCMP x, x` then `JP` (a value compares unordered with itself iff it is NaN).
- **copysign(x, y)**: clear x's sign with `AND` against `$7FFF...`, extract y's sign with
  `AND` against `$8000...`, then `OR` the two.
- **isinf / isfinite / fpclassify**: mask the exponent and mantissa fields with `AND` and
  compare the bit patterns; the exponent-all-ones / mantissa-zero test distinguishes infinity
  from NaN.

## 8.9 Rounding and portability notes (informative)

The reference VM computes on the host FPU (IEEE-754 conformant), setting the hardware
rounding direction from FRM and reading the hardware exception flags into FFLAGS, with
`std::fma` for the fused multiply-add. The four directed rounding modes (RNE / RTZ / RDN /
RUP) map onto hardware directions and are correctly rounded by the host FPU. RMM
(ties-to-max-magnitude), which no hardware direction provides, is synthesized by computing
the operation in a wider host type and rounding to the target width with an explicit
ties-away rule; its flags come from the equivalent nearest evaluation, which shares RMM's
exception conditions and overflow threshold.

RMM carries a small set of documented limitations, all confined to exact-tie behavior and
none affecting the four hardware rounding modes:

- The wider-type path is exact for binary32 add/sub/mul (double is exact) and for binary64
  add/sub/mul on a host with an 80-bit `long double`. The RMM tie decision for div, sqrt, and
  FMA inherits a benign double-rounding edge (the wider result is itself a rounding), which a
  focused conformance fixture does not exercise.
- A binary64 FMA under RMM at an exact tie falls back to the nearest-even value, because
  re-rounding a 106-bit product exceeds the 80-bit host `long double`.

RMM's directed sibling modes and every mode of the other ops are unaffected. Because RMM's
tie edge depends on the host `long double` width, a portability-sensitive program that
requires bit-identical RMM ties across hosts should avoid RMM for div / sqrt / FMA; the four
directed modes and nearest-even are host-independent.

## 8.10 Floating-point and the C ABI

Under Zfinx, floating-point arguments and results share the integer registers: a `float`
occupies the low 32 bits (`H0`) of its register and a `double` the full 64 bits (`W0`).
There is no separate FP argument class. See Appendix C and
`toolchain/qbe-maize/CALLING-CONVENTION.md`.

## Sourcing

- Zfinx placement, formats, and the same-width rule: the reference VM FP dispatch and
  `fp_width_from_subreg` in `src/cpu.cpp`; `src/maize_cpu.h` FP opcode constants; README
  "Floating-Point (IEEE-754)".
- FCSR layout, FRM check, and sticky FFLAGS: `src/cpu.cpp` `fcsr_frm` / `fp_checked_frm` /
  `fcsr_raise`; README "FCSR" section.
- FCMP flag mapping (C/Z/P, N=V=0) and the P flag: `src/cpu.cpp` `do_fcmp` and
  `eval_condition` case 10 (JP/SETP); README "FCMP and the float branch idioms".
- NaN, rounding, conversions, and the RMM notes: `src/fpu.*` and README "NaN handling",
  "Operations", "Implementation note (RMM)"; illegal-FP encodings trap via `raise_illegal_fp`
  (Trap Model chapter, cause 0).
