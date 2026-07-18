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

## Provider routing: native vs guest (card maize-24, decision D9)

A new RF bit, the **syscall-guest flag** (toggled by the zero-operand `SETSYSG` /
`CLRSYSG` instructions), selects which provider `SYS` dispatches to:

- **Clear (boot default):** `SYS` calls the native `sys::call` provider, byte-identical
  to v1.0. This is the firmware baseline that runs before / without a guest OS.
- **Set:** `SYS` traps through the shared trap table at **cause 7** into the
  guest-installed handler (`deliver_vectored`). The register ABI above is unchanged
  (args in `R0`/`R1`/`R2`, result the guest writes to `RV`); the syscall number, which
  the native path takes from the `SYS` operand, is delivered to the guest handler as the
  trap frame's **aux word** (`[RS+0]` on handler entry, below cause / saved-RF / saved-PC).
  A guest OS (quesOS) sets the flag once its cause-7 handler is resident, and toggles it
  clear/set around a re-issued `SYS` to pass the already-native file/IO calls straight
  through to `sys::call`. With no cause-7 handler installed, a `SYS` taken with the flag
  set halts the VM under the uniform no-handler rule, exactly like any other vector.

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

**EFAULT is never produced by the native/hosted syscall layer.** Maize memory is sparse
and lazily zero-filled, so at that layer every guest address is physically valid and a
bad-pointer syscall cannot fault. This is an honest deviation from Linux, recorded rather
than faked. It scopes strictly to the native flat-memory layer: a guest kernel that emulates
per-process paging on top (quesOS's Sv48 address spaces, maize-180/194) enforces its own
memory-protection bounds and DOES produce `-EFAULT` for a bulk syscall naming an unmapped or
kernel-owned page (see the forwarded `$F4`/`$F5` note below). That is a distinct, higher
enforcement layer, not a contradiction of this claim, the same relationship a real kernel's
`SIGSEGV` has to physical RAM that is always addressable.

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
| `$4D` | `sys_ftruncate` | `R0`=fd, `R1`=length | `RV`=0 or `-errno` |
| `$0C` | `sys_brk` | `R0`=new break (0=query) | `RV`=new (or current) break; never `-errno` |
| `$3C` | `_exit` (`sys_exit`) | `R0`=code | does not return |
| `$A9` | `sys_reboot` | reserved (VM stub) | reserved |
| `$D9` | `sys_getdents64` | `R0`=fd, `R1`=dirp, `R2`=count | `RV`=bytes / 0 (EOF) / `-errno` |
| `$F0` | `sys_clock_ms` | none | `RV`=monotonic ms since VM start; never `-errno` |
| `$F1` | `sys_tcgetattr` | `R0`=fd, `R1`=termios* | `RV`=0 or `-errno` |
| `$F2` | `sys_tcsetattr` | `R0`=fd, `R1`=optional_actions, `R2`=termios* | `RV`=0 or `-errno` |

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

`$4D` (`sys_ftruncate`, maize-179) mirrors the Linux x86-64 number (77). It is the fd
form: `R0`=fd, `R1`=length (signed 64-bit). Only a hostfs-backed fd (`>= 3`) can be
truncated; the core applies the same `:ro` / synthetic-root write-gate as the other
mutating ops (`-EROFS`), rejects a negative length with `-EINVAL`, then resizes the open
file to exactly `length` on its confined handle (a shrink drops the tail, an extend
zero-fills; the file offset is unchanged). The stdio reservations `0`/`1`/`2` are not
regular files, so truncating them is `-EINVAL`, matching Linux `ftruncate` on a pipe/tty.
kilo's save rewrites the whole buffer after `ftruncate`, so a save-after-shrink is now
byte-exact with no stale tail.

The remaining out-of-scope path-based / mutating numbers (`$04` stat, `$54` rmdir via a
distinct number, the `*at` family at `$101`+, symlink / chmod, and the path form of
`truncate` at `$4C`) are NOT dispatched in this POC and stay reserved for later cards.

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

`$F1` / `$F2` host the console termios calls (`sys_tcgetattr` / `sys_tcsetattr`, maize-140).
They live in the private block rather than mirroring a Linux number because Linux has no
dedicated termios syscall: it drives termios through `ioctl(fd, TCGETS/TCSETS, ...)`, so
there is no Linux number to mirror. Each copies the frozen 36-byte termios wire image
(four little-endian 32-bit flag words `c_iflag`/`c_oflag`/`c_cflag`/`c_lflag`, then a
20-byte `c_cc[]`; `src/console_io.h` and `toolchain/rt/termios.h` agree byte for byte)
between guest memory and the window console's termios state, and returns `0` or `-EBADF`
(no window console is bound / the fd is not a tty). `tcgetattr()` / `tcsetattr()` /
`cfmakeraw()` (`toolchain/rt/termios.c`) wrap them.

The RV-returns-`uint64`-ms shape of `sys_clock_ms` has no Linux-ABI equivalent: Linux
`clock_gettime` takes `(clockid, struct timespec*)` and returns `0`/`-errno`, writing
its result to memory. Reusing that number with a different shape would violate the
"mirror the Linux number ⇒ mirror the Linux semantics" contract, so the clock lives in
the private block instead.

Current `$F0`-`$FF` block map (so a later assigner takes a genuinely-free number, not one
that merely looks free in a partial dispatch switch): `$F0` `sys_clock_ms`; `$F1`/`$F2`
termios (`sys_tcgetattr`/`sys_tcsetattr`); `$F3` `sys_palette_blit` (maize-213); `$F4`/`$F5`
bulk memory (`sys_bulk_copy`/`sys_bulk_set`, maize-216); `$F6` `sys_ttysize` (maize-228, three-branch
size source since maize-253: a bound console device's cell grid, else the real host terminal, else -ENOTTY);
`$F7`/`$F8`/`$F9` the quesOS-guest framebuffer registration calls (maize-236, documented in
the guest section below); `$FA`/`$FB` the quesOS-guest job-control calls (maize-174,
`sys_tcgetpgrp`/`sys_tcsetpgrp`, documented in the signal section below); `$FC`
`sys_fb_mmap` (maize-238, the quesOS-guest framebuffer mmap call, documented in the guest
section below); `$FD`/`$FE`/`$FF` the quesOS-guest display-surface calls (maize-251,
`sys_fb_present`/`sys_kbd_read`/`sys_bigalloc`, documented in the guest section below).
**With `$FD`/`$FE`/`$FF` assigned by maize-251, the Maize-private high block `$F0`-`$FF` is
now FULLY EXHAUSTED**: there is no free number left in it, so any future no-Linux-equivalent
call needs a different home (widening the SYS operand beyond 8 bits is an ISA change, or a
second reserved block would have to be carved).
`$F3`-`$FF` are native or guest calls that this doc enumerates here so the block map is complete.

Once C compiles against these stubs, the numbers are ABI. The convention frozen here
(RV result, the `[-4095, -1]` errno range, the raw/wrapper split, Linux-mirrored
numbers) is binding for everything built on top.

## quesOS guest-OS process calls (maize-93)

The process calls below are the **guest-OS surface**: they are dispatched by quesOS's
own cause-7 handler in GUEST code (a user process runs with the syscall-guest flag SET,
so its `SYS` traps into quesOS), never by the native `sys::call` provider, and nothing
is added to `src/sys.cpp` for them (the maize-24 placement rule). They are a namespace
distinct from the native hosted table above; a program run **directly by the VM** (not
under quesOS) must not call them. The raw stubs live in `toolchain/rt/syscall.mazm`
(`sys_fork` / `sys_wait4` / `sys_getpid` / `sys_execve` / `sys_pipe` / `sys_dup2` /
`sys_dup`), each a `SYS <n>; RET` over the C ABI argument registers. The numbers mirror
the Linux x86-64 table per the numbering policy.

| Number | Symbol | Args | Result |
|--------|--------|------|--------|
| `$16` | `sys_pipe` | `R0`=`int fds[2]` | `RV`=0 or `-errno`; `fds[0]` read end, `fds[1]` write end |
| `$20` | `sys_dup` | `R0`=oldfd | `RV`=new fd or `-errno` |
| `$21` | `sys_dup2` | `R0`=oldfd, `R1`=newfd | `RV`=newfd or `-errno` |
| `$27` | `sys_getpid` | none | `RV`=caller pid |
| `$39` | `sys_fork` | none | `RV`=child pid in the parent, `0` in the child, or `-errno` |
| `$3B` | `sys_execve` | `R0`=path, `R1`=argv, `R2`=envp | does not return on success; `-errno` on failure |
| `$3D` | `sys_wait4` | `R0`=pid (`-1`=any), `R1`=`int *status`, `R2`=options, `R3`=rusage | `RV`=reaped pid or `-errno` |
| `$0D` | `sys_rt_sigaction` | `R0`=sig, `R1`=act, `R2`=oldact | `RV`=0 or `-errno` (maize-174) |
| `$0E` | `sys_rt_sigprocmask` | `R0`=how, `R1`=set, `R2`=oldset | `RV`=0 or `-errno` (maize-174) |
| `$0F` | `sys_rt_sigreturn` | none (pops the kernel-pushed signal frame) | does not return normally (maize-174) |
| `$3E` | `sys_kill` | `R0`=pid (`<0`=pgid `-pid`, `0`=own group), `R1`=sig | `RV`=0 or `-errno` (maize-174) |
| `$6D` | `sys_setpgid` | `R0`=pid (`0`=self), `R1`=pgid (`0`=use pid) | `RV`=0 or `-errno` (maize-174) |
| `$79` | `sys_getpgid` | `R0`=pid (`0`=self) | `RV`=pgid or `-errno` (maize-174) |

Semantics quesOS implements: `fork` gives the child its own Sv48 address space with an
eager copy of the parent's mapped pages (independent memory) and a copied fd table
(shared open-file descriptions); `execve` rebuilds the address space + the SysV start
block from `argv`/`envp` while the fd table survives; `pipe` is a kernel ring buffer
whose empty-read / full-write parks the PROCESS (never the VM) and whose last-writer
close is EOF; `wait4` blocks until a matching child is a zombie and reports its status
(`WEXITSTATUS` in bits 8..15), reaping in any order; `dup2`/`dup` alias fd-table slots.
The file/IO calls a process makes (`read`/`write`/`open`/`close`) are dispatched by the
same quesOS handler: fd 1/2 and hostfs fds bounce through quesOS to the native provider,
pipe-end fds go to the ring buffer. A read of fd 0 rides the console device's
IRQ/status path (vector 33, ports `$00`/`$01`), NOT a native blocking read: when no byte
is available the reading PROCESS parks and the console IRQ delivers each byte and wakes
it, so a waiting reader never parks the whole CPU thread (design doc 17). The `INT $80`
naming remains a later concern; job control and signals (`kill`) land in maize-174 below.

quesOS also FORWARDS the Maize-private console calls a process makes so an interactive
shell works under it (maize-94): `$F1`/`$F2` (`sys_tcgetattr`/`sys_tcsetattr`, raw mode)
and `$F6` (`sys_ttysize`, the `TIOCGWINSZ` terminal-size query the line editor runs at
startup; since maize-253 the native provider answers it from a bound console device's cell
grid too, so a TUI under `--console-dump` or the windowed backend sizes its screen, not just
one on a real host terminal). Each maps the guest fd to its native fd and bounces the wire
image through the kernel `g_iobuf`; a pipe (non-native) fd returns `-ENOTTY`, and any Maize-private number
quesOS does NOT forward returns `-ENOSYS` to the caller (never strands the process), with a
one-line `[quesos] unhandled syscall N` diagnostic naming the number to wire next.

quesOS also forwards the bulk-memory accelerators `$F4`/`$F5` (`sys_bulk_copy`/`sys_bulk_set`,
maize-247). The native handlers read/write FLAT PHYSICAL addresses with no MMU walk, so quesOS
translates the caller's user src/dst VAs to physical addresses through the caller's own page
table before forwarding, and validates every page in range against `PTE_V` (mapped) AND `PTE_U`
(user-owned). `$F4` (copy/move) forwards a SINGLE native call only when BOTH src and dst are each
one physically-contiguous run; a scattered range would need a boundary-merged, memmove-safe split
the native call cannot express, so quesOS returns `-ENOSYS` and the guest RT `memcpy`/`memmove`
fall back to their portable word loop (`toolchain/rt/string.c` gates its fast path on the raw
return equalling `n`). `$F5` (fill) has no overlap hazard, so it ALWAYS forwards, coalescing the
dst range into contiguous physical runs and issuing one native call per run (two-pass:
validate-the-whole-range-then-deliver, preserving the native "no guest write on rejection"
invariant). A walked page that is unmapped or kernel-owned (`PTE_U`=0, e.g. the quesOS image
identity range) fails the range with `-EFAULT` (14) and no byte is touched, quesOS's own guest-side
memory-protection layer as noted in the Error convention section above.

### Guest signal subsystem (maize-174)

quesOS Phase-2 process-model signals, dispatched by quesOS's cause-7 handler in GUEST
code (never `sys::call`; nothing added to `src/sys.cpp`, the maize-24 rule). Six calls
mirror their Linux x86-64 numbers (`rt_sigaction` $0D, `rt_sigprocmask` $0E,
`rt_sigreturn` $0F, `kill` $3E, `setpgid` $6D, `getpgid` $79); the two controlling-tty
calls have no Linux syscall equivalent (real Linux drives them through
`ioctl(TIOCGPGRP/TIOCSPGRP)`, which this dispatcher does not forward), so they take the
next free numbers in the Maize-private `$F0`-`$FF` block after maize-236's `$F7`-`$F9`.

| Number | Symbol | Args | Result |
|--------|--------|------|--------|
| `$FA` | `sys_tcgetpgrp` | none | `RV`=foreground pgid of the controlling tty |
| `$FB` | `sys_tcsetpgrp` | `R0`=pgid | `RV`=0 (sets the foreground pgid; no session check, v1) |

`rt_sigaction` reads/writes `sa_handler` + `sa_mask` only (`sa_flags`/`sa_restorer`/siginfo
not modeled; SA_RESTART is implicitly on because no quesOS syscall returns EINTR).
`SIGKILL` cannot be caught (`rt_sigaction` on it returns `-EINVAL`) or blocked. Default
actions: SIGINT/SIGQUIT/SIGTERM/SIGKILL terminate (`wait4` then reports WIFSIGNALED with
the low 7 status bits = the signal); SIGCHLD is ignored. Handler dispatch pushes a signal
frame on the user stack and enters the handler with the signal number in `R0`; the handler
returns through a small user trampoline whose `SYS $0F` (`rt_sigreturn`) restores the
interrupted context. The console recognizes 0x03 (INTR)/0x1C (QUIT) by raw byte value in
quesOS's own input path and raises SIGINT/SIGQUIT on the foreground process group; the
host machine layer is unchanged (no host-side signal awareness, doc 18).

### Framebuffer registration calls (maize-236)

quesOS-private, guest-only display-arbitration calls with **no Linux equivalent** (Linux
does this through `ioctl` on `/dev/fb0` or a VT `ioctl`, so there is no Linux number to
mirror). Per the numbering policy they take numbers in the reserved Maize-private high block
`$F0`-`$FF`; the next free bytes after the block map above are `$F7`-`$F9`. (Decision D6
named `0x1000`-`0x1002`, which cannot encode: the `SYS` immediate rides only its low 8 bits
into the cause-7 frame, and widening it would be an ISA change out of scope for that card.
Review cycle 1 rejected an earlier `$E0`-`$E2` because those are real Linux numbers, 224/225/226
`timer_gettime` family; Convention counterexamples Entry 5.) They are dispatched by quesOS's
cause-7 handler, never `sys::call`; a bare-VM program's `SYS $F7` hits the native table
instead. The device holds the registration table; quesOS tracks per-process ownership.

| Number | Symbol | Args | Result |
|--------|--------|------|--------|
| `$F7` | `sys_fb_geometry` | `R0`=`u32 out[3]` (width, height, format) | `RV`=0 |
| `$F8` | `sys_fb_register` | `R0`=base VA of a mapped pixel buffer | `RV`=slot index, or `-errno` |
| `$F9` | `sys_fb_release` | none | `RV`=0, or `-errno` |

`sys_fb_register` errno set: `-ENODEV` (19, no display attached / the device rejected the
claim), `-EBUSY` (16, the caller already holds a registration), `-EINVAL` (22, the base VA
is zero or its page is unmapped), `-ENOSPC` (28, the table is full). `sys_fb_release`
returns `-EBADF` (9) when the caller holds no registration. A registration is scoped to one
exec-lifetime: `fork` does not propagate it to the child, and both `execve` and process exit
release a held slot.

### Phase 3: unix sockets, select/poll, framebuffer mmap (maize-238)

quesOS Phase 3 adds the syscall gates TinyX and the borrowed window managers need per the
display architecture doc: AF_UNIX SOCK_STREAM sockets, `select`/`poll` readiness
multiplexing, and mmap of the framebuffer into user page tables. All are dispatched by
quesOS's cause-7 handler (guest-only); a bare-VM program calling them hits the native table
instead. The socket and poll/select numbers **mirror the Linux x86-64 table** (per the
numbering policy); `sys_fb_mmap` is Maize-private (`$FC`, the next free byte after the
`$F7`-`$FB` block) because there is no Linux syscall that maps a fixed hardware framebuffer
by fixed geometry.

| Number | Symbol | Linux x86-64 # | Args | Result |
|--------|--------|----------------|------|--------|
| `$29` | `sys_socket` | 41 | `R0`=domain, `R1`=type, `R2`=protocol | `RV`=fd or `-errno` |
| `$31` | `sys_bind` | 49 | `R0`=fd, `R1`=`sockaddr*`, `R2`=addrlen | `RV`=0 or `-errno` |
| `$2A` | `sys_connect` | 42 | `R0`=fd, `R1`=`sockaddr*`, `R2`=addrlen | `RV`=0 or `-errno` |
| `$32` | `sys_listen` | 50 | `R0`=fd, `R1`=backlog | `RV`=0 or `-errno` |
| `$2B` | `sys_accept` | 43 | `R0`=fd, `R1`=`sockaddr*` (nullable), `R2`=`socklen_t*` (nullable) | `RV`=newfd or `-errno` |
| `$35` | `sys_socketpair` | 53 | `R0`=domain, `R1`=type, `R2`=protocol, `R3`=`int sv[2]` | `RV`=0 or `-errno` |
| `$07` | `sys_poll` | 7 | `R0`=`struct pollfd*`, `R1`=nfds, `R2`=timeout_ms | `RV`=ready-count, `0` (timeout), or `-errno` |
| `$17` | `sys_select` | 23 | `R0`=nfds, `R1`=readfds, `R2`=writefds, `R3`=exceptfds, `R4`=`timeval*` | `RV`=ready-count, `0` (timeout), or `-errno` |
| `$FC` | `sys_fb_mmap` | (none, Maize-private) | none | `RV`=VA of the mapped fb buffer, or `-errno` |

**Sockets (Family A).** `AF_UNIX` (1) `SOCK_STREAM` (1) only; `protocol` must be 0. A
connected pair is two cross-wired ring buffers (full duplex); socket data I/O uses the
ordinary `read`/`write`/`close`. `socket` errno set: `-EAFNOSUPPORT` (97, domain not
AF_UNIX), `-EINVAL` (22, type not SOCK_STREAM, e.g. a `SOCK_NONBLOCK`/`SOCK_CLOEXEC` bit),
`-EPROTONOSUPPORT` (93, protocol != 0). `bind`: `-EADDRINUSE` (98, path already bound),
`-ENOSPC` (28, bind table full), `-EINVAL`. `connect`: `-ECONNREFUSED` (111, no listener /
not listening / backlog full). `accept` parks until a connector arrives. Closing a
listening socket wakes every still-parked connector with `-ECONNREFUSED` and frees the
path. Sockets follow the pipe fork/exec rules (refcounted fd-table sharing; survive
`execve`). No `sendto`/`recvfrom`/`shutdown`/`getsockname`/`setsockopt` (no named consumer;
maize-90 scoping). The `sys/socket.h` + `sys/un.h` wrapper headers match the Linux/glibc
shapes bit for bit so unmodified X11 client sources compile unchanged.

**select/poll (Family B).** A non-blocking readiness check runs first; if nothing is ready
the caller parks and is woken by a shared recheck sweep on any readiness-changing event, or
by the timer on a finite-timeout deadline. Readiness: fd 0 (console) is `POLLIN`-ready when
a byte is queued; a hostfs/stdout fd is always ready (Linux regular-file semantics); a
pipe/socket read side is readable with data or at EOF; a pipe/socket write side is writable
with room but reports `POLLERR` (not `POLLOUT`) once the peer read end is gone; a listening
socket is `POLLIN`-ready with a connection waiting. `poll` timeout: negative = block
forever, 0 = pure non-blocking, positive = milliseconds. `select` `timeval*`: NULL = block
forever, `{0,0}` = non-blocking, else bounded. `exceptfds` is accepted but never reports
ready (no OOB model). The `poll.h` + `sys/select.h` headers carry the real
`struct pollfd`/`POLL*` and 1024-bit `fd_set`/`FD_*` shapes.

**Framebuffer mmap (Family C).** `sys_fb_mmap` takes no arguments (geometry is fixed per
boot) and maps a physically-contiguous, page-aligned framebuffer buffer into the caller's
private fb-mmap window (`FB_MMAP_BASE`, a dedicated per-process 2 MiB region), returning its
VA. Idempotent: a second call while one buffer is mapped returns the same VA. `-ENOMEM`
(12) if the frame pool cannot satisfy the contiguous request (graceful, no PANIC). The
buffer persists for the process lifetime (released at address-space teardown; no
`fb_munmap`), and does not propagate across `fork` (the region is excluded from the eager
copy). Pass the returned VA to `sys_fb_register` for scanout; that call now validates the
whole `[base, base + width*height*4)` range for physical contiguity, returning `-EINVAL`
(22) on a mapped-but-scattered buffer.

## quesOS guest-OS display surface (maize-251)

These three calls let a quesOS graphical child (DOOM) present its registered framebuffer,
read the keyboard, and allocate a zone heap larger than the sbrk ceiling. They are the LAST
three numbers in the Maize-private high block, which they exhaust. All are guest-only
(dispatched by quesOS's cause-7 handler); a bare-VM `SYS $FD/$FE/$FF` hits the native table's
unmatched case (returns `0`, no memory touched), which the shim and `malloc` treat as a
transparent no-op.

| Number | Symbol | Args | Result |
|--------|--------|------|--------|
| `$FD` | `sys_fb_present` | none | `RV`=0, or `-EBADF` (9) if the caller holds no registration |
| `$FE` | `sys_kbd_read` | none | `RV`=scancode (0-255), `-EAGAIN` (11, none pending), or `-EACCES` (13, not the active-slot owner) |
| `$FF` | `sys_bigalloc` | `R0`=size in bytes | `RV`=VA of a fresh zeroed mapping, or `-errno` |

**Present (`$FD`).** A quesOS process cannot issue the privileged `OUT $54` present itself, so
this is its only path to the present step (and, under the maize-264 session-host transport, to
making a frame visible to a live presenter). It selects the caller's registered slot and
presents; `-EBADF` if the caller holds none. `present_into`'s write-destination (private
capture vector vs. shared presenter frame region) is entirely host-side; the guest contract is
unchanged from bare metal.

**Keyboard read (`$FE`).** Combined status+read (one trap round-trip instead of a poll pair),
gated to the process owning the currently-active fb slot. The physical `keyboard_device` is one
shared queue, so an ungated read would let a background graphical process steal the foreground
slot's scancodes. Order is status-first: `-EAGAIN` when no scancode is pending; otherwise
`-EACCES` for a non-owner (the scancode is left latched for the real owner), else the Set-1
scancode. quesOS installs a keyboard-IRQ (vector 34) sink so a delivered key event does not halt
the VM; the sink leaves the scancode latched for this poll.

**Big alloc (`$FF`).** A per-process bump allocator over a dedicated 16 MiB VA window
(`BIGALLOC_BASE`), for allocations too large for the sbrk heap ceiling (`USER_BRK_MAX`, ~960
KiB) -- DOOM's ~6 MiB zone heap in particular. `malloc` falls back to it when sbrk growth is
refused (transparent no-op under bare-VM). `size` 0 is `-EINVAL` (22); a request past the
per-process window is `-ENOMEM` (12); frame-pool exhaustion is also `-ENOMEM` (graceful, no
PANIC). The mapping is fresh, zeroed, and physically contiguous; it is NOT inherited across
`fork` (the window is excluded from the eager copy, `bigalloc_next` reset in the child) and NOT
reclaimed on exit/exec (the accepted maize-238 bump-no-free precedent).
