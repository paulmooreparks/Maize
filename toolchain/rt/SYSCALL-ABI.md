# Maize syscall ABI (hosted)

The C-callable binding for Maize syscalls. This is the contract the C
runtime and every C program compile against; the numbers and the error convention
below are **binary ABI**. It is scoped to the runtime (`toolchain/rt`)
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

Arguments continue in the remaining argument registers for calls that take more (none
in scope today). `count`/size arguments are full 64-bit (`regs::r2.w0`); buffers are
full-64 addresses. The VM lands the result in `RV` (`src/cpu.cpp`:
`regs::rv.w0 = sys::call(...)`), which is also the C return register.

## Error convention: negative result is `-errno`

The raw layer returns `RV` **verbatim**, with no error interpretation. Following the
Linux/musl convention, a result in the range **`[-4095, -1]`** encodes `-errno`;
`MAX_ERRNO` is `4095`. Everything else is a valid result.

The POSIX-named wrapper layer turns that into the familiar `errno` + `-1` contract via
the musl `__syscall_ret` translator:

```c
long __syscall_ret(unsigned long r) {
    if (r > -4096UL) {      /* r in [-4095, -1]; -4096UL == 0xFFFFFFFFFFFFF000 */
        errno = -(long)r;
        return -1;
    }
    return (long)r;
}
```

`errno` is a plain global `int` (the VM is single-threaded; TLS is reserved for
future threading work). The wrapper signature does not change if `errno`
later becomes thread-local.

The VM produces real `-errno` codes in the `[-4095, -1]` band. A wrong-direction or
nonexistent fd (`write` to fd 0, `read` from fd 1/2, either to fd >= 3) returns
`-EBADF` (9); a real host I/O failure on the in-scope stdio fds returns the host
errno verbatim on Linux (numerically identical to the ABI) and a synthesized `-EIO`
(5) on Windows. `sys_read` returns the byte count and copies only the bytes actually
read, so a short read never spills an uninitialized buffer tail into guest memory.

**`brk` is exempt from the errno convention.** `sys_brk` ($0C) always returns the
current (possibly unchanged) break, never `-errno`: the break is a low address that
cannot land in the `[-4095, -1]` band, and failure is detected by the caller comparing
the returned break to the requested one (the libc `sbrk` wrapper).

**EFAULT is never produced.** Maize memory is sparse and lazily zero-filled, so every
guest address is physically valid and a bad-pointer syscall cannot fault. This is an
honest deviation from Linux, recorded rather than faked.

## Raw layer vs wrapper layer

| Layer | Symbols | Where | Role |
|-------|---------|-------|------|
| raw stubs | `sys_read`, `sys_write`, `_exit` | `toolchain/rt/syscall.mazm` | `SYS <n>; RET`; return `RV` verbatim (the musl `__syscall` role) |
| wrappers | `read`, `write`, `open`, `close`, `lseek`, `fstat` | `toolchain/rt/errno.c` | call the raw stub, pass the result through `__syscall_ret` |
| storage | `errno`, `__syscall_ret` | `toolchain/rt/errno.c` | global `int errno`; the translator |
| header | all of the above | `toolchain/rt/syscall.h` | C declarations |

The raw process-termination stub is `_exit` (POSIX raw termination: no `atexit`/flush).
The stdlib `exit()` (`toolchain/rt/stdlib.c`) runs the `atexit` registry LIFO, then
delegates to `_exit`. `stdout`/`stderr` are unbuffered (direct `sys_write`), but
`fopen`'d streams are fully buffered, so stdio registers
`__stdio_flush_all` on the `atexit` registry at first `fopen`; `exit()` therefore
flushes buffered write streams on a return from `main`, while `_Exit`/`abort` (which
bypass the registry) do not.

## Frozen number table (hosted ABI)

Mirrors the Linux x86-64 table by construction. Frozen:

| Number | Symbol | Args | Result |
|--------|--------|------|--------|
| `$00` | `sys_read` | `R0`=fd, `R1`=buf, `R2`=count | `RV`=bytes read |
| `$01` | `sys_write` | `R0`=fd, `R1`=buf, `R2`=count | `RV`=bytes written |
| `$02` | `sys_open` | `R0`=path, `R1`=flags, `R2`=mode | `RV`=fd or `-errno` |
| `$03` | `sys_close` | `R0`=fd | `RV`=0 or `-errno` |
| `$05` | `sys_fstat` | `R0`=fd, `R1`=statbuf | `RV`=0 or `-errno` |
| `$08` | `sys_lseek` | `R0`=fd, `R1`=offset, `R2`=whence | `RV`=new offset or `-errno` |
| `$52` | `sys_rename` | `R0`=oldpath, `R1`=newpath | `RV`=0 or `-errno` |
| `$53` | `sys_mkdir` | `R0`=path, `R1`=mode | `RV`=0 or `-errno` |
| `$57` | `sys_unlink` | `R0`=path | `RV`=0 or `-errno` |
| `$0C` | `sys_brk` | `R0`=new break (0=query) | `RV`=new (or current) break; never `-errno` |
| `$3C` | `_exit` (`sys_exit`) | `R0`=code | does not return |
| `$A9` | `sys_reboot` | reserved (VM stub) | reserved |
| `$D9` | `sys_getdents64` | `R0`=fd, `R1`=dirp, `R2`=count | `RV`=bytes / 0 (EOF) / `-errno` |
| `$F0` | `sys_clock_ms` | none | `RV`=monotonic ms since VM start; never `-errno` |

`$F0` is a Maize-private number (see "Maize-private high block" below), not a Linux
mirror: it returns a raw monotonic millisecond count in `RV` and, like `sys_brk`, is
exempt from the errno convention. The returned value cannot fall in the `[-4095, -1]`
band in any realistic runtime (that band begins at ~5.8e8 years of milliseconds), so
`RV` is always a valid clock value.

`$02`/`$03`/`$05`/`$08`/`$D9` are the hostfs file syscalls (design of record
`docs/design/hostfs.md`), each mirroring its Linux x86-64 number. They are guest-visible
only when a `--mount` / `--mount-home` grant installs a mount table (or the default
sandbox root is present); `read`/`write` for a granted fd `>= 3` resolve through that
table (lifting the M4 real-file restriction).

`$52`/`$53`/`$57` (`sys_rename` / `sys_mkdir` / `sys_unlink`, maize-151) are the
path-mutating hostfs syscalls, mirroring their Linux x86-64 numbers (82 / 83 / 87). Each
takes guest path pointer(s) rather than an fd: the core normalizes the path against the
cwd, longest-prefix matches a mount, and applies the write-intent gate before the backend
touches the host. A `:ro` mount and the synthetic root reject them with `-EROFS` (30); an
unmounted path is `-ENOENT` (2); a `rename` whose two paths land in different mounts is
`-EXDEV` (18), because the backend rename is a single host operation that cannot cross
mounts. The backend confines the remainder beneath the mount anchor exactly as `open`
does (openat2 `RESOLVE_BENEATH` on Linux; the canonical prefix-child + reparse-point
check on Windows), so neither a guest `..` nor an escaping host symlink can leave the
mount. `mkdir`'s `mode` follows the guest value (falling back to `0755` when the guest
passes `0`, so the directory is enterable).

The remaining out-of-scope path-based / mutating numbers (`$04` stat, `$54` rmdir via a
distinct number, the `*at` family at `$101`+, symlink / chmod / truncate) are NOT
dispatched in this POC and stay reserved for later cards.

## Numbering policy

- Mirror the Linux x86-64 number for any call that has a Linux equivalent (for
  example `brk=$0C`, the heap primitive).
- Reserve a Maize-private high block for calls with no Linux equivalent.
- This hosted SYS table (the VM acting as kernel) is a namespace **distinct** from the
  eventual guest-OS `INT $80` table. Freezing the SYS numbers here does not bind that
  future surface.

### Maize-private high block (`$F0`-`$FF`)

`$F0`-`$FF` is reserved as the **Maize-private high block**: numbers for calls that
have no Linux equivalent and so cannot mirror a Linux number. Assignments in this block
deliberately do **not** mirror the Linux x86-64 table (where those bytes are the
`mq_*` / `keyctl` / `inotify_*` family); that non-mirror is intentional and of record.
A Maize-private call returns whatever shape suits it, not the Linux call's shape at the
same byte. `$F0` currently hosts `sys_clock_ms` (the monotonic millisecond clock); it
previously hosted a different, since-removed syscall, which is why this block starts at
`$F0` rather than being wholly unused history.

The RV-returns-`uint64`-ms shape of `sys_clock_ms` has no Linux-ABI equivalent: Linux
`clock_gettime` takes `(clockid, struct timespec*)` and returns `0`/`-errno`, writing
its result to memory. Reusing that number with a different shape would violate the
"mirror the Linux number ⇒ mirror the Linux semantics" contract, so the clock lives in
the private block instead.

Once C compiles against these stubs, the numbers are ABI. The convention frozen here
(RV result, the `[-4095, -1]` errno range, the raw/wrapper split, Linux-mirrored
numbers) is binding for everything built on top.
