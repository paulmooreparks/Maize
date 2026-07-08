# Maize QBE back-end: instruction-selection coverage

The QBE Maize target covers the full instruction-selection matrix reached by a
nontrivial single-TU C program (as of maize-63): control flow, the comparison
family both as branch predicates and as materialized 0/1 values, arithmetic /
logic / shift / signed+unsigned div/mod, sign/zero-extending sub-word loads and
stores, explicit width casts, and frame-slot addressing. Anything genuinely
outside that set (see **Still out of scope**) still `err()`s in `isel`/`abi` or
`die()`s in `emit`, so an unsupported construct surfaces rather than
miscompiling silently.

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
call-linkage keyword, so the pipeline normalizes `call extern` -> `call`. The
same pinned qbe also predates cproc's `neg` unary op, so the pipeline normalizes
`=<w|l> neg X` -> `=<w|l> sub 0, X` (the identity lowering). In the single-TU
whole-program model these annotations are meaningless -- see
`scripts/run-ctest.sh` and the card's decision on the cproc/qbe IL version skew.)

## Supported ops

| QBE IL | Maize lowering | Notes |
|--------|----------------|-------|
| `data { b "..." }` | labelled `DATA $HH ...` byte list | one label per symbol; strings decoded from QBE's gas-escaped form to raw bytes (`data.c`) |
| global address (`$sym`) | `CP <label> Rn`, Rn = whole register (W0) | full-64 materialization, maize-11 decision 6415 (see below) |
| integer/pointer arg | `CP <src> Rk` into `R0..R9` | ABI arg registers |
| `call $sym` / `call %r` | `CALL <label>` / `CALL Rn` | direct or register-indirect |
| `ret <val>` | `CP <val> RV` then epilogue | scalar return via RV |
| function entry/exit | `PUSH BP; CP SP BP; [SUB fs SP] ... CP BP SP; POP BP; RET` | prologue/epilogue, see CALLING-CONVENTION.md |
| `copy` (reg/const) | `CP` at class width | constants pass straight through as immediates (Maize is CISC) |
| `add`/`sub`/`mul` | `ADD`/`SUB`/`MUL <arg1> <to>` | two-address dst-accumulate (below); `sub` non-commutative |
| `and`/`or`/`xor` | `AND`/`OR`/`XOR <arg1> <to>` | commutative dst-accumulate |
| `shl`/`shr`/`sar` | `SHL`/`SHR`/`SAR <count> <to>` | `shr` logical, `sar` arithmetic (maize-54); count printed at `w` width |
| `div`/`rem` | `DIV`/`MOD <arg1> <to>` | signed, truncate toward zero (maize-5 / decision 6781) |
| `udiv`/`urem` | `UDIV`/`UMOD <arg1> <to>` | unsigned |
| `ceq*`/`cne*`/`cs{lt,le,gt,ge}*`/`cu{lt,le,gt,ge}*` | `CMP <arg1> <arg0>` + `Jcc` (branch) or `SETcc <to>` (value) | operand order + cond table below |
| conditional branch (`jnz`) | fused compare -> `CMP <arg1> <arg0>` + `<Jcc> Lm<s1>`; non-flag value -> `CMP $00 <reg>` + `JNZ`; then fall-through or `JMP Lm<s2>` | decisions 6777/6778/6782 |
| `load` | `LD @a <dst>` | full-width load |
| `loadsb`/`sh`/`sw` | `LD @a <dst.sub>` + `CP <dst.sub> <dst>` | sign-extend (no LDZ) |
| `loadub`/`uh`/`uw` | `LD @a <dst.sub>` + `CPZ <dst.sub> <dst>` | zero-extend |
| `extsb`/`sh`/`sw` | `CP <arg0.sub> <dst>` | sign-extend cast, no load |
| `extub`/`uh`/`uw` | `CPZ <arg0.sub> <dst>` | zero-extend cast, no load |
| `storeb`/`h`/`w`/`l` | `ST <src at width> @a` | store at the op's width |
| `alloc4`/`8`/`16` | prologue `SUB $<frame> SP` reservation | constant size only |
| frame-slot `addr` (`fixarg`) | `LEA $-<off> BP <reg>` | BP-relative, below the saved-register block |

### Sub-register width convention (`w`-representation, maize-11 decision 6406)

A QBE `w` (32-bit) value lives in a register's `H0` sub-register and its ALU/CMP
ops run at 32-bit width (`R0.H0`); a QBE `l` (64-bit) value occupies the whole
register (`R0`). `emit.c:clssz()` maps each operand's class to its mazm
sub-register suffix, so every operand is printed at exactly its class width. The
sub-word field sizes used by loads/stores/extensions are `.B0` (8-bit), `.Q0`
(16-bit), and `.H0` (32-bit), matching the README register map.

### Two-address dst-accumulate reconciliation (decision 6780)

Maize ALU/DIV ops are two-address (`OP src dst` => `dst = dst OP src`), so a
3-address QBE op `to = arg0 OP arg1` is reconciled by establishing `to == arg0`
(copy `arg0 -> to` when they differ) and then `OP arg1 to`. When `arg1` already
aliases `to` for a non-commutative op (`sub`/shifts/`div`/`mod`), the value is
computed in the `RT` scratch (`CP arg0 RT; OP arg1 RT; CP RT to`) so `arg0` is
not clobbered before `arg1` is read. `RT` is globally reserved (`rglob`) and is
never register-allocated, so it is always free as back-end scratch. The `±1`
add/sub `INC`/`DEC` peephole is intentionally not taken (uniform `ADD`/`SUB`).

### Condition -> Jcc / SETcc mapping (decision 6776)

Reuses the single predicate table shared by `Jcc` and `SETcc` in
`src/maize_cpu.h`:

| QBE cond | branch | value | | QBE cond | branch | value |
|----------|--------|-------|-|----------|--------|-------|
| ieq  | JZ  | SETZ  | | iuge | JAE | SETAE |
| ine  | JNZ | SETNZ | | iugt | JA  | SETA  |
| islt | JLT | SETLT | | iule | JBE | SETBE |
| isle | JLE | SETLE | | iult | JB  | SETB  |
| isgt | JGT | SETGT | |      |     |       |
| isge | JGE | SETGE | |      |     |       |

### Full-64 address-materialization idiom (maize-11 decision 6415)

Global/label addresses are materialized into the **whole destination register**
(`CP <label> Rn`, Rn = W0), never the `.H0`-truncated shortcut. `mazm`'s label
table is 32-bit, so the immediate field is encoded 32-bit (`R_MAIZE_ABS32` in the
hand-assembly convenience forms, `R_MAIZE_ABS64` here) and the sign-extending
`CP` into the whole register widens a label value below 2^31 cleanly across all
64 bits. The distinction decision 6415 cares about -- whole-register destination
vs. sub-register write -- holds; only the immediate encoding width is an inherent
`mazm` property, not a truncation.

## ISA-ambiguity log

Every lowering point where more than one lowering was plausible, the chosen one,
and why. These are the durable prose home for the `decision` checklist items
recorded on maize-63 (6775-6790).

- **CMP operand order (6775).** `CMP src dst` computes `dst - src`; QBE evaluates
  `cmp<cc> arg0,arg1` as `arg0 <cc> arg1`. Lower to `CMP arg1 arg0` (dst=arg0,
  src=arg1 => arg0 - arg1) so the flags match QBE's sense. When `arg0` is a
  constant and `arg1` a register, swap operands and swap the condition
  (`cmpop`), because the constant must sit in the `src` position (the CMP dst is
  a register).
- **Cond -> Jcc/SETcc mapping (6776).** The back-end cond table is exactly the
  shared `src/maize_cpu.h` predicate table (cond_row / jcc_base / setcc_base), so
  a branch and its branchless `SETcc` counterpart can never disagree for the same
  flag state. Chosen over an independent table to make divergence impossible.
- **Fuse vs. materialize (6777).** A comparison consumed only by a `jnz` folds
  into `CMP arg1 arg0` + `Jjf<cc>` with no register materialized (mirrors
  `qbe/amd64/isel.c:seljmp`); otherwise it materializes with `CMP arg1 arg0` +
  `SETcc<cc>` into the destination. Chosen to avoid a dead 0/1 register when the
  only consumer is the branch.
- **Branch on a non-flag value (6778).** Data movement (`CP`/`LD`/`ST`) does not
  set flags, so `if (x)` / `while (str[len])` emit an explicit `CMP $00 <reg>`
  before `JNZ`/`JZ` -- the `CMP $00 R.B0; JZ` idiom in `toolchain/rt/puts.mazm`.
  Chosen over assuming flags are live from a prior op.
- **Jcc target is a label (6782).** Post-maize-64 `Jcc` encodes an immediate
  (label) target only. Block edges emit `<Jcc> Lm<s1>` for the taken edge, then
  fall through to `s2` when it is the next block in layout, else `JMP Lm<s2>`
  (`JMP` is a full-64-bit target). Every non-entry block is labelled `Lm<id>` so
  a single-predecessor taken edge is always addressable.
- **Sub-word extension via CP/CPZ (6779).** There is no `LDZ`. A load reads
  exactly the destination sub-register width; sign extension is `CP dst.sub dst`
  and zero extension is `CPZ dst.sub dst`. Explicit `Oext*` casts do the same
  `CP`/`CPZ` without a preceding load. Chosen because it is the only primitive
  the ISA offers, and the `0xC8 -> 200` (not `-56`) capstone check pins it.
- **Two-address dst-accumulate with RT scratch (6780).** See above; the
  non-commutative aliasing case routes through `RT` rather than reordering
  operands, keeping the ambiguity log deterministic.
- **Signed div/mod semantics (6781).** `DIV`/`MOD` are signed, truncate toward
  zero, remainder takes the dividend's sign (`asm/test_div.mazm`), matching C99
  exactly, so `div->DIV` / `rem->MOD` / `udiv->UDIV` / `urem->UMOD` is a direct
  1:1 lowering.
- **Phi inherited from rega (6783).** QBE's target-independent register allocator
  materializes phi moves as edge copies before emit runs; the back-end adds no
  phi code and a phi reaching `maize_emitfn` is a `die()` bug, not a silent
  handle.

## Still out of scope

The following genuinely remain unsupported and `err()` in `abi.c`/`isel.c` (or
`die()` in `emit.c`); none is reached by the capstone:

- floating point (`s`/`d`), out of scope for the whole workstream (maize-11
  decision 6413);
- aggregate (struct) arguments and returns;
- varargs;
- environment calls;
- more than 10 register arguments / stack-passed argument overflow past R9;
- dynamic (non-constant) `alloc`;
- address references embedded in initialized data.
