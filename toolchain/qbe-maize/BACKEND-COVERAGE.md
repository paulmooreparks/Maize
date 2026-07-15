# Maize QBE back-end: instruction-selection coverage

The QBE Maize target covers the full instruction-selection matrix reached by a
nontrivial single-TU C program (as of maize-63): control flow, the comparison
family both as branch predicates and as materialized 0/1 values, arithmetic /
logic / shift / signed+unsigned div/mod, sign/zero-extending sub-word loads and
stores, explicit width casts, and frame-slot addressing. Floating point (`s`/`d`)
is also covered: under the Zfinx ABI float values live in the general register
file (an isel pre-pass reclasses float temps `Ks`->`Kw`, `Kd`->`Kl` so the
allocator places them in `R0..R9/RV`), and the emitter selects `FADD`/`FSUB`/
`FMUL`/`FDIV`, `FCMP` with IEEE NaN/unordered-correct predicate synthesis, the
signed `FCVTFF`/`FCVTFS`/`FCVTSF` conversions, inline IEEE-bit float constants,
and float args/returns in GP registers. Anything genuinely outside that set (see
**Still out of scope**) still `err()`s in `isel`/`abi` or `die()`s in `emit`, so
an unsupported construct surfaces rather than miscompiling silently.

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
| data object (`data $sym = ...`) | `SECTION <kind>` + optional `GLOBAL` + `<sym>:` + body | deferred section-routing state machine (`data.c`, modelled on `qbe/gas.c`); routing below |
| `b/h/w/l <int>` item | `DATA $HH ...` byte list via `emitnum` | 1/2/4/8-byte little-endian; proven at each width by `ctest/globals.c` |
| `b "..."` item | `DATA $HH ...` byte list via `emitstr` | strings decoded from QBE's gas-escaped form to raw bytes |
| wholly-zero object (`z N` only) | `SECTION BSS` + `ZERO N` (NOBITS) | zero-at-load via `load_mzx`, no file bytes; proven by `globals.c` zero array read-before-write |
| partially-zero object (`w 1, z N`) | stays in `SECTION DATA`, holes/tail as real `DATA $00` bytes | C data-vs-bss rule (decision 7166) |
| `align N` (`DAlign`) | `ALIGN N` (maize-89 directive) | section-relative pad + section max-align; honored by `mzld`; proven by `globals.c` 8-aligned `long` |
| pointer-in-data (`l $sym[+off]`) | `DREF 8 <sym>[+off]` (maize-89 directive) -> `R_MAIZE_ABS64` | linker patches slot with `sym_addr + addend`; proven by `ctest/ptrdata.c` (`int *p=&g`, `&arr[1]`, `char *msgs[]`) |
| pointer-in-data (`w $sym[+off]`) | `DREF 4 <sym>[+off]` -> `R_MAIZE_ABS32` | 4-byte variant (not reached by the current fixtures; supported) |
| global address (`$sym`, code) | `CP <label> Rn`, Rn = whole register (W0) | full-64 materialization, maize-11 decision 6415 (see below); code-side `R_MAIZE_ABS64`, unchanged by maize-77 |
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
| `loadsb`/`sh`/`sw` | `LD @a <dst.sub>` + `CP <dst.sub> <dst>` | sign-extend; LDZ (maize-204) covers zero-extend only, this stays two instructions |
| `loadub`/`uh`/`uw` | `LDZ @a <dst.sub>` | zero-extend, one instruction (maize-204, folds the former LD+CPZ pair); proven by `ctest/ldzfold.c` |
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

## Image layout (segmented `.mzx`, maize-77)

The C pipeline is `cproc-qbe -> qbe -t maize -> mazm -c -> mzld -> maize`: the QBE
back-end emits mazm text decorated with the object-mode directives
`SECTION`/`GLOBAL`/`ALIGN`/`DREF` (all inert no-ops in mazm's flat mode, so the
same text also assembles flat), `mazm -c` produces a relocatable `.mzo`, and `mzld`
links the runtime objects (`crt0`/`syscall`/`puts`) plus the body object into one
linked `.mzx`. `mazm` without `-c` and the flat `.mzb` format are unchanged; the
hand-written asm suite stays on the flat path.

### Section routing (`data.c`, OQ-e disposition (a))

The pinned cproc emits no `section ".rodata"` annotation, so routing is by name in
the back-end:

- `.L`-prefixed compiler-internal objects (string literals, switch tables) ->
  `SECTION RODATA`;
- named objects (mutable file-scope globals) -> `SECTION DATA`;
- wholly-zero objects -> `SECTION BSS` (NOBITS);
- an explicit `DStart` section hint is honored if ever present (forward-compat).

Exported objects (`data $sym`, non-static) additionally get `GLOBAL <sym>`; static
objects stay local. `emit.c` opens each function with `SECTION CODE` and emits
`GLOBAL <fn>` for an exported function, so the runtime's cross-object `CALL main`
resolves through `mzld`.

### Segment picture

`mzld` lays sections out from base `0x0` in fixed order CODE, RODATA, DATA, BSS,
each aligned to its recorded max-align; `load_mzx` copies each segment to its vaddr,
zero-fills the BSS tail, and sets `RP = entry` (`_start`, the GLOBAL entry in crt0):

```
 low  0x0            +-----------------+  CODE   (R+X)   entry = _start
                     |  crt0/syscall/  |
                     |  puts / body    |
                     +-----------------+  RODATA (R)     string literals, consts
                     +-----------------+  DATA   (R+W)   mutable globals (initialized)
                     +-----------------+  BSS    (R+W, NOBITS) zero-at-load
      end-of-image   +-----------------+
                     |  (implicitly-   |  <- future heap grows UP (OQ-d intent)
                     |   zero gap)     |
                     |  full-desc stack|  <- grows DOWN from TOP
      RS -> &argc    | argc/argv/envp  |  maize-60 process-start block
high 0xFFFFFFFFFFFFFFF8 +-------------+  TOP
```

- **W^X:** `mzld` rejects any W+X input section up front; with CODE = R+X and
  DATA/BSS = R+W (`default_attrs`) nothing is ever both. Proven positively (the C
  program links and runs) and negatively (a section-attrs-flipped object is
  rejected), `scripts/run-ctest.sh`.
- **NOBITS BSS:** a wholly-zero object carries `mem_size > file_size == 0`;
  `load_mzx` zero-fills it at load, so it is both correct and compact (no on-disk
  zero bloat).
- **Honored alignment:** `align N` -> `ALIGN N` -> section-relative pad + section
  max-align -> `mzld` aligns the section base; a datum's absolute address then
  satisfies `% N == 0`.
- **Pointer-in-data:** `R_MAIZE_ABS64` (8-byte) / `R_MAIZE_ABS32` (4-byte) slots
  are patched by `mzld` with `sym_addr + addend`.
- **Non-collision with maize-60:** the image occupies low memory from 0; the
  process-start block sits at TOP and is built after load. The heap (unbuilt) is
  reserved to grow up from end-of-image into the gap (direction only; no
  brk/sbrk/malloc here).

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
- **Sub-word extension (6779, revised by maize-204).** A narrowing *load* now
  uses one instruction: zero-extend is `LDZ @a dst.sub` (card maize-204,
  reintroducing the zero-extending load), sign-extend stays `LD @a dst.sub` +
  `CP dst.sub dst`. Explicit `Oext*` casts (no load) still widen a register in
  place with `CP dst.sub dst` / `CPZ dst.sub dst`. The `0xC8 -> 200` (not `-56`)
  capstone check pins the zero-extension.
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

- aggregate (struct) arguments and returns;
- environment calls;
- more than 10 register arguments / stack-passed argument overflow past R9;
- dynamic (non-constant) `alloc`;
- variadic float `va_arg` reads (the float ARG side of a call is supported; only
  a float fetched by `va_arg` inside a variadic body still `err()`s);
- unsigned int-to-float conversions (the pinned qbe core has no `uwtof`/`ultof`
  op, so an unsigned conversion cannot be represented without a core change; the
  front end does not synthesize it in this pinned pairing). Signed int<->float
  and float<->double conversions are supported.

(Address references embedded in initialized data are supported via
`R_MAIZE_ABS64`/`ABS32`; see the Image-layout and pointer-in-data rows above.
Nonzero CODE-side address offsets on `CAddr` operands are now supported too, via
the isel routing + `emitcopy` LEA lowering described below, maize-143.)

## Nonzero-offset `CAddr` operands (`$sym + K`, maize-143)

QBE folds a global-symbol address plus a compile-time byte constant into a single
address constant (a `Con` of type `CAddr` with `bits.i == K`): this is what
`&global_array[K]`, `&s.field`, and `"lit" + K` fold to, and the `endptr == base
+ N` idiom (maize-142). mazm has no `sym+K` instruction-operand form (only the
`DREF` data addend), and `opnd()` cannot emit a preceding materialization
mid-line, so before maize-143 the emitter `die()`d on any such operand at four
sites (`opnd`, `memaddrreg`, `emitcopy`->slot, `emitcall`).

The fix is overlay-only (`isel.c` + `emit.c`, no qbe submodule bump, no VM/mazm
change):

- `isel.c` `fixarg()` routes every nonzero-offset `CAddr` con through a fresh
  register (`Ocopy`), so it always lands as the source of a register-destination
  copy; `fixarg` runs on every ALU/cmp/load/store/ext operand, the successor-phi
  arguments, and (added here) the `Ocall` target, so no `$sym+K` reaches an inline
  operand printer.
- `emit.c` `emitcopy()` is then the sole materialization site: it emits `CP
  <label> Rd` (the existing zero-offset idiom) followed by a FLAG-NEUTRAL `LEA
  $<off> Rd Rd` (the `lea_off` helper, a sibling of `lea_negoff`). The offset add
  MUST be flag-neutral because the successor-phi-arg pass can land the
  materialization at a block end, between a fused flag-only `CMP` and its `Jcc`;
  maize `LEA` saves/restores `RF` around its internal add (src/cpu.cpp,
  `instr::lea_immVal_regreg`; maize-4), an `ADD`/`SUB` would clobber the branch.
  Both the reg-dest and the `CAddr`->slot (spilled temp) copy sub-paths lower the
  offset this way. The offset immediate is digit-sized like `lea_negoff` so mazm
  does not sign-extend it; a negative offset uses `LEA $-<off> Rd Rd`.
- The three other `die` sites (`opnd`, `memaddrreg`, `emitcall`) stay as
  defensive invariants: unreachable for a nonzero offset once isel routes it, so a
  future routing gap surfaces as a `die` rather than a miscompile.

Regressions: `ctest/caddroff.c` forms and uses each fold pattern with checked
results; `ctest/caddroff_flag.qbe` (hand-written QBE, since cproc keeps locals in
memory and never emits a bare `CAddr`-con phi argument) drives the phi-across-a-
fused-branch case that gates the flag-neutral lowering.
