# hostfs: a capability-granted *nix view of the host filesystem

Status: design of record (maize-113). Contract every later hostfs card implements
against. The proof-of-concept code ships under **maize-114** (gated behind this
card); no guest-visible code ships on maize-113.

hostfs is the "run against my real files" mode. It mounts host directories
(Windows, Linux, later macOS) into the guest's *nix-shaped namespace and operates
on them through the Linux-mirroring file syscalls (the maize-90 subset atop the
existing `read`/`write`). It unblocks `ls`/`cat`-grade userland (the maize-90
porting ladder) before any disk image (maize-22) exists, and it is complementary
to maize-22: hostfs is the host-files mode, maize-22 is the disk-image
filesystem.

The security posture is the WASI-preopen capability model already ratified on
maize-74/75, applied to files: **nothing is mounted by default**, and each
`--mount` grant is an explicit, read-only-unless-opted preopen. Confinement
(proving every resolution stays inside its mount root) is host-specific and lives
in the backend layer, never in the portable core (comment 2226, binding).

Where a surface below is byte-level binary ABI (syscall numbers, `struct stat`,
`struct linux_dirent64`, open flags, whence), the exact value/offset stated here
is what freezes into the ABI. These are hard-frontier: they must match the
x86-64 kernel layout that both musl- and glibc-shaped recompiled userland consume
at the syscall boundary. All numbers below are fixed x86-64 values grounded
against `src/sys.cpp`, `toolchain/rt/SYSCALL-ABI.md`, `toolchain/rt/syscall.h`,
and `toolchain/rt/errno.h`; nothing is invented.

The nine contract surfaces:

1. Mount-grant CLI contract
2. Syscall subset and binary-ABI structures
3. Guest namespace rules
4. Confinement (backend layer)
5. Per-host semantic-deviations table
6. Freestanding-C99 VFS core boundary
7. Provider-migration terminal form (9P device sketch)
8. Consistency with maize-114 (the POC)
9. Design-doc home and ABI landing

---

## 1. Mount-grant CLI contract

### Grammar

```
--mount <host-path>=<guest-path>[:ro|:rw]
--mount-home[=<host-home>]
```

- `<host-path>` accepts a native Windows path (`C:\work`, `C:/work`) or a POSIX
  path (`/home/paul/proj`). `<guest-path>` is ALWAYS a *nix absolute path under
  the synthetic root (for example `/proj`).
- Posture is **read-only unless `:rw` is given**. The `:ro` suffix is the
  explicit default and may be written for clarity.
- **Nothing is mounted without an explicit grant.** Absent any `--mount` /
  `--mount-home` flag, the guest sees only the synthetic root with no entries
  (WASI-preopen capability model, ratified maize-74/75).
- `--mount-home` is sugar mapping the host home to `/home/user`. The host home is
  derived from `HOME` (POSIX) or `USERPROFILE` (Windows), or an explicit
  `--mount-home=<host-home>` override. It is mounted **read-write** (operator
  ruling 2026-07-12, OQ 7790): typing the flag is itself the explicit capability
  grant, and a read-only dev home defeats the flag's purpose. This is the one
  documented rw-by-default convenience; plain `--mount` grants keep the `:ro`
  default.

### Examples (both required)

Linux:

```
maize --mount /home/paul/proj=/proj:ro \
      --mount /var/data=/data:rw \
      --mount-home \
      program.mzx
```

Windows:

```
maize --mount C:\work\src=/src:ro ^
      --mount C:/scratch=/scratch:rw ^
      --mount-home=C:\Users\paul ^
      program.mzx
```

### Fail-closed startup errors

A grant is rejected before the VM runs (the process exits nonzero with a
diagnostic; the guest never starts) when:

- the `<host-path>` does not exist or is not a directory (**missing host path**);
- the `<guest-path>` is not a *nix absolute path (does not begin with `/`, or
  contains a drive letter / backslash) (**non-absolute guest path**);
- two grants name the same `<guest-path>`, or one grant's `<guest-path>` is a
  path-prefix of another's (**overlapping guest path**) so resolution would be
  ambiguous;
- the `<guest-path>` is `/` itself (the root is synthetic and cannot be a mount
  target, see section 3).

Fail closed: an unparseable or conflicting grant never degrades to "mount
nothing" or "mount read-write"; it stops startup.

---

## 2. Syscall subset (Linux x86-64 numbering)

Args in `R0..R9`, result in `RV`. A result in `[-4095, -1]` encodes `-errno`
(the frozen convention, `toolchain/rt/SYSCALL-ABI.md`; `MAX_ERRNO` = 4095).
Everything else is a valid result. Pointer args are guest virtual addresses; the
provider copies path strings in with a bounded length (untrusted parser input,
maize-79 fuzzing discipline). The fd argument is a 32-bit C `int` carried in
`R0.H0`, exactly as the existing `read`/`write` dispatch reads it (`src/sys.cpp`,
`regs::r0.h0`); address/count args are full-64 (`regs::rN.w0`, maize-56).

The dispatcher is `maize::sys::call` in `src/sys.cpp` (the `case`-per-number
switch), delegating to the `maize::syscall::*` backend helpers. This document
uses `sys::call` for that dispatcher throughout; there is no `syscall::call`.

### Classic path-based calls (maize-90 subset, atop read 0x00 / write 0x01)

| Call | Num | Args (reg) | Returns |
|------|-----|-----------|---------|
| `open` | 0x02 | path `R0`, flags `R1`, mode `R2` | fd, or `-errno` |
| `close` | 0x03 | fd `R0` | 0 / `-errno` |
| `stat` | 0x04 | path `R0`, statbuf `R1` | 0 / `-errno` |
| `fstat` | 0x05 | fd `R0`, statbuf `R1` | 0 / `-errno` |
| `lseek` | 0x08 | fd `R0`, offset `R1`, whence `R2` | new offset / `-errno` |
| `rename` | 0x52 | oldpath `R0`, newpath `R1` | 0 / `-errno` |
| `mkdir` | 0x53 | path `R0`, mode `R1` | 0 / `-errno` |
| `rmdir` | 0x54 | path `R0` | 0 / `-errno` |
| `unlink` | 0x57 | path `R0` | 0 / `-errno` |
| `getdents64` | 0xD9 | fd `R0`, dirp `R1`, count `R2` | bytes / `-errno` |

### The *at family (canonical implementation)

Operator ruling 2026-07-12 (OQ 7791 / decision 7815): the frozen hostfs ABI
carries **both** syscall families. The `*at` family is the canonical
implementation; the classic path calls above are thin shims over it
(`dirfd = AT_FDCWD`, `flags = 0`), exactly how the Linux kernel itself structures
the pair. This makes both musl- and glibc-shaped recompiled userland work
(modern glibc emits the `*at` forms) without a libc shim layer.

| Call | Num | Args (reg) | Classic shim equivalence |
|------|-----|-----------|--------------------------|
| `openat` | 0x101 | dirfd `R0`, path `R1`, flags `R2`, mode `R3` | `open(p,fl,m)` = `openat(AT_FDCWD,p,fl,m)` |
| `mkdirat` | 0x102 | dirfd `R0`, path `R1`, mode `R2` | `mkdir(p,m)` = `mkdirat(AT_FDCWD,p,m)` |
| `newfstatat` | 0x106 | dirfd `R0`, path `R1`, statbuf `R2`, flags `R3` | `stat(p,b)` = `newfstatat(AT_FDCWD,p,b,0)` |
| `unlinkat` | 0x107 | dirfd `R0`, path `R1`, flags `R2` | `unlink(p)` = `unlinkat(AT_FDCWD,p,0)`; `rmdir(p)` = `unlinkat(AT_FDCWD,p,AT_REMOVEDIR)` |
| `renameat` | 0x108 | olddirfd `R0`, oldpath `R1`, newdirfd `R2`, newpath `R3` | `rename(o,n)` = `renameat(AT_FDCWD,o,AT_FDCWD,n)` |

`fstat` (0x05) has no path and needs no `*at` twin: it operates on an already
resolved fd. `newfstatat` with `AT_EMPTY_PATH` (0x1000) and an empty path is the
`fstat`-equivalent form and resolves to the fd's backend handle directly.

Relevant `AT_*` constants (x86-64 fixed values):

| Constant | Value | Meaning |
|----------|-------|---------|
| `AT_FDCWD` | -100 | dirfd sentinel: resolve relative to the synthetic cwd |
| `AT_SYMLINK_NOFOLLOW` | 0x100 | do not follow a trailing symlink |
| `AT_REMOVEDIR` | 0x200 | `unlinkat` removes a directory (the `rmdir` path) |
| `AT_EMPTY_PATH` | 0x1000 | operate on `dirfd` itself when path is empty |

Because the guest namespace is a single synthetic root (section 3), `AT_FDCWD`
resolves against `/`; an absolute guest path ignores `dirfd` as on Linux. A
non-`AT_FDCWD` `dirfd` with a relative path resolves relative to that open
directory fd's backend handle (deferred past the POC, section 8).

### No collision with the frozen set

None of the numbers above collides with the existing frozen ABI numbers
(`toolchain/rt/SYSCALL-ABI.md`, frozen as of maize-74):

| Number | Frozen symbol |
|--------|---------------|
| 0x00 | `read` |
| 0x01 | `write` |
| 0x0C | `brk` |
| 0x3C | `exit` |
| 0xA9 | `reboot` (reserved) |

The hostfs numbers (0x02, 0x03, 0x04, 0x05, 0x08, 0x52, 0x53, 0x54, 0x57, 0xD9,
0x101, 0x102, 0x106, 0x107, 0x108) are disjoint from {0x00, 0x01, 0x0C, 0x3C,
0xA9}. Every one mirrors its Linux x86-64 number by construction, per the
numbering policy in SYSCALL-ABI.md ("mirror the Linux x86-64 number for any call
that has a Linux equivalent").

### Guest fd table

A **guest fd table** maps small-int guest fds to backend handles (a host `int`
fd on POSIX, a `HANDLE` on Win32, or a synthetic-root cursor). `open`/`openat`
returns the lowest free guest fd >= 3; `close` frees it. fds 0/1/2 remain the
stdio reservations (`src/sys.cpp` already special-cases them and returns
`-EBADF` for real-file use of fd >= 3 today; hostfs is what lifts that M4
restriction). A `getdents64`/`fstat`/`lseek` on an unknown or closed guest fd
returns `-EBADF` (9).

### Binary-ABI structures (exact offsets/sizes, x86-64 kernel layout)

**`struct stat`** pinned to the x86-64 kernel layout (`asm/stat.h`), as returned
by the raw `stat`/`fstat`/`newfstatat` syscalls, consumed identically by musl and
glibc at the syscall boundary (operator ruling 2026-07-12, OQ 7792). Total size
144 bytes; all fields naturally aligned; little-endian:

| Offset | Size | Field | Type |
|--------|------|-------|------|
| 0 | 8 | `st_dev` | u64 |
| 8 | 8 | `st_ino` | u64 |
| 16 | 8 | `st_nlink` | u64 |
| 24 | 4 | `st_mode` | u32 |
| 28 | 4 | `st_uid` | u32 |
| 32 | 4 | `st_gid` | u32 |
| 36 | 4 | `__pad0` | u32 |
| 40 | 8 | `st_rdev` | u64 |
| 48 | 8 | `st_size` | s64 |
| 56 | 8 | `st_blksize` | s64 |
| 64 | 8 | `st_blocks` | s64 |
| 72 | 8 | `st_atim.tv_sec` | s64 |
| 80 | 8 | `st_atim.tv_nsec` | s64 |
| 88 | 8 | `st_mtim.tv_sec` | s64 |
| 96 | 8 | `st_mtim.tv_nsec` | s64 |
| 104 | 8 | `st_ctim.tv_sec` | s64 |
| 112 | 8 | `st_ctim.tv_nsec` | s64 |
| 120 | 24 | `__unused[3]` | s64[3] |

Note the x86-64 ordering quirk: `st_nlink` precedes `st_mode` (unlike the generic
32-bit layout). The provider writes these fields at these exact offsets; it must
NOT rely on a host `struct stat` having the same layout (Windows and macOS
`struct stat` differ), it composes the bytes explicitly.

`st_mode` type bits (the high nibble; OR'd with the permission bits):

| Constant | Value (octal) | Meaning |
|----------|---------------|---------|
| `S_IFMT` | 0170000 | type-field mask |
| `S_IFSOCK` | 0140000 | socket |
| `S_IFLNK` | 0120000 | symlink |
| `S_IFREG` | 0100000 | regular file |
| `S_IFBLK` | 0060000 | block device |
| `S_IFDIR` | 0040000 | directory |
| `S_IFCHR` | 0020000 | char device |
| `S_IFIFO` | 0010000 | FIFO |

**`struct linux_dirent64`** for `getdents64`. Records are variable-length and
packed back to back in the caller buffer; each `d_reclen` is rounded up so the
next record begins on an 8-byte boundary. `d_name` is NUL-terminated and its
declared length is included in `d_reclen`:

| Offset | Size | Field | Type |
|--------|------|-------|------|
| 0 | 8 | `d_ino` | u64 |
| 8 | 8 | `d_off` | s64 |
| 16 | 2 | `d_reclen` | u16 |
| 18 | 1 | `d_type` | u8 |
| 19 | .. | `d_name[]` | char[], NUL-terminated |

`d_off` is the opaque cookie the kernel uses to resume the directory stream; the
provider fills it with the byte offset of the next record (or a monotonic
sequence cookie) and treats it as opaque on the read side. `getdents64` returns
the total bytes written; 0 at end of stream; `-EINVAL` (22) if the buffer is too
small for even one record.

`d_type` values (fixed): `DT_UNKNOWN` 0, `DT_FIFO` 1, `DT_CHR` 2, `DT_DIR` 4,
`DT_BLK` 6, `DT_REG` 8, `DT_LNK` 10, `DT_SOCK` 12.

**open flags** (x86-64 fixed values; octal as in `<asm/fcntl.h>`):

| Flag | Octal | Hex |
|------|-------|-----|
| `O_RDONLY` | 0 | 0x0 |
| `O_WRONLY` | 01 | 0x1 |
| `O_RDWR` | 02 | 0x2 |
| `O_CREAT` | 0100 | 0x40 |
| `O_EXCL` | 0200 | 0x80 |
| `O_TRUNC` | 01000 | 0x200 |
| `O_APPEND` | 02000 | 0x400 |
| `O_DIRECTORY` | 0200000 | 0x10000 |
| `O_NOFOLLOW` | 0400000 | 0x20000 |
| `O_CLOEXEC` | 02000000 | 0x100000 |

The low two bits are the access mode (`O_RDONLY`/`O_WRONLY`/`O_RDWR`); the rest
are OR'd flag bits. Any write-intent bit (`O_WRONLY`, `O_RDWR`, `O_CREAT`,
`O_TRUNC`, `O_APPEND`) on a `:ro` mount fails with `EROFS` (section 4).

**`lseek` whence**: `SEEK_SET` 0, `SEEK_CUR` 1, `SEEK_END` 2. An unknown whence
returns `-EINVAL` (22); a resulting negative offset returns `-EINVAL`.

---

## 3. Guest namespace rules

- **Synthetic read-only `/`.** The root is a virtual directory whose entries are
  exactly the granted mount points. `stat("/")` returns a synthetic directory
  (`S_IFDIR`, mode `0555`, `st_ino` a fixed synthetic value). `getdents64` on an
  fd opened on `/` enumerates the granted mounts as directory entries (`d_type`
  `DT_DIR`). Creating or removing an entry directly at `/` (outside any mount),
  or any write-intent op on `/`, fails: `mkdir("/x")` and `unlink("/x")` return
  `-EROFS` (30); the root itself is never writable.
- **Prefix resolution.** Slash separators. A guest path is matched against the
  granted guest-path prefixes; the longest matching prefix wins, and the
  remainder is resolved relative to that mount's host root. Because overlapping
  guest prefixes are rejected at startup (section 1), at most one mount matches.
- **Unmounted paths return `ENOENT`.** A path that falls under no granted mount
  returns `-ENOENT` (2): the capability is simply absent, indistinguishable (by
  design) from a path that does not exist inside a mount. Absence of a grant is
  not a distinct error class the guest can probe.
- **Case posture.** Case-sensitive treatment. On a case-insensitive host
  filesystem (Windows NTFS default, macOS APFS default) resolution **follows the
  host** and the deviation is documented per mount (section 5). Do NOT fake
  case-sensitivity by layering a case-folding index over an insensitive host
  (the WSL1 DrvFs lesson): a lookup for `Makefile` on a Windows mount that holds
  `makefile` resolves to the host file, and the per-mount deviation note records
  that behavior.
- **`/home/user`.** Maps to the host home when `--mount-home` is granted
  (read-write, section 1); absent the flag, `/home/user` is simply an unmounted
  path and returns `-ENOENT`.

---

## 4. Confinement (enforced in the BACKEND layer, not the VFS core)

Comment 2226 is binding: confinement is host-specific and lives entirely in the
POSIX and Win32 backend implementations behind the ops struct (section 6), never
in the portable core. Every path resolution must prove it stays inside its mount
root against all three escape vectors:

| Vector | Linux mechanism | Windows mechanism |
|--------|-----------------|-------------------|
| `..` traversal | `openat2` with `RESOLVE_BENEATH` | `GetFinalPathNameByHandle` canonicalize + prefix-verify against the mount root |
| host symlink escape | `RESOLVE_BENEATH` (rejects any `..` or symlink that leaves the anchor); `RESOLVE_NO_SYMLINKS` when a mount forbids symlinks entirely | reparse-point policy (below) |
| Windows junction / reparse point | n/a | explicit reparse policy: refuse the reparse, or resolve-and-verify the final path is still beneath the mount root |

**Operator ruling 2026-07-12 (OQ 7793): the Linux backend hard-requires
`openat2(RESOLVE_BENEATH)`, kernel >= 5.6.** hostfs fails at startup with a clear
error on an older kernel. No `realpath` canonicalize-and-verify-prefix fallback
ships: that fallback carries a TOCTOU window between the canonicalize and the
open, which is exactly the race class `RESOLVE_BENEATH` exists to eliminate.
Revisit only if a real user hits the 5.6 floor.

`openat2` takes `struct open_how { __u64 flags; __u64 mode; __u64 resolve; }`
with `resolve = RESOLVE_BENEATH` (0x08). Relevant resolve bits: `RESOLVE_NO_XDEV`
0x01, `RESOLVE_NO_MAGICLINKS` 0x02, `RESOLVE_NO_SYMLINKS` 0x04, `RESOLVE_BENEATH`
0x08, `RESOLVE_IN_ROOT` 0x10. The mount root is held as an O_PATH/O_DIRECTORY
anchor fd opened once at mount time; every guest open is an `openat2` relative to
that anchor with `RESOLVE_BENEATH`, so a `..` or symlink that would escape fails
in the kernel rather than in a userspace check.

On Windows the mount root is opened once; each resolution opens the target with
`FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT` as needed, calls
`GetFinalPathNameByHandle` to canonicalize, and verifies the canonical path is a
prefix-child of the mount root's canonical path before handing back a handle. A
reparse point (junction, symlink) is subject to the reparse policy: the default
is refuse (return `-EACCES`); a per-mount opt-in resolves and re-verifies beneath
the root.

### Errno contract (matches maize-114 acceptance)

| Condition | errno | Value |
|-----------|-------|-------|
| attempt to escape the mount root (`..`, host symlink, junction) | `EACCES` | 13 |
| path that simply does not exist (incl. unmounted paths) | `ENOENT` | 2 |
| any write-intent op on a `:ro` mount | `EROFS` | 30 |

Write-intent ops that trigger `EROFS` on a `:ro` mount: `write`; `open`/`openat`
with `O_WRONLY`/`O_RDWR`/`O_CREAT`/`O_TRUNC`/`O_APPEND`; `mkdir`/`mkdirat`;
`rmdir` (`unlinkat AT_REMOVEDIR`); `unlink`/`unlinkat`; `rename`/`renameat`.

Guest-controlled paths are untrusted parser input: path copy-in (bounded length)
and resolution inherit the maize-79 fuzzing discipline and trust-boundary
documentation. A path longer than the bound returns `-ENAMETOOLONG` (36); a
malformed / non-UTF-8 path on a host that requires valid encoding returns
`-EINVAL` (22) rather than being passed through.

---

## 5. Per-host semantic-deviations table

Each cell states the concrete behavior. Where a host cannot honor a Linux
semantic natively, the deviation is documented rather than faked.

| Aspect | Windows | Linux | macOS |
|--------|---------|-------|-------|
| case sensitivity | insensitive by default; resolution follows the host, per-mount deviation noted; no faked sensitivity | case-sensitive passthrough | insensitive by default (APFS/HFS+); follows the host, per-mount deviation noted; case-sensitive APFS volumes pass through |
| inode source (`st_ino`) | NTFS 64-bit File ID (`FILE_ID_INFO` / `nFileIndex`) | native inode number | native inode number |
| delete / rename semantics | `FILE_SHARE_DELETE` + POSIX-semantics delete (`FILE_DISPOSITION_POSIX_SEMANTICS`) so an open file can be unlinked; rename is `SetFileInformationByHandle(FileRenameInfoEx)` with POSIX-rename where supported | native `unlink`/`rename` (rename is atomic within a mount) | native `unlink`/`rename` |
| permission model (`st_mode` low bits) | synthesized: read-only attribute maps to `0444`, else `0644`; directories `0755`; no real uid/gid/mode | passthrough of real `st_mode`/`st_uid`/`st_gid` | synthesized like Windows for the low bits; native where meaningful |
| reserved names / illegal chars | reject/deviate on `CON`, `PRN`, `AUX`, `NUL`, `COM1..9`, `LPT1..9`, trailing dot or space, and the set `< > : " / \ | ? *`; a guest path containing one that cannot map returns `-EINVAL` | none beyond `/` and NUL | none beyond `/` and NUL (`:` historically special, not enforced) |
| symlink / reparse handling | reparse points (junctions, symlinks) subject to the reparse policy (section 4): default refuse, opt-in resolve-and-verify-beneath | symlinks followed under `RESOLVE_BENEATH`; `O_NOFOLLOW`/`AT_SYMLINK_NOFOLLOW` honored; `RESOLVE_NO_SYMLINKS` per mount | symlinks as Linux; macOS firmlinks treated as the host resolves them, verified beneath the root |

The full-fidelity form of this table (every permission-bit mapping, every
reserved-name behavior) is owned by later cards; the POC implements only the
subset in section 8.

---

## 6. Freestanding-C99 VFS core boundary

Comment 2226 is binding: a single **freestanding-C99 header** (proposed
`hostfs.h`) defines the core. **No VM includes, no host includes** in this
header. That is what lets the native C++ VM, the reference C VM (maize-87), and
future firmware/quesito/quesOS providers all consume the identical core. Only
fixed-width integer typedefs (a local `hostfs_u64` etc., or `<stdint.h>` if the
freestanding target provides it) and the declarations below appear; no
`<windows.h>`, no `<fcntl.h>`, no `maize.h`.

The header declares three things:

### (a) Backend operations struct

A struct of function pointers covering the full subset plus a resolve/confine
hook the backend owns. The confine hook is where the host-specific escape defense
of section 4 lives; the core calls it but never implements it.

```c
/* hostfs.h -- freestanding C99; no VM or host headers. */

typedef struct hostfs_mount hostfs_mount;   /* section (b) */
typedef struct hostfs_stat  hostfs_stat;    /* the section-2 struct stat image */
typedef struct hostfs_dirent hostfs_dirent; /* the section-2 linux_dirent64 image */

typedef struct hostfs_backend_ops {
    /* Resolve/confine: prove guest_path stays beneath mount->host_root and
       hand back an opaque backend handle, or a negative errno. THIS is where
       openat2(RESOLVE_BENEATH) / GetFinalPathNameByHandle live -- backend only. */
    long (*confine)(hostfs_mount *mount, const char *guest_path,
                    int flags, void **out_handle);

    long (*open) (hostfs_mount *mount, const char *path, int flags, int mode);
    long (*close)(void *handle);
    long (*read) (void *handle, void *buf, unsigned long count);
    long (*write)(void *handle, const void *buf, unsigned long count);
    long (*lseek)(void *handle, long offset, int whence);
    long (*stat) (hostfs_mount *mount, const char *path, hostfs_stat *out);
    long (*fstat)(void *handle, hostfs_stat *out);
    long (*getdents)(void *handle, void *buf, unsigned long count);
    long (*mkdir) (hostfs_mount *mount, const char *path, int mode);
    long (*rmdir) (hostfs_mount *mount, const char *path);
    long (*unlink)(hostfs_mount *mount, const char *path);
    long (*rename)(hostfs_mount *mount, const char *oldp, const char *newp);
} hostfs_backend_ops;
```

Every op returns a `long`: a non-negative result on success (fd, byte count, new
offset, or 0), or a value in `[-4095, -1]` encoding `-errno`, matching the VM
result convention exactly so the core can pass it straight back to `RV`.

### (b) Mount table shape

```c
typedef enum { HOSTFS_RO = 0, HOSTFS_RW = 1 } hostfs_mode;

struct hostfs_mount {
    const char  *guest_prefix;   /* nix absolute, e.g. "/proj" */
    const char  *host_root;      /* native host path, opaque to the core */
    hostfs_mode  mode;           /* ro / rw; write-intent on RO -> EROFS */
    void        *anchor;         /* backend-owned root handle (anchor fd / HANDLE) */
};

typedef struct hostfs_table {
    hostfs_mount            *mounts;   /* sorted; longest-prefix match */
    unsigned                 count;
    const hostfs_backend_ops *ops;     /* the active backend */
} hostfs_table;
```

The core does prefix matching, the `:ro`/`:rw` write-intent gate (`EROFS`), the
synthetic-root enumeration, and the guest fd table; it delegates every actual
host touch (including confinement) to `ops`.

### (c) Linux-numbered errno constants

So nothing pulls in VM or host headers, the core defines the errno codes it
returns, mirroring Linux x86-64 (the same numbering as `toolchain/rt/errno.h`,
extended for the file surface):

```c
#define HOSTFS_EPERM      1
#define HOSTFS_ENOENT     2
#define HOSTFS_EIO        5
#define HOSTFS_EBADF      9
#define HOSTFS_ENOMEM    12
#define HOSTFS_EACCES    13
#define HOSTFS_EEXIST    17
#define HOSTFS_ENOTDIR   20
#define HOSTFS_EISDIR    21
#define HOSTFS_EINVAL    22
#define HOSTFS_EROFS     30
#define HOSTFS_ENAMETOOLONG 36
#define HOSTFS_ENOSYS    38
#define HOSTFS_ENOTEMPTY 39
#define HOSTFS_ELOOP     40
```

These match the runtime table (`toolchain/rt/errno.h`: `EPERM` 1, `EBADF` 9,
`ENOMEM` 12, `EINVAL` 22) and extend it with the file-surface codes the POSIX
translation table names.

### Native C++ provider consumes this header

The native C++ provider is `src/sys.cpp`, today's `sys::call` dispatch (the
`case`-per-number switch, delegating to `maize::syscall::*` helpers). It gains a
`case` per hostfs number (section 2) that copies the path/args out of guest
memory, then calls into a `hostfs_table` whose `ops` is the POSIX or Win32
backend selected at build time, exactly the `#ifdef __linux__ / #elif _WIN32`
split already present in `src/sys.cpp` (the maize-38 native-I/O split precedent).
Confinement lives entirely in those backend `confine`/`open`/`stat` bodies, never
in `sys::call` and never in `hostfs.h`.

---

## 7. Provider-migration terminal form (design sketch only)

Syscall-level passthrough in the native provider is today's form. The durable
terminal shape is a **9P-flavored host-files device** in the maize-83 device
family, so firmware/quesito/quesOS providers retain host access under maize-82's
provider-migration staging.

Sketch level only (this defers the protocol detail to maize-83's family):

- The host-files access becomes a device in the maize-83 device model rather than
  a set of VM-privileged syscalls. The guest kernel/runtime speaks a
  9P-flavored request/response protocol (walk / open / read / write / clunk
  shaped operations) to that device over the device transport.
- The `hostfs_backend_ops` struct of section 6 is deliberately shaped to sit
  behind **either** a direct syscall provider (today) **or** a 9P device
  transport (terminal): the ops are per-handle verbs (walk-to-handle,
  read/write/stat/getdents on a handle, clunk) that map cleanly onto 9P messages.
  A provider migration re-homes the ops implementation from in-process host calls
  to device-message round-trips without changing the core or the guest-visible
  syscall ABI.
- Confinement remains a backend concern in both forms: the syscall backend
  enforces it with `openat2`/`GetFinalPathNameByHandle`; the device backend
  enforces the same mount-root anchoring inside the device.

Protocol framing, message numbering, and the device register/DMA shape are
maize-83's to specify; this document only fixes that the VFS ops struct is the
seam both forms share.

---

## 8. Consistency with maize-114 (the POC)

maize-114's description draws this line; this section matches it exactly. This
card `blocks` maize-114 (link 540); the server auto-materialized a Build-Queue
dependency gate on maize-114, so the POC cannot leave Build Queue until this
design lands.

**maize-114 MAY implement:**

- `--mount` and `--mount-home` CLI grants;
- the guest fd table;
- `open`/`close`/`lseek`/`fstat`/`getdents64` beside the existing `read`/`write`
  in the native provider;
- path-prefix confinement with canonicalization on **both** hosts (Linux and
  Windows);
- one backend interface with POSIX + Win32 implementations.

Acceptance: a cproc-built C program (via mzcc) that cats a file from a `:ro`
mount and lists a directory, on Windows **and** Linux; a `..`/symlink escape
fails with `EACCES`/`ENOENT`; a write to a `:ro` mount fails with `EROFS`.

**maize-114 MAY NOT implement (owned here, specified for later cards):**

- permissions fidelity;
- the full per-host deviations table;
- path-based `stat`;
- `mkdir`, `rmdir`, `unlink`, `rename`;
- the 9P-style device shape.

---

## 9. Design-doc home and ABI landing

This design document lands at `docs/design/hostfs.md` (the new `docs/design/`
tree; operator decision 7797). hostfs is cross-cutting (VM provider + syscall ABI
+ device roadmap) and larger than an ABI addendum, so it gets its own design doc;
`toolchain/rt/SYSCALL-ABI.md` stays the home for the frozen numbers per its
existing scope.

This design doc is not itself binary ABI. The syscall numbers, `struct stat` /
`struct linux_dirent64` layouts, open flags, and whence values it pins become ABI
when a POC (maize-114 and successors) implements them; at that point the frozen
numbers/errno additions also land in `toolchain/rt/SYSCALL-ABI.md` (its existing
home for frozen numbers), mirroring the Linux x86-64 table exactly as this
document does.
