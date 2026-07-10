# Maize syscall ABI (hosted)

The C-callable binding for Maize syscalls (maize-74). This is the contract the C
runtime and every C program compile against; the numbers and the error convention
below are **binary ABI** as of maize-74. It is scoped to the runtime (`toolchain/rt`)
and the VM's SYS dispatch; the C-language register/frame ABI lives in
[`../qbe-maize/CALLING-CONVENTION.md`](../qbe-maize/CALLING-CONVENTION.md).

## Registers

A syscall is the `SYS <number>` instruction. It reads its arguments from the C ABI
argument registers and writes its result to the C ABI return register, so a stub is
nothing more than `SYS <number>; RET`:

| Role | Register |
|------|----------|
| syscall number | the immediate operand of `SYS` |
| arg 1 (fd) | `R0` |
| arg 2 (buf) | `R1` |
| arg 3 (count) | `R2` |
| result | `RV` |

Arguments continue in `R3..R9` for calls that take more (none in scope today).
`count`/size arguments are full 64-bit (`regs::r2.w0`, maize-56); buffers are full-64
addresses. The VM already lands the result in `RV` (`src/cpu.cpp`:
`regs::rv.w0 = sys::call(...)`), which is also the C return register, so adopting `RV`
is zero-cost (maize-74, OQ1).

## Error convention: negative result is `-errno`

The raw layer returns `RV` **verbatim**, with no error interpretation. Following the
Linux/musl convention, a result in the range **`[-4095, -1]`** encodes `-errno`;
`MAX_ERRNO` is `4095`. Everything else is a valid result.

The POSIX-named wrapper layer turns that into the familiar `errno` + `-1` contract via
the musl `__syscall_ret` translator (maize-74 decision 7306):

```c
long __syscall_ret(unsigned long r) {
    if (r > -4096UL) {      /* r in [-4095, -1]; -4096UL == 0xFFFFFFFFFFFFF000 */
        errno = -(long)r;
        return -1;
    }
    return (long)r;
}
```

`errno` is a plain global `int` (the VM is single-threaded; TLS is reserved for a
future threading card, maize-74 OQ3). The wrapper signature does not change if `errno`
later becomes thread-local.

**Known gap (maize-75):** the VM does not yet produce real `-errno` codes. Today a
failing call returns a bare `-1`, which still lands in `[-4095, -1]`, so the
translation MECHANISM runs and `errno` becomes `1` on error. maize-75 makes the VM
return the correct code (and fixes `sys_read`, whose `case $00` currently falls
through to `return 0` instead of the byte count). The wrapper does not change then.

## Raw layer vs wrapper layer

| Layer | Symbols | Where | Role |
|-------|---------|-------|------|
| raw stubs | `sys_read`, `sys_write`, `_exit` | `toolchain/rt/syscall.mazm` | `SYS <n>; RET`; return `RV` verbatim (the musl `__syscall` role) |
| wrappers | `read`, `write` | `toolchain/rt/errno.c` | call the raw stub, pass the result through `__syscall_ret` |
| storage | `errno`, `__syscall_ret` | `toolchain/rt/errno.c` | global `int errno`; the translator |
| header | all of the above | `toolchain/rt/syscall.h` | C declarations |

The raw process-termination stub is `_exit` (POSIX raw termination: no `atexit`/flush).
Buffered stdlib `exit()` is deferred to maize-76. `crt0` inlines `SYS $3C` directly
rather than calling `_exit`.

## Frozen number table (hosted ABI)

Mirrors the Linux x86-64 table by construction. Frozen as of maize-74:

| Number | Symbol | Args | Result |
|--------|--------|------|--------|
| `$00` | `sys_read` | `R0`=fd, `R1`=buf, `R2`=count | `RV`=bytes read |
| `$01` | `sys_write` | `R0`=fd, `R1`=buf, `R2`=count | `RV`=bytes written |
| `$3C` | `_exit` (`sys_exit`) | `R0`=code | does not return |
| `$A9` | `sys_reboot` | reserved (VM stub) | reserved |

## Numbering policy

- Mirror the Linux x86-64 number for any call that has a Linux equivalent (for
  example `brk=$0C`, reserved for maize-75's heap).
- Reserve a Maize-private high block for calls with no Linux equivalent.
- This hosted SYS table (the VM acting as kernel) is a namespace **distinct** from the
  eventual guest-OS `INT $80` table. Freezing the SYS numbers here does not bind that
  future surface.

Once C compiles against these stubs, the numbers are ABI. The convention frozen here
(RV result, the `[-4095, -1]` errno range, the raw/wrapper split, Linux-mirrored
numbers) is inherited by maize-75, maize-76, and the eventual Doom-demo chain.
