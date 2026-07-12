# Appendix C: Syscall Surface (Informative)

*This appendix is informative. The syscall ABI is a separate contract; this appendix
summarizes the currently-implemented surface for orientation. The authoritative documents
are `toolchain/qbe-maize/CALLING-CONVENTION.md` (the C calling convention) and
`toolchain/rt/SYSCALL-ABI.md` (the C-to-syscall binding).*

## C.1 The C calling convention

- The first six integer/pointer arguments pass in **R0..R5**, left to right. Under the
  Zfinx floating-point model a `float`/`double` argument uses the same integer argument
  registers (a `float` in H0, a `double` in W0); there is no separate FP argument class.
- Arguments past R5 are pushed on the stack right-to-left in 8-byte slots.
- Results are returned in **RV** (a `float` in RV.H0, a `double` in RV.W0). RV is distinct
  from R0.
- **R0..R5 and RV are caller-saved; R6..R9 are callee-saved.** RT is back-end scratch (not
  register-allocatable). RB/BP is the frame pointer (callee-saved, established by the
  prologue); RS/SP is the stack pointer; RP/PC and RF are fixed-role.
- **R9** is the thread pointer by convention (callee-saved, never an argument; Chapter 12).
- The stack is full-descending, 8-byte slots, 8-byte alignment at call boundaries. CALL
  pushes an 8-byte return address. The standard prologue is `PUSH BP; CP SP BP; SUB
  framesize SP`; the epilogue is `CP BP SP; POP BP; RET`.

## C.2 The SYS instruction and the syscall boundary

`SYS` executes a system call with the syscall index in its operand (`SYS $01` or `SYS Rn`).
The index is a single byte (`operand.b0`), so the id space is `$00`..`$FF`. Syscall arguments
follow the same register convention (R0, R1, R2, ...), and the result is placed in RV.
`SYS` is privileged and is trap-class (Chapter 10 reserves it as cause 7); today the
reference VM dispatches it directly to the BIOS / syscall surface.

An alternative `INT $80` path (syscall number in R9, raise the interrupt) is the planned
OS-level surface and is not implemented yet; the implemented path is `SYS`.

## C.3 The implemented syscall numbers

Maize mirrors Linux x86-64 numbers where an analog exists:

| SYS | Name | Args | Returns |
|:---:|:-----|:-----|:--------|
| `$00` | sys_read | R0=fd, R1=buf, R2=count | RV = bytes read, `-errno` on error |
| `$01` | sys_write | R0=fd, R1=buf, R2=count | RV = bytes written, `-errno` on error |
| `$02` | sys_open | R0=path, R1=flags, R2=mode | RV = fd, `-errno` on error |
| `$03` | sys_close | R0=fd | RV = 0, `-errno` on error |
| `$05` | sys_fstat | R0=fd, R1=statbuf | RV = 0, `-errno` on error |
| `$08` | sys_lseek | R0=fd, R1=offset, R2=whence | RV = new offset, `-errno` on error |
| `$0C` | sys_brk | R0=requested break (0 queries) | RV = the current break (never `-errno`) |
| `$3C` | sys_exit | R0=code | does not return; low 8 bits become the exit status |
| `$A9` | sys_reboot | (none) | (reserved) |
| `$D9` | sys_getdents64 | R0=fd, R1=dirp, R2=count | RV = bytes read, 0 at end of directory, `-errno` on error |

For `sys_read` / `sys_write` the count in R2 is read as a full 64-bit value. The file
syscalls (`$02`/`$03`/`$05`/`$08`/`$D9`, and read/write on fds >= 3) are guest-visible only
when the program was started with a `--mount` / `--mount-home` grant; without a grant the
guest filesystem is empty and only the stdio fds exist.

## C.4 The error convention

Errors follow the Linux/musl convention: a result in `[-4095, -1]` encodes `-errno`, and
everything else is a valid result. The C runtime's wrapper layer translates that into the
familiar `errno` + `-1` return.

## C.5 sys_exit versus HALT

`sys_exit` (`SYS $3C`) is the status-carrying termination path: it records the low 8 bits of
R0 as the process exit status and stops the VM, so the host process returns that value (codes
wrap to 0..255). HALT (`$00`) halts the core pending an interrupt and records **no** status,
so a program that ends via HALT exits 0. See Chapter 9.
