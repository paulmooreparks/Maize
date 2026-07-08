# Maize QBE back-end: instruction-selection coverage

What the QBE Maize target supports **as of maize-62** (partial toward maize-11 AC
6400; the full instruction-selection matrix is completed under maize-63). This card
is scoped tightly to the QBE IL that a C hello world reaches; everything outside
that set `err()`s (in isel/abi) or `die()`s (in emit) so maize-63 adds it
deliberately rather than inheriting a silent bug.

## The hello-world QBE IL

`ctest/hello.c` compiles (via `cproc-qbe`) to:

```
data $.Lstring.2 = align 1 { b "Hello, world!\000", }
export function w $main() {
@start.1
@body.2
	%.1 =w call $puts(l $.Lstring.2)
	ret 0
}
```

(The pinned cproc emits `call extern $puts`; the pinned qbe predates that
call-linkage keyword, so the pipeline normalizes `call extern` -> `call`. In the
single-TU whole-program model this annotation is meaningless -- see
`scripts/run-ctest.sh` and the card's decision on the cproc/qbe IL version skew.)

## Supported ops (this card)

| QBE IL | Maize lowering | Notes |
|--------|----------------|-------|
| `data { b "..." }` | labelled `DATA $HH ...` byte list | one label per symbol; strings decoded from QBE's gas-escaped form to raw bytes (`data.c`) |
| global address (`$sym`) | `CP <label> Rn`, Rn = whole register (W0) | full-64 materialization, maize-11 decision 6415 (see below) |
| integer/pointer arg | `CP <src> Rk` into `R0..R9` | ABI arg registers |
| `call $sym` | `CALL <label>` | direct call; register-indirect `CALL Rn` also emitted when the target is a register |
| `ret <wconst>` | `CP $<imm> RV` | constant into the return register |
| function entry/exit | `PUSH BP; CP SP BP; [SUB fs SP] ... CP BP SP; POP BP; RET` | prologue/epilogue, see CALLING-CONVENTION.md |
| `copy` (reg/const) | `CP` | constants pass straight through as immediates (Maize is CISC) |

## Recorded idioms

### `w`-representation idiom (maize-11 decision 6406)

QBE `w` (32-bit) values are held in the register's `H0` sub-register; Maize does
native 32-bit ALU ops at sub-register width, and the maize-29 `CP`/`CPZ`
sign/zero-extension rules widen to `l`. Hello world reaches only the `w` **constant
`0`** (main's return), materialized as `CP $00000000 RV` -- a whole-register write
of a zero-extended 32-bit immediate. Non-trivial `w` arithmetic (the general `H0`
convention) is maize-63 surface.

### Full-64 address-materialization idiom (maize-11 decision 6415)

Global/label addresses are materialized into the **whole destination register**
(`CP <label> Rn`, Rn = W0), never the `.H0`-truncated `CP <label> Rn.H0` shortcut
`asm/hello.mazm` uses. Verifiable in the emitted body: the string address loads as
`CP _Lstring_2 R0` (R0, not R0.H0).

Assembler-capability note (resolves maize-62 open question 6640): `mazm`'s label
table is 32-bit (`u_hword`), so a label reference is encoded as a **32-bit immediate
field** in the flat `.bin` path (`compile_label` / `write_label` in `src/mazm.cpp`);
`mazm` does not emit a 64-bit-wide immediate field for a label. Decision 6415's
requirement is nevertheless satisfied **without any assembler change**: because the
destination is the whole 64-bit register, the sign-extending `CP` widens the 32-bit
label value across all 64 bits (`copy_memval_reg` in `src/cpu.cpp`), yielding a
clean full-64 address for any label below 2^31 (sign-ext == zero-ext there). This is
materially different from -- and correct versus -- the `.H0` shortcut, which writes
only the low 32-bit sub-register and leaves the upper 32 bits of the register
undefined. The distinction that decision 6415 cares about (whole-register
destination vs. sub-register write) holds; only the immediate **encoding width** is
32-bit, an inherent `mazm` property, not a truncation. (A `CPZ` zero-extending form
would be the correct choice for label values >= 2^31; that is maize-63 territory and
not reached by hello world.)

## Explicitly deferred to maize-63 (err()/die() today)

Signed/unsigned `div`/`rem`; bitwise `and`/`or`/`xor`; shifts `shl`/`shr`/`sar`;
width-correct sign/zero-extending loads and stores (`loadsb`..`loaduw`,
`storeb`..`storel`); comparisons materialized as 0/1 values and conditional
branches; `alloc` and local stack addressing; multi-level argument passing / stack
overflow past R9; aggregate (struct) arguments and returns; varargs; environment
calls; floating point (`s`/`d`, out of scope for the whole workstream, maize-11
decision 6413); and address references embedded in initialized data.
