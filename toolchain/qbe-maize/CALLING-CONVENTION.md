# Maize C calling convention

The C ABI the QBE Maize target implements (maize-62, satisfying maize-11 AC 6401 /
decision 6416). This documents the **final** register partition and the frame
shape the back-end actually emits; it is the contract maize-63 builds on.

## Register partition

`mazm` register names: `R0..R9`, `RV`, `RT`, `RB` (alias `BP`), `RS` (alias `SP`).
Sub-registers: `B*` = 8-bit, `Q*` = 16-bit, `H*` = 32-bit, `W0`/`W` = 64-bit.

| Register | Role | Preservation |
|----------|------|--------------|
| R0..R5 | integer/pointer args 1-6; general allocatable | caller-saved |
| R6..R9 | integer/pointer args 7-10; general allocatable | callee-saved |
| RV | return value (narrow returns per the `w` idiom, maize-11 decision 6406) | caller-saved |
| RT | back-end scratch (isel/addressing temporaries) | not RA-allocatable (globally reserved) |
| RB (BP) | frame pointer | callee-saved (established by the standard prologue) |
| RS (SP) | stack pointer; grows down; 8-byte slots | fixed |
| RP (PC), RF (flags) | fixed-role | not allocatable |

The partition is exactly maize-11 decision 6416's proposal: **no tuning of the
caller/callee-saved split was needed** to satisfy QBE's register allocator for the
hello-world slice. Recorded so maize-63 inherits a settled contract.

In QBE-internal terms (`toolchain/qbe-maize/targ.c`):

- allocatable general-purpose pool = `R0..R9, RV`;
- `rsave` (caller-saved) = `{R0, R1, R2, R3, R4, R5, RV}`;
- `rclob` (callee-saved) = `{R6, R7, R8, R9}` (plus RB, saved by the prologue);
- `rglob` (never allocated) = `{RT, RB, RS}`.

Unlike some targets, the return register **RV is distinct from the first argument
register R0**: arguments arrive in `R0..R9` and results leave in `RV`.

## Argument passing

- The first ten integer/pointer arguments go in `R0..R9`, left to right.
- Arguments past `R9` are pushed on the stack right-to-left by the caller
  (**not reached by hello world; maize-63 exercises the overflow path**).
- `CALL` pushes an 8-byte return address onto the `RS` stack.
- Aggregates (struct-by-value), varargs, and environment/closure calls are **not
  yet implemented** and the ABI lowering `err()`s on them (maize-63 surface),
  rather than miscompiling silently.

## Return values

- Integer/pointer results are returned in `RV`.
- Floating-point and aggregate returns are **not yet implemented** (`err()`).
- For the hello-world slice `main`'s `return 0;` materializes the `w` constant `0`
  into `RV` (`CP $00000000 RV`); the value is currently unobserved because `crt0`
  `HALT`s after `CALL main` (maize-11 decision 6408 / maize-62 decision 6638).

## Frame layout and prologue/epilogue

Stack grows down; slots are 8 bytes. Stack-alignment target at call boundaries is
**8 bytes** (there is no FP/SIMD, so no 16-byte requirement). The prologue/epilogue
match `asm/hello.mazm:strlen`:

```
; prologue                       ; epilogue
PUSH BP                          ; (restore any saved callee-saved regs)
CP SP BP                         CP BP SP
SUB <framesize> SP  (if > 0)     POP BP
; (save any used callee-saved regs)   RET
```

`<framesize>` = 8-byte-aligned local/spill area plus space for preserved
callee-saved registers. When a function uses callee-saved registers `R6..R9`, they
are saved to reserved frame slots (`LEA $-NN BP RT; ST Rn @RT`) after the frame is
established and restored (`LD @RT Rn`) before teardown.

For **hello-world `main`** there are no locals, no spills, and no callee-saved
registers in use, so the frame is empty and the emitted prologue/epilogue reduce to
exactly:

```
main:
    PUSH BP
    CP SP BP
    ...
    CP BP SP
    POP BP
    RET
```

The runtime's hand-written `puts` (`toolchain/rt/puts.mazm`) preserves `R6` across
its `sys_write` call, demonstrating the callee-saved store/restore contract.

## Syscall ABI

The C-to-syscall boundary (the `SYS` instruction's argument/result registers, the
`-errno` error convention, the raw-stub / errno-wrapper split, and the frozen hosted
syscall-number table) is a separate contract, recorded in
[`../rt/SYSCALL-ABI.md`](../rt/SYSCALL-ABI.md). In brief: `SYS` reads arguments from
the same `R0..R9` this document assigns and writes its result to `RV`; a result in
`[-4095, -1]` is `-errno`, which the C wrapper layer (`toolchain/rt/errno.c`) flips
into `errno` + a `-1` return.
