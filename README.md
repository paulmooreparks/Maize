# The Maize Virtual Machine

<img src="logo/logo_256x256.png" align="right" width="256" alt="Maize logo">

[![CI](https://github.com/paulmooreparks/Maize/actions/workflows/ci.yml/badge.svg)](https://github.com/paulmooreparks/Maize/actions/workflows/ci.yml)
![platforms](https://img.shields.io/badge/platforms-Windows%20%7C%20Linux-blue)
![C++](https://img.shields.io/badge/C%2B%2B-20-blue)
[![License](https://img.shields.io/badge/license-Apache%202.0-blue)](LICENSE)

This project implements a 64-bit virtual machine called "Maize." This is an outgrowth of my [Tortilla](https://github.com/paulmooreparks/Tortilla)
project, which began life as an x86 emulator implemented in C# on .NET and then later became a virtual CPU of my own making.

Maize today is a working computer, not just a byte-code interpreter. The [instruction set](https://paulmooreparks.github.io/Maize/) is
frozen at v1.0 and fully specified, the toolchain (assembler, linker, disassembler, C compiler)
is complete end to end, and the machine runs its own operating system: **quesOS**, a
multi-process kernel on a paging MMU, booting a borrowed Unix userland. It plays DOOM at around
65fps with the byte code still fully interpreted. See [ROADMAP.md](ROADMAP.md) for where it goes
next.

## UPDATE: Maize Runs DOOM!

As of 13 July 2026, Maize can run a version of DOOM compiled to Maize bytecode from C sources!

<img src="doom.png" alt="DOOM running on Maize">

It now averages around ~~45fps~~ **65fps** on my workstation, peaking at 72fps. Considering that the Maize bytecode is still completely interpreted, that's pretty good. I just made a couple of small changes to the DOOM source to take advantage of an optimization inside Maize, and the FPS jumped from around 15 to 70. Once JIT is implemented, it should be considerably faster.

If you want to try it yourself, you just need to build `maizeg` (the graphical VM) and
provide your own DOOM WAD (the shareware `doom1.wad` works).

### Build `maizeg`, the graphical VM

Maize ships two VM binaries: `maize`, which is console-attached and talks to your terminal,
and `maizeg`, which opens a window. The SDL2 window backend is opt-in
(`-DMAIZE_DISPLAY=ON`) and applies only to `maizeg`; a default build is headless and has no
SDL2 dependency. The compiled DOOM image, `demos/doom/doom.mzx`, is already in the repo, so
you only need to build `maizeg`, then bring a WAD.

**Windows** (PowerShell, from the repo root). SDL2 is bundled under `.toolchains/`, so there
is nothing extra to install:

``` powershell
cmake --preset windows-llvm-mingw-release -DMAIZE_DISPLAY=ON
cmake --build --preset windows-llvm-mingw-release --target maizeg
```

**Linux** (bash, from the repo root). Install the SDL2 development package first, then build:

``` bash
sudo apt-get install -y libsdl2-dev        # Debian/Ubuntu; use your distro's SDL2 -dev package
cmake --preset linux-release -DMAIZE_DISPLAY=ON
cmake --build --preset linux-release --target maizeg
```

### Run it

Maize gives every guest a persistent sandbox root at `~/.maize/root` that IS the
guest's `/`, so the simplest way to hand DOOM its WAD is to drop the file inside it.
Make a `doom` folder under the root and put your WAD there (you supply the WAD; the
shareware `doom1.wad` or a retail `DOOM.WAD` both work):

- Windows: `%USERPROFILE%\.maize\root\doom\doom1.wad`
- Linux: `~/.maize/root/doom/doom1.wad`

The guest sees that file as `/doom/doom1.wad`. Now run, from the repo root; no
`--mount` is needed, because the WAD already lives inside the sandbox root:

``` powershell
build\windows-llvm-mingw-release\maizeg.exe --display --display-scale 4 --refresh-hz 20 --input=keyboard demos/doom/doom.mzx -iwad /doom/doom1.wad
```

The same run on Linux:

``` bash
build/linux-release/maizeg --display --display-scale 4 --refresh-hz 20 --input=keyboard demos/doom/doom.mzx -iwad /doom/doom1.wad
```

DOOM writes saved games to a relative path (`./.savegame/...`), which
resolves against the guest's `/home/user` working directory in that same persistent
sandbox root, so no writable mount is needed and your saves and config survive
across runs.

A `~/.maize/config` file sets default values for the launcher flags, written one `key=value` per line with the dashes dropped (`display-scale=4`, `refresh-hz=20`, `input=keyboard`), so
you set them once instead of on every command line, and `~/.maize/env` supplies a
default guest environment. The sandbox root, the
config file, and the environment file are all explained in detail further below,
under "The sandbox root and mounting host directories", "Startup defaults
(`~/.maize/config`)", and "Setting the program's environment". (Pass `--no-root` to
opt out of the sandbox root entirely.)

For how the DOOM port itself is built and tested (the vendored doomgeneric tree, the headless
render gate, and the license-clean synthetic IWAD used by CI), see
[demos/doom/README.md](demos/doom/README.md).

## The Maize ISA Reference

**[The Maize ISA Reference (v1.0)](https://paulmooreparks.github.io/Maize/)** is the
complete, frozen instruction-set specification, published as a book. It is the single
authoritative description of the machine, and it is deliberately *not* duplicated in this
README: v1.0 is a behavioral freeze, and one frozen source of truth beats two that drift
apart. Source under [docs/spec/](docs/spec/README.md).

Where to look for what:

* **[Overview](docs/spec/overview.md)** and
  **[Register model](docs/spec/register-model.md)** /
  **[Sub-registers](docs/spec/subregister-model.md)**: the shape of the machine.
* **[Addressing modes](docs/spec/addressing-modes.md)** and
  **[Instruction encoding](docs/spec/instruction-encoding.md)**: how an instruction is
  built, plus the [encoding quick reference](docs/spec/appendix-b-encoding-quickref.md).
* **[Instruction reference](docs/spec/instruction-reference.md)**: every instruction, its
  operation, and its flag effects. The [opcode map](docs/spec/appendix-a-opcode-map.md) is
  the numeric table.
* **[Execution](docs/spec/execution-model.md)**, **[Memory](docs/spec/memory-model.md)**,
  and **[Traps](docs/spec/trap-model.md)**: what happens at run time, including the
  paging model and every defined trap.
* **[Device surface](docs/spec/device-surface.md)** and
  **[syscall surface](docs/spec/appendix-c-syscall-surface.md)**: how the machine reaches
  the outside world.
* **[Floating point](docs/spec/floating-point.md)**: IEEE-754, in v1.0.
* **[Conformance](docs/spec/conformance.md)**, **[versioning](docs/spec/versioning.md)**,
  and **[reservations](docs/spec/reservations.md)**: what a conforming VM must do, what
  the freeze guarantees, and what is deliberately left open for later.


## What It Is, Basically

* A 64-bit virtual machine implemented in C++ that executes a custom byte code, with a
  paging MMU (Sv48), supervisor/user privilege separation, and a full trap and interrupt model
* An assembly language that represents the byte code
* An assembler (`mazm`), a linker (`mzld`), and a disassembler (`mzdis`), with a relocatable
  object format (`.mzo`) and a linked executable format (`.mzx`)
* A C compiler pipeline (`mzcc`) built on vendored [cproc](https://sr.ht/~mcf/cproc/) and
  [QBE](https://c9x.me/compile/) with a Maize code-generation target and a freestanding C runtime
* An operating system, **quesOS**, running on the machine itself
* An execution environment implemented in C++ that so far runs on Windows and Linux and could easily be ported to other platforms

## What It Runs Today

Maize is past the point where the interesting artifact is the VM. These all run on it now:

* **quesOS**, a multi-process kernel with per-process address spaces on the Sv48 paging MMU:
  `fork`, `exec`, pipes, signals, `poll`, termios, and a preemptive scheduler, presenting a
  Linux-shaped syscall ABI. Source under [os/quesos/](os/quesos/).
* **A borrowed Unix userland.** The [oksh](https://github.com/ibara/oksh) shell and the
  [sbase](https://core.suckless.org/sbase/) coreutils, recompiled for the Maize ISA, with
  pipelines, redirection, and job exit statuses working at an interactive prompt. Nothing in
  the userland is hand-written for Maize; the project's rule is to borrow every solved problem
  and build only what is unique to the machine.
* **DOOM**, compiled from C to Maize byte code, playable at around 65fps.
* **kilo**, a real terminal text editor, running in raw mode against your actual terminal.
* Two VM binaries: `maize` is console-attached, so guest programs read and write your
  terminal; `maizeg` opens a window and acts as a presentation host for graphical guests.

## Yeah, but... WHY?

It's a long story.

In 2016 I had a contract working on an ARM system, and I wasn't too familiar with ARM assembly. I had an idea to write an ARM
emulator, since I've always believed that the best way to understand a system is to try to build one. After getting stuck with the
ARM emulator, I decided to first build an x86 emulator and then go back to ARM. While I knew x86 assembly well enough to debug it,
I wasn't really an expert at it, and I didn't know the lowest levels of machine language. I thought that tackling an ISA I knew
would help me get the basics sorted out, and I'd come back to ARM later.

I got the x86 emulator working well enough to run code generated by standard compilers, but by then I wasn't working on ARM anymore, and I
was more interested in learning about how CPUs work. I also found
[Ben Eater's Youtube! channel](https://www.youtube.com/@BenEater/playlists), where he builds an 8-bit computer from scratch, and I decided
to use those as guidance for building a virtual CPU of my own design. The first implementation of that was the
[Tortilla](https://github.com/paulmooreparks/Tortilla) project.

With Tortilla, I wrote code for every single cycle of each instruction, as if the CPU were moving data around the buses like a physical CPU. That
was fun and enlightening, but it was also terribly inefficient. I decided to rewrite the entire thing in C++ and make the virtual machine more of a
byte-code execution environment rather than a simulation of a CPU, and that became the Maize project. The idea is to be able to compile any language
to Maize byte code and run it on any system that can run the Maize VM.

No, I never got back around to the ARM emulator, and at this point I doubt I will.

## Uses for Maize

Maize began mainly as a toy to learn about a few concepts:

* How byte-code virtual machines work
* The construction of an assembly language and corresponding assemblers and disassemblers
* Porting compiler back-ends to a new architecture
* How to write an operating system for a new architecture
* How systems integrate with hardware

It's been really useful for all of the above, and several of them have stopped being learning
exercises and become shipped artifacts. What I'm most excited about now is the promise of
compiling any language to Maize byte code and running it anywhere that can run the Maize VM.

## Building From Source

Maize builds with CMake + Ninja and either Clang or GCC. On Windows, the primary compiler is a pinned
llvm-mingw toolchain fetched by a small bootstrap script, no installer or admin rights
required.

The core VM and tools build from a plain checkout. The C toolchain additionally vendors
cproc and QBE as pinned git submodules, so clone with `git clone --recurse-submodules`
(or run `git submodule update --init --recursive` after a plain clone) if you want `mzcc`.
See [toolchain/VENDORING.md](toolchain/VENDORING.md) for the pins and build environments.

### Prerequisites (all platforms)

* CMake 3.21 or newer
* Ninja

Windows: `winget install Kitware.CMake` and `winget install Ninja-build.Ninja` (both
install per-user, no admin required). Linux: `sudo apt install cmake ninja-build`.
macOS: `brew install cmake ninja`.

### Build everything in one command

    scripts\build-world.ps1        Windows
    scripts/build-world.sh         Linux / macOS

Builds the native binaries and tools, the C cross toolchain, quesOS
(quesos.mzx), the wave-1 userland set, and the demos (kilo, doom), in one
call, leaving every artifact where the per-piece scripts below already put
it. This is the "I pulled, now what" answer; the sections below cover each
piece individually (a partial build, or understanding what build-world
composes).

Prerequisites, explicit and minimal: CMake and Ninja (above) on every
platform; on Windows, Git for Windows (git-scm.com), which also ships Git
Bash, used to build the vendored cproc/QBE compiler (the one build step that
needs a POSIX shell). The per-piece quesOS, userland, and demo builds run
through the native `mzcc` binary and shell out to nothing. No WSL, no MSYS2,
and no other separate install is required.

### Windows, primary path: llvm-mingw

    scripts\bootstrap-toolchain.ps1
    cmake --preset windows-llvm-mingw-debug
    cmake --build --preset windows-llvm-mingw-debug

The bootstrap script downloads a pinned llvm-mingw release into `.toolchains\llvm-mingw\`
(gitignored) and verifies it against a pinned SHA256 checksum. Re-running it is a no-op
once the pinned version is already present.

A plain clang release build interprets ~26-28% slower than the same source built with
gcc/Linux (host codegen quality on the interpreter loop, not ISA flags). Maize closes
that gap with Clang PGO (Profile-Guided Optimization) instead of switching
compilers: `scripts/install-mazm.ps1` (and the Ctrl+Shift+B build task) apply a
profile committed at `scripts/pgo-profiles/windows-llvm-mingw-release/` automatically,
so the shipped `maize.exe`/`maizeg.exe` are profile-guided with no extra step on a
fresh clone. See `CMakeLists.txt`'s `MAIZE_PGO` option and `scripts/build-pgo.ps1` if
you need to regenerate the profile.

### Windows, fallback: MSYS2 UCRT64 GCC

Install MSYS2 (msys2.org) to its default location (C:\msys64), then from an MSYS2
UCRT64 shell:

    pacman -S mingw-w64-ucrt-x86_64-toolchain

From a regular Windows shell (PowerShell or Git Bash):

    cmake --preset windows-msys2-debug
    cmake --build --preset windows-msys2-debug

If MSYS2 is installed somewhere other than C:\msys64, override the compiler paths in a
local, gitignored CMakeUserPresets.json.

### Linux

    cmake --preset linux-debug
    cmake --build --preset linux-debug

Uses whichever of GCC or Clang CMake finds by default; set CC/CXX before configuring to
force a specific compiler.

### macOS

    cmake --preset macos-debug
    cmake --build --preset macos-debug

Uses the system Clang from the Xcode Command Line Tools (xcode-select --install).

### Smoke test

    mazm asm/hello.mazm
    maize asm/hello.mzb

Should print "Hello, world!". Every preset's build directory lives under build/<preset-name>/.

### Running the test suite

    scripts\run-tests.ps1        Windows
    scripts/run-tests.sh         Linux

Each script builds the tools it needs (maize, maizeg, mazm, mzld, mzdis), then assembles
and runs every in-scope test under asm/, comparing captured output against the
expected result for each. Prints a per-test PASS/FAIL report plus a summary line.
Exits 0 if all tests pass, 1 if any test fails, 2 if the environment isn't set up
correctly (missing CMake or Ninja, or a build failure).

A separate harness, `scripts/run-ctest.sh`, compiles and runs the C corpus under
ctest/ through the full mzcc pipeline and diffs each program's output against its
committed fixture, so a codegen regression reports separately from an asm-suite
regression.

### Editor setup (VS Code)

Open the repo in VS Code, install the recommended extensions when prompted (CMake Tools
and clangd), pick a configure preset from the CMake Tools status bar, and build.
Everything above also works from any editor or a bare terminal; presets are the only
interface CMake Tools uses.

### A note on build type

Each platform has both a `-debug` and a `-release` preset (for example
`windows-llvm-mingw-debug` and `windows-llvm-mingw-release`). Use a release preset for
anything where speed matters, such as running DOOM; on Windows the release build is
additionally profile-guided, as described above.

## How To Use Maize

Maize is implemented in standard C++ and runs on Windows and Linux. The toolchain is six
binaries:

* **maize** ([src/maize.cpp](src/maize.cpp)) runs a program image: a flat `.mzb` memory image
  or a linked `.mzx` executable. It is console-attached, so the guest's standard input and
  output are your terminal's. See "Running Maize programs directly" below for the full
  command line.
* **maizeg** is the same VM built with the SDL2 window backend (`-DMAIZE_DISPLAY=ON`). It
  opens a window and acts as the presentation host for guests that draw to the framebuffer,
  which is how DOOM and any graphical program reach the screen.
* **mazm** ([src/mazm.cpp](src/mazm.cpp)) assembles a `.mazm` source to a flat `.mzb` image,
  or to a relocatable `.mzo` object with `-c`. It reports `file:line` diagnostics, exits
  nonzero on error without leaving a stale binary, and has editor-integration modes
  (`--check`, `--stdin`); run `mazm --help` for the full flag list. The language
  and directive reference is [ASSEMBLER.md](ASSEMBLER.md).
* **mzld** ([src/mzld.cpp](src/mzld.cpp)) links `.mzo` objects into a `.mzx` executable.
  See "Object Files, Linking, and Executables" below.
* **mzdis** ([src/mzdis.cpp](src/mzdis.cpp)) disassembles a `.mzb` or `.mzx` back to
  assembly; a flat `.mzb` listing reassembles through mazm back to the exact original
  bytes, with synthesized `fn_`/`loc_` labels at call and branch targets so the listing
  reads like a normal program.
* **mzcc** compiles C11 to a runnable `.mzx` through the vendored cproc/QBE pipeline.
  See "The C Toolchain (mzcc)" below.

## Running Maize programs directly

You can register `maize` as the operating system's handler for Maize images, so
that an image runs directly the way a `.py` or `.js` file does. The full command
line is:

```
maize [options] <image> [guest-args...]
```

There are two VM binaries. **`maize`** is the console build: its output goes to
the terminal, so use it for programs that print (`maize hello.mzb`, quesOS).
**`maizeg`** is the graphical build: it opens an on-screen window, so use it for
framebuffer programs (DOOM). Both accept the same options. On Windows the split is
load-bearing: a graphical binary is linked as a GUI-subsystem app and cannot write
to the terminal it was launched from, which is why console output lives in `maize`.

Options are consumed up to the first non-option token, which is `<image>`;
everything after `<image>` is passed to the program as its `argv`, verbatim (a
`-flag` after the image is a guest argument, never a maize option). `argv[0]` is
`<image>` exactly as you typed it. A `--` explicitly ends the options if you need
the image name itself to start with `-`.

`maize` loads and runs both image formats, dispatching on the header: a `.mzx`
(linked) image begins with the magic bytes `M Z X 0x01` and is loaded segment by
segment; anything else is loaded as a flat `.mzb` image at address 0. Registration
is OS-level glue on top of that, and the runner behaves as a well-formed
interpreter: it takes the image path as its first non-option argument, accepts an
absolute path, and passes any following arguments through to the guest.

### Setting the program's environment

A program's environment is built only from what you pass on the command line
plus the optional standing default file `~/.maize/env`; `maize` never inherits
your shell's own environment, so a run is deterministic.

- `-e KEY=VAL`, `--env KEY=VAL`, or `--env=KEY=VAL` adds one variable. Repeatable.
- `--env-file <path>` adds variables from a file of `KEY=VAL` lines. Blank lines
  and lines whose first non-whitespace character is `#` are ignored. Repeatable.
- `~/.maize/env` (if present) is a persistent, operator-owned default environment
  in the same `KEY=VAL` format. It is loaded into every guest first, then any
  `-e`/`--env`/`--env-file` entry appends to it (or overrides a key it defines).

`KEY` must match `[A-Za-z_][A-Za-z0-9_]*`; the value is everything after the first
`=` (it may contain further `=` characters or be empty, and there is no shell
quoting or `$`-expansion). A key defined more than once takes its last value, so a
CLI entry overrides a `~/.maize/env` default of the same key; the guest sees one
entry per key. With no env flags and no default file the program still receives a
valid, empty environment. The host environment is never inherited even with
`~/.maize/env`: that file is a standing default you control, not a leak of the
ambient host environment (the deny-by-default posture holds). It is not shipped
with any values; sensible entries an operator might add are `HOME=/home/user`,
`USER=user`, `PWD=/home/user`, `TMPDIR=/tmp`, and a `TERM`.

```sh
maize --env GREETING=hi --env TARGET=world hello.mzb alpha beta
maize --env-file run.env prog.mzb
```

### Startup defaults (`~/.maize/config`)

A long invocation collapses if you record the flag values you always use in an
optional `~/.maize/config` file. It supplies the **default** value for the scalar
and boolean launcher flags, so the precedence is built-in default < `~/.maize/config`
< CLI flag: a flag you pass on the command line always wins, and the config only
changes the value a flag starts from. The file is optional; when it is absent the
built-in defaults stand and behavior is unchanged.

The format mirrors `--env-file`: one `key=value` per line, with blank lines and
lines whose first non-whitespace character is `#` ignored. Keys are the long flag
names **without** the leading dashes:

- `display-scale` = 1..16
- `refresh-hz` = 1..1000
- `resolution` = `<width>x<height>` (e.g. `320x200`) -- framebuffer pixels
- `console-size` = `<cols>x<rows>` (e.g. `80x25`, the default; cols 20..500, rows
  10..200) -- the text console grid, as `--console-size`
- `root` = host path for the sandbox root (as `--root`)
- `input` = `sys`, `keyboard`, or `console` (the console `maize` ignores this
  key; an explicit `--input` on the command line still applies)
- `show-perf` = boolean
- `display` = boolean (graphical `maizeg` only, which opens a window by default; set
  `display=false` to force it headless. The console `maize` ignores this key.)
- `pause-on-halt` = boolean (graphical `maizeg` only: hold the window open on the
  final frame, and any `--show-perf` report, until a key or window-close)
- `vsync` = boolean (graphical `maizeg` only, default `true`: sync each frame present
  to the monitor's vblank so the picture cannot free-run against the display. With
  vsync on the visible present rate is monitor-locked and `refresh-hz` no longer sets
  the present cadence; `refresh-hz` then drives only the guest vsync-IRQ cadence and
  the input-poll timeout. Set `vsync=false` to present unsynced, paced by `refresh-hz`.)
- `no-root` = boolean
- `mount` = `<host-path>=<guest-path>[:ro|:rw]`, byte-for-byte the same grammar as
  `--mount` (see "The sandbox root and mounting host directories" below).
  **Repeatable**: one grant per `mount=` line, in file order.
- `mount-home` = `true`/`1`/`yes` or an empty value (resolve the host home via
  `HOME`/`USERPROFILE`, like bare `--mount-home`), `false`/`0`/`no` (no-op, same as
  omitting the key), or any other value (an explicit host-path override, like
  `--mount-home=<path>`)

Booleans accept `true`/`false`, `1`/`0`, or `yes`/`no`. Parsing is fail-soft for
every key above **except** `mount` and `mount-home`: an unknown key or a malformed
scalar/boolean value is reported on stderr and ignored, so a bad line never bricks
the launcher. Mount grants are the one exception, because a dropped mount grant is
a capability quietly vanishing, not a cosmetic fallback: a malformed or unreachable
`mount=`/`mount-home=` line exits `maize` nonzero before the guest starts (the same
fail-closed contract `--mount`/`--mount-home` already have), with a diagnostic
naming the config file and the offending line.

Config and CLI mounts merge the same way every other config key does: `mount=`/
`mount-home=` lines load first as defaults, and a `--mount`/`--mount-home` on the
command line for the exact same guest path overrides the matching config grant
(the config grant is silently dropped, no error) -- so a one-off CLI override
never requires editing the config file. Two grants from the SAME source (two
config lines, or two CLI flags) that merely overlap remain a hard fail-closed
startup error, unchanged.

#### Per-binary overrides (`~/.maize/maize.config`, `~/.maize/maizeg.config`)

Some keys mean nothing on one binary: `input` and `display` are graphical-only,
and the console `maize` ignores them. Rather than crowd the shared file with keys
that only ever apply to one build, you can put per-binary overrides in an optional
second file, read on top of the shared `~/.maize/config`:

- The console `maize` reads `~/.maize/maize.config`.
- The graphical `maizeg` reads `~/.maize/maizeg.config`.

Both use the identical `key=value` format and honor every key the shared file does.
The precedence becomes built-in default < `~/.maize/config` (shared) < the per-binary
config < CLI flag: a key set in the per-binary file overrides the shared file, and a
key it leaves unset keeps whatever the shared file (or the built-in default) set. Each
file is independently optional; either, both, or neither may exist, and a shared-only
config keeps working unchanged. Warnings from a bad line now name the file they came
from, so you can tell a `maize.config` mistake from a shared-`config` one.

Mount grants layer across the three tiers the same way scalar keys do: a `mount=`/
`mount-home=` grant in the per-binary file for the exact same guest path silently
replaces the shared file's grant, and a CLI `--mount`/`--mount-home` for that path
still wins over both. Two grants that merely overlap WITHIN one tier (two lines in
the same file) remain a hard fail-closed startup error, unchanged.

Because the console build ignores graphical-only keys, an `input=keyboard` line in
`maize.config` (the console's own file) is a likely mistake and is reported on stderr
before it is dropped; the same key inherited from the shared `config` stays silent, as
it always has.

```
# ~/.maize/config
display-scale=4
refresh-hz=20
input=keyboard
show-perf=true
mount=/home/paul/data=/data:rw
mount-home=true
```

With that file in place, `maizeg --display doom.mzx` behaves as if you had also
typed `--display-scale 4 --refresh-hz 20 --input=keyboard --show-perf --mount
/home/paul/data=/data:rw --mount-home`, and you can still override any of them on
the command line (e.g. `--display-scale 6`, or a `--mount` naming `/data` again to
redirect just that one run).

### The sandbox root and mounting host directories

By default the guest gets a persistent sandbox root filesystem: a dedicated host
directory (`~/.maize/root`, created on first run with a `/home/user` and `/tmp`
skeleton) is mounted read-write as the guest root `/`, and the startup working
directory is `/home/user`. A relative guest path resolves against that cwd, so a
program that writes `./file` (or DOOM saving to `./.savegame/...`) lands under the
sandbox root and persists across runs with no per-program configuration. Your real
filesystem is NOT reachable: only the sandbox root plus any explicit overlay grants.

Because the root is just a host directory, you can stage files into it yourself:
anything you create under `~/.maize/root` appears to the guest at the matching path.
Create `~/.maize/root/doom/` and it is the guest's `/doom/`, which is exactly how the
DOOM quickstart above hands the game its WAD (`~/.maize/root/doom/doom1.wad` becomes
`/doom/doom1.wad`) without any `--mount`.

- `--root <hostpath>` uses a different host directory as the sandbox root.
- `--no-root` disables the sandbox root; the guest starts with an empty,
  deny-by-default filesystem (only explicit `--mount` grants are reachable), the
  WASI-preopen-style capability model.

Mounts are explicit grants that overlay on top of the root (read-only unless opted
into read-write; longest guest-path prefix wins, so the root is the fallback):

- `--mount HOST=/GUEST[:ro|:rw]` grants the guest a *nix view of one host
  directory. Repeatable. `HOST` may be a native Windows path (`C:\work`,
  `C:/work`) or a POSIX path; `/GUEST` is always a *nix absolute path and cannot
  be `/` itself. `:ro` is the explicit default; `:rw` opts into writes.
- `--mount-home[=HOST]` is sugar mapping the host home directory over
  `/home/user`, read-write.

Both are also configurable as standing defaults via `~/.maize/config`'s `mount`/
`mount-home` keys (identical grammar; see "Startup defaults" above).

```sh
maize --mount C:/work=/proj:rw prog.mzx
maize --mount /home/paul/data=/data --mount-home prog.mzx
maize --no-root --mount /srv/data=/data:ro prog.mzx
```

Within a mount the guest uses the Linux-mirroring file syscalls (open, close,
fstat, lseek, getdents64, plus read/write on the granted fds; the full syscall
surface is in [toolchain/rt/SYSCALL-ABI.md](toolchain/rt/SYSCALL-ABI.md)).
Guest paths are normalized (`.`, `..`, and duplicate slashes are
resolved, `..` clamped at the root) to select the mount, and every host-side
resolution is then confined to its mount root, so `..` can never escape a mount.
Startup fails closed on a malformed or unreachable grant. The full contract,
including the binary-ABI structures, lives in
[docs/design/hostfs.md](docs/design/hostfs.md).

The scripts below are documented, user-run tools. They are **not** run by the
build, and they change OS state, so run them yourself and reverse them with the
matching `unregister` action when you are done. Both are idempotent (safe to run
twice) and fully reversible.

### Linux (binfmt_misc)

`scripts/register-binfmt.sh` registers two `binfmt_misc` entries, keyed the same
way the loader dispatches:

- `.mzx` images are matched by the header **magic** (`4D 5A 58 01`) at offset 0.
  Because the match is on magic rather than name, a magic-bearing image runs
  directly even with a different extension or no extension at all, as long as it
  has the executable bit set.
- `.mzb` images are matched by **extension**, because a flat image has no header
  magic to key on (it begins with the first instruction).

```sh
# maize must be on PATH (or pass --interp /path/to/maize). Needs root.
sudo ./scripts/register-binfmt.sh register
chmod +x hello.mzb hello.mzx
./hello.mzb        # runs via maize
./hello.mzx        # runs via maize
sudo ./scripts/register-binfmt.sh unregister   # reverses it cleanly
```

`binfmt_misc` invokes the interpreter as `maize <full-path> [args...]`, so the
image path arrives as `maize`'s first argument. The entries are per-kernel (and,
under WSL, per-instance), independent of Windows file associations.

### Windows (file associations)

`scripts/register-assoc.ps1` creates `.mzb`/`.mzx` associations under `HKCU`
(no administrator rights required, nothing machine-wide changed) pointing at
`maize.exe "%1"`:

```powershell
# maize.exe must be on PATH (or pass -MaizeExe C:\path\to\maize.exe).
pwsh ./scripts/register-assoc.ps1 register
.\hello.mzb        # runs via maize.exe (double-click in Explorer also works)
pwsh ./scripts/register-assoc.ps1 unregister   # removes the association
```

To run `prog` and have the shell find `prog.mzb`/`prog.mzx`, add `.MZB;.MZX` to
`PATHEXT`. The script prints guidance for this by default; pass `-UpdatePathext`
to append them to your user `PATHEXT` automatically (also reversed by
`unregister`).

### What works

- **Exit status works.** A directly-run image's process exit code reflects the
  program's return value (`maize` surfaces the guest's `SYS $3C` exit through its
  own exit status). A C program that ends with `return 13` yields `$?` == 13 on
  Linux (or `%ERRORLEVEL%` == 13 on Windows); codes truncate to the usual 0-255
  range.
- **Arguments and environment are delivered.** Command-line arguments after the
  image reach the guest as `argv`, and `-e/--env`/`--env-file` values reach it as
  `envp`, so a C `main(int argc, char **argv, char **envp)` sees them directly.
  When registered as an OS handler, `binfmt_misc` / the file association pass the
  invocation's trailing arguments straight through to the guest.

## Project Status

The instruction set is frozen at v1.0, fully specified in
[the ISA Reference](https://paulmooreparks.github.io/Maize/), and implemented and CI-tested on
Windows and Linux. That freeze is a promise: a binary assembled against v1.0 runs on any v1.x
VM. Floating point is part of v1.0, not deferred.

The toolchain is complete end to end: mazm assembles flat images and relocatable objects, mzld
links executables, mzdis round-trips flat images byte for byte, and mzcc compiles C11 programs
against a Unix-style syscall surface with real errno reporting, a brk-backed heap, and a
variadic printf. The syscall set covers file and directory operations, terminal control, and
bulk memory operations, and is documented in
[toolchain/rt/SYSCALL-ABI.md](toolchain/rt/SYSCALL-ABI.md).

Above that sits quesOS, a multi-process kernel on the paging MMU running a borrowed shell and
coreutils, described under "What It Runs Today" above.

This implementation in C++ is MUCH faster and MUCH tighter than the .NET version.

The near-term road map (see [ROADMAP.md](ROADMAP.md), the sequencing source of truth):

* A block device and a real filesystem, so the machine has a disk instead of a host mount
* A window system for quesOS, ported rather than invented
* Optimizing compiler backends, LLVM first and then GCC, to lift the strict-C-subset ceiling
* A JIT, and a fast path for address translation
* Networking: a network device and a socket ABI
* Make the assembler read Unicode source files

## Hello, World!

Here is a simple ["Hello, World!" application](https://github.com/paulmooreparks/Maize/blob/master/asm/hello.mazm)
written in Maize assembly.

    ; **********************************************************************************
    ; The entry point. Execution begins at address $00000000.

    $0000`0000:             ; The back-tick (`)  is used as a number separator.
                            ; Underscore (_) and comma (,) may also be used as separators.
        CALL main
        HALT                ; HALT halts the core pending an interrupt. With no interrupt
                            ; source in the VM, a halted core has nothing to wake it, so the
                            ; run loop returns and the Maize host process exits 0 with no
                            ; status. The status-carrying termination path is sys_exit
                            ; (SYS $3C): it records the low 8 bits of R0 as the process exit
                            ; status. A C program's crt0 routes main's return value there.

    ; **********************************************************************************
    ; The output message

    hw_string:
        STRING "Hello, world!\0"

    ; **********************************************************************************
    ; Return the length of a zero-terminated string. Equivalent to the following C code:
    ;
    ;   size_t strlen(char const *str) {
    ;       size_t len = 0;
    ;       while (str[len]) {
    ;           ++len;
    ;       }
    ;       return len;
    ;   }
    ;
    ; Maize uses a flat 64-bit address space, so pointers and the stack pointer are full
    ; 64-bit values; addresses live in whole registers, not H0 sub-registers.
    ;
    ; Parameters:
    ;   R0: Address of string
    ; Return:
    ;   RV: Length of string

    strlen:
        PUSH BP                 ; Save the caller's base pointer
        CP SP BP                ; Establish this frame: BP = SP
        SUB $08 SP              ; Reserve an 8-byte local slot for the counter
        LEA $-08 BP RT          ; RT = address of the counter (BP - 8), a full 64-bit address
        CLR R2                  ; counter = 0
        ST R2 @RT               ; Store the counter to its stack slot
    loop_condition:
        LD @RT R2               ; Load the counter
        LEA R2 R0 R1            ; R1 = string address + counter
        LD @R1 R3.B0            ; R3.B0 = the character at that address
        CMP $00 R3.B0           ; Data movement does not set flags; test the byte explicitly.
        JZ loop_exit            ; Jump out of the loop when the terminating NUL is reached.
    loop_body:
        LD @RT R2               ; Load the counter
        INC R2                  ; ...add one...
        ST R2 @RT               ; ...and store it back.
        JMP loop_condition      ; Continue the loop.
    loop_exit:
        LD @RT RV               ; Return the counter in RV.
        CP BP SP                ; Tear down the frame: SP = BP
        POP BP                  ; Restore the caller's base pointer
        RET                     ; Pop the return address from the stack into PC.

    ; **********************************************************************************
    ; The main function

    main:
        CLR R0
        CP hw_string R0.H0      ; Copy the address of the message string into R0
        CALL strlen             ; Call strlen to get the string length (into RV)
        CP $01 R0               ; $01 in R0 indicates output to stdout
        CLR R1
        CP hw_string R1.H0      ; R1 holds the address of the message to output
        CP RV R2                ; Put the string length into R2
        SYS $01                 ; Call the output function implemented in the Maize VM
        CP $00 RV               ; Set the return value for main
        RET                     ; Leave main

## Assembler Syntax

Maize assembly is line-oriented: comments run from `;` to the end of the line,
numeric literals carry a `$`/`#`/`%` base prefix, a token ending in `:` opens a
labelled block (or, for a numeric header like `$0000,0000:`, sets the
assembly address), and directives (`INCLUDE`, `LABEL`, `DATA`, `STRING`,
`ADDRESS`, plus the object-mode set) cover everything an instruction doesn't.

[ASSEMBLER.md](ASSEMBLER.md) is the complete reference: the assembler's command
line, the source syntax, and every directive with its parameters and examples.
For working, tested examples see [asm/hello.mazm](asm/hello.mazm) and the
`test_*.mazm` programs under [asm/](asm/), all of which assemble and run as part
of the test suite.


## Object Files, Linking, and Executables

By default `mazm` assembles a single `.mazm` source straight to a flat, header-less
memory image (a `.mzb`) that `maize` loads byte-for-byte at address 0. For programs
built from more than one translation unit, Maize adds a relocatable **object format**
(`.mzo`), a **linker** (`mzld`), and a linked **executable format** (`.mzx`). These are
additive and opt-in: the flat `.mzb` path is unchanged.

    mazm -c a.mazm            # assemble a.mazm -> a.mzo (relocatable object)
    mazm -c b.mazm            # assemble b.mazm -> b.mzo
    mzld -o prog.mzx a.mzo b.mzo   # link objects -> prog.mzx (runnable)
    maize prog.mzx            # run the linked executable

All multi-byte fields in both formats are **little-endian**, matching the ISA's
immediate encoding and the flat-64 memory model. Every offset is a byte offset from
the start of the file. The layouts below are complete enough to hand-decode a produced
object or executable with nothing but this section.

### Object-mode assembler directives

In object mode (`-c`) the assembler never resolves a symbolic operand inline: every
label reference becomes a relocation the linker fills in, and content is partitioned
into CODE, RODATA, DATA, and BSS sections. Object mode uses **strict declared
interfaces**: a reference to a symbol the unit neither defines nor declares `EXTERN`
is an error, so a typo is caught at assembly time rather than deferred to the linker.

The directives that drive this (`SECTION`, `GLOBAL`, `PUBLIC`, `EXTERN`, `ZERO`,
`DREF`, `ALIGN`) are documented, with parameters and examples, in
[ASSEMBLER.md](ASSEMBLER.md); in flat (`-c` absent) assembly they are inert no-ops
and the `.mzb` output is byte-identical.

A label reference's relocation width follows the immediate width the operand encodes.
For a two-operand data move the width is the destination sub-register width, so
`CP label Rn` materialises a full 64-bit address (`R_MAIZE_ABS64`) and
`CP label Rn.H0` a 32-bit address (`R_MAIZE_ABS32`). Single-operand control-transfer
targets (`CALL label`, `JMP label`) use a 32-bit target (`R_MAIZE_ABS32`). (The
QBE backend materialises addresses full-width, i.e. `R_MAIZE_ABS64`; the
narrow forms are a `mazm` hand-assembly convenience.)

### Object format `.mzo`

    Header (48 bytes)
      off  size  field
      0    4     magic          = 'M','Z','O', 0x01   (ASCII "MZO" + format version 1)
      4    2     flags          (u16, reserved = 0)
      6    2     section_count  (u16)
      8    8     shoff          (u64) file offset of the section-header array
      16   8     symoff         (u64) file offset of the symbol table
      24   4     symcount       (u32) number of symbol entries
      28   8     stroff         (u64) file offset of the string table
      36   4     strsize        (u32) string-table size in bytes
      40   4     entry_sym      (u32) symtab index of the entry symbol, or 0xFFFFFFFF if none
      44   4     reserved       (u32 = 0)

    Section header (40 bytes each; section_count of them at shoff)
      0    4     name_off       (u32) offset into the string table
      4    1     kind           (u8)  0=NULL 1=CODE 2=RODATA 3=DATA 4=BSS
      5    1     attrs          (u8)  0x1 EXEC  0x2 READ  0x4 WRITE  0x8 ALLOC  0x10 NOBITS
      6    1     align          (u8)  required alignment in bytes (power of two; 1 = byte-aligned)
      7    1     reserved       (u8 = 0)
      8    8     file_off       (u64) offset of section contents in this file (0 when NOBITS)
      16   8     size           (u64) in-memory size in bytes
      24   8     reloc_off      (u64) file offset of this section's relocation array (0 if none)
      32   8     reloc_count    (u64) number of relocation entries at reloc_off

    Symbol entry (24 bytes each; symcount of them at symoff)
      0    4     name_off       (u32) offset into the string table
      4    2     section_index  (u16) defining section index, or 0xFFFF UNDEF, 0xFFF0 ABS
      6    1     binding        (u8)  0=LOCAL 1=GLOBAL 2=WEAK (WEAK reserved)
      7    1     type           (u8)  0=NOTYPE 1=FUNC 2=OBJECT 3=SECTION
      8    8     value          (u64) offset within the defining section (0 for UNDEF)
      16   8     size           (u64) symbol size in bytes (0 if unknown)

    Relocation entry (24 bytes each; reloc_count of them at a section's reloc_off)
      0    8     r_offset       (u64) offset within the target section where the fixup applies
      8    4     r_symbol       (u32) symtab index of the referenced symbol
      12   1     r_type         (u8)  relocation type (see below)
      13   3     reserved       (3 bytes = 0)
      16   8     r_addend       (i64) signed addend added to the symbol value

    String table (strsize bytes at stroff)
      NUL-separated UTF-8 names; byte 0 is a leading NUL, so name_off = 0 means "".

The canonical attribute set per section kind: CODE = EXEC+READ+ALLOC, RODATA =
READ+ALLOC, DATA = READ+WRITE+ALLOC, BSS = READ+WRITE+ALLOC+NOBITS. Sections are laid
out in the fixed order CODE, RODATA, DATA, BSS.

Relocation types are keyed to the immediate-operand width the fixup patches. The
applier writes `(symbol_value + r_addend)` little-endian into the operand bytes at
`r_offset`, truncated to the type width, and errors if the value does not fit.

    0  R_MAIZE_NONE    no-op
    1  R_MAIZE_ABS8    patch 1 immediate byte   (absolute)
    2  R_MAIZE_ABS16   patch 2 immediate bytes  (absolute)
    3  R_MAIZE_ABS32   patch 4 immediate bytes  (absolute)   e.g. CP label Rn.H0
    4  R_MAIZE_ABS64   patch 8 immediate bytes  (absolute)   e.g. CP label Rn (full register)
    5..15 reserved for R_MAIZE_REL* (PC-relative) and future kinds

### Linker `mzld`

    mzld [-o out.mzx] [-e entry_symbol] in1.mzo [in2.mzo ...]
      -o   output path (default a.mzx)
      -e   entry symbol name (default _start)

`mzld` concatenates same-kind sections across objects (preserving input order,
honouring per-section alignment), lays them out from base address 0 in the order
CODE, RODATA, DATA, BSS, resolves symbols, applies relocations, resolves the entry
point, and writes a `.mzx`. It runs a **hygiene pass** whose failures are hard errors:
a section that is both writable and executable (W+X) is rejected; every relocation
must land inside its section and its value must fit the relocation width; the entry
symbol must resolve; and no two segments may overlap in the address space. Undefined
symbols and duplicate GLOBAL definitions are reported with a diagnostic naming the
symbol and the objects involved. In v1 the object that defines `_start` is placed
first, so `_start` lands at the reset vector (address 0).

### Executable format `.mzx`

    Header (24 bytes)
      0    4     magic       = 'M','Z','X', 0x01
      4    2     flags       (u16 = 0)
      6    2     seg_count   (u16)
      8    8     entry       (u64) absolute entry address
      16   8     shoff       (u64) file offset of the segment table

    Segment entry (40 bytes each; seg_count of them at shoff)
      0    1     kind        (u8) 1=CODE 2=RODATA 3=DATA 4=BSS
      1    1     attrs       (u8) EXEC/READ/WRITE/ALLOC/NOBITS (as in .mzo)
      2    6     reserved    (6 bytes = 0)
      8    8     vaddr       (u64) load address
      16   8     file_off    (u64) offset of contents in this file (0 for NOBITS)
      24   8     mem_size    (u64) bytes to occupy in memory
      32   8     file_size   (u64) bytes present in the file (0 for NOBITS)

To load a `.mzx`, `maize` walks the segment table, copies each segment's `file_size`
bytes to `vaddr`, zero-fills the `mem_size - file_size` (NOBITS) tail, and sets PC to
`entry`. A file that does not begin with the `.mzx` magic is loaded as a flat image at
address 0, exactly as before. RO/EXEC enforcement of the segment attributes is deferred
until the VM has memory-protection hardware; today they are honest, load-bearing
metadata that the linker's hygiene pass already validates.


## The C Toolchain (mzcc)

Maize has a working C11 compiler pipeline. [cproc](https://sr.ht/~mcf/cproc/) (the C11
front end) and [QBE](https://c9x.me/compile/) (the back end) are vendored as pinned git
submodules under `toolchain/` (see [toolchain/VENDORING.md](toolchain/VENDORING.md)), QBE
carries a Maize code-generation target, and a freestanding C runtime under `toolchain/rt`
(crt0, errno, string/ctype/stdio/stdlib, a brk-backed heap, variadic printf) is linked
into every program. The pipeline is:

    file.c -> cpp -> cproc (C11 -> QBE IL) -> qbe -t maize (IL -> .mazm)
           -> mazm -c (.mzo) -> mzld (+ C runtime) -> file.mzx

`mzcc` is a compiled tool, built and installed alongside `maize`, `mazm`, `mzld`, and
`mzdis`. It drives that whole pipeline itself, spawning `cproc-qbe`, `qbe`, `mazm`, and
`mzld` as needed and threading the intermediates through in memory, with a
content-addressed object cache so an unchanged translation unit is not recompiled.

- `mzcc file.c` compiles and links to `file.mzx` beside the source.
- `mzcc file.c -r` compiles and runs, propagating the guest exit code.
- `mzcc file.c --emit` also leaves `file.mazm` (the generated assembly) beside
  the source.
- `mzcc -o out.mzx file.c` writes the linked image to an explicit path.
- `mzcc --build` rebuilds the vendored cproc/qbe toolchain.

The whole pipeline builds and runs natively on Windows, no WSL and no MSYS2 required:
`mzcc` itself is a native binary, and the one piece it still shells out for is building
the vendored compiler. `mzcc --build` (and `scripts/install-mazm.ps1`, which calls the
same path) runs `scripts/build-toolchain.sh` to compile `cproc-qbe`/`qbe` directly with
the vendored llvm-mingw clang, skipping only the POSIX-only cproc driver binary (which
`mzcc` never uses anyway). That toolchain build is the sole place a POSIX shell is
needed on Windows, and Git Bash, which ships with Git for Windows, satisfies it; the
day-to-day `mzcc file.c` compile path invokes no shell at all.

cproc is strict C11: declare any libc-style function you call. Floating point is
supported, passing and returning through the general-purpose registers under the Zfinx
ABI. The remaining gaps are aggregates (passing or returning a struct by value) and
variadic float `va_arg`; in every case the ABI lowering reports an error rather than
miscompiling silently. The C register/frame ABI is documented in
[toolchain/qbe-maize/CALLING-CONVENTION.md](toolchain/qbe-maize/CALLING-CONVENTION.md) and
the syscall binding in [toolchain/rt/SYSCALL-ABI.md](toolchain/rt/SYSCALL-ABI.md).


## License

Maize is licensed under the Apache License, Version 2.0. See [LICENSE](LICENSE) for the full text.
