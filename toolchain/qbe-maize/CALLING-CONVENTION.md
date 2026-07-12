# Maize C calling convention

The C ABI the QBE Maize target implements. This documents the register partition
and the frame shape the back-end actually emits.

## Register partition

`mazm` register names: `R0..R9`, `RV`, `RT`, `RB` (alias `BP`), `RS` (alias `SP`).
Sub-registers: `B*` = 8-bit, `Q*` = 16-bit, `H*` = 32-bit, `W0`/`W` = 64-bit.

| Register | Role | Preservation |
|----------|------|--------------|
| R0..R5 | integer/pointer args 1-6; general allocatable | caller-saved |
| R6..R9 | general allocatable (never carry arguments) | callee-saved |
| RV | return value (narrow returns per the `w` idiom) | caller-saved |
| RT | back-end scratch (isel/addressing temporaries) | not RA-allocatable (globally reserved) |
| RB (BP) | frame pointer | callee-saved (established by the standard prologue) |
| RS (SP) | stack pointer; grows down; 8-byte slots | fixed |
| RP (PC), RF (flags) | fixed-role | not allocatable |

Call arguments only ever occupy caller-saved registers, and the variadic register
save area stays a constant 48 bytes, matching x86-64 SysV.

In QBE-internal terms (`toolchain/qbe-maize/targ.c`):

- allocatable general-purpose pool = `R0..R9, RV`;
- `rsave` (caller-saved) = `{R0, R1, R2, R3, R4, R5, RV}`;
- `rclob` (callee-saved) = `{R6, R7, R8, R9}` (plus RB, saved by the prologue);
- `rglob` (never allocated) = `{RT, RB, RS}`.

Unlike some targets, the return register **RV is distinct from the first argument
register R0**: arguments arrive in `R0..R5` and results leave in `RV`.

## Argument passing

- The first six integer/pointer arguments go in `R0..R5`, left to right.
- **Floating-point arguments** use the **same** integer argument registers `R0..R5`,
  interleaved with integer/pointer arguments in one left-to-right sequence (there is no
  separate FP argument class, unlike SysV/XMM). This falls out of the Zfinx floating-point
  ISA (maize-122): floats live in the integer registers, so a `float` occupies the low 32
  bits (`H0`) of its argument register and a `double` occupies the full 64 bits (`W0`).
  Floating-point arguments past `R5` spill to 8-byte stack slots exactly like integer args.
  (This is the frozen ISA/ABI contract; the QBE backend lowering that emits it is a
  downstream card, so the lowering below still `err()`s on FP arguments until then.)
- Arguments past `R5` (arg 7 onward) are pushed on the stack right-to-left by the
  caller in 8-byte slots, so arg 7 sits at the lowest address:
  after the callee's `PUSH BP`, overflow args land at `[BP+16]`, `[BP+24]`, ...
  (`BP+0` = saved BP, `BP+8` = return address). The caller reserves and releases
  the slots.
- `CALL` pushes an 8-byte return address onto the `RS` stack.
- Variadic functions: a variadic callee
  spills all six argument registers `R0..R5` into a 48-byte register save area at
  `BP-48..BP-0`, and `va_list` is the 24-byte SysV gp-subset
  `{gp_offset:u32 @0, reserved:u32 @4, overflow_arg_area:ptr @8,
  reg_save_area:ptr @16}`. `va_start`/`va_arg` lowering is a port of QBE's
  amd64/sysv.c with the floating-point branch dropped. Under Zfinx there is no FP
  register file to save, so the existing 48-byte gp-only register save area and the
  FP-branch-dropped `va_arg` lowering stay correct as-is even now that the ISA has
  floating point: a variadic `float`/`double` argument is spilled and fetched through
  the same gp register save area as any other 64-bit argument slot.
- Aggregates (struct-by-value) and environment/closure calls are **not yet
  implemented** and the ABI lowering `err()`s on them, rather than miscompiling
  silently.

## Return values

- Integer/pointer results are returned in `RV`.
- **Floating-point results** are returned in `RV`: a `float` in `RV.H0` (low 32 bits), a
  `double` in `RV.W0` (full 64 bits). This is the frozen ISA/ABI contract (maize-122);
  the QBE backend lowering that emits it is a downstream card, so the lowering still
  `err()`s on floating-point returns until that card lands.
- Aggregate returns are **not yet implemented** (`err()`).
- `main`'s return value is observed: `crt0` routes it through `exit()` to
  `sys_exit` (`SYS $3C`), which records the low 8 bits as the host process exit
  status.

## Frame layout and prologue/epilogue

Stack grows down; slots are 8 bytes. Stack-alignment target at call boundaries is
**8 bytes** (floating point is Zfinx scalar, reusing the integer registers, and there is
no SIMD, so no 16-byte requirement). The prologue/epilogue
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


## Syscall ABI

The C-to-syscall boundary (the `SYS` instruction's argument/result registers, the
`-errno` error convention, the raw-stub / errno-wrapper split, and the frozen hosted
syscall-number table) is a separate contract, recorded in
[`../rt/SYSCALL-ABI.md`](../rt/SYSCALL-ABI.md). In brief: `SYS` reads arguments from
the same argument registers this document assigns (`R0..R2` for every call in scope
today) and writes its result to `RV`; a result in `[-4095, -1]` is `-errno`, which
the C wrapper layer (`toolchain/rt/errno.c`) flips into `errno` + a `-1` return.
