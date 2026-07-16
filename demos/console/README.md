# First-class graphical console

The SDL window as a first-class glass TTY (maize-140). Ordinary stdio guest programs run
in the window with zero special wiring: the guest writes BYTES on fd 1/2 and the HOST
renders GLYPHS; physical keystrokes become bytes on fd 0. This is the host/VM console
device (approach (i)): the terminal engine lives in host C++ (`src/devices.cpp`,
`text_console`), reusing the maize-121 `term_core` VT/ANSI logic, and is bound to fd 0/1/2
through the `console_io` seam in `src/sys.cpp` when the window console is active.

It is distinct from `demos/terminal` (a self-hosted GUEST program that draws to the raw
framebuffer over the `mzdev` port shim). Here the guest does nothing special: `printf`,
`fgets`, and `read()` just work in the window.

## What it supports

- 80 columns x 50 rows of 8x8 cells at a 640x400 default console resolution (distinct from
  DOOM's 320x200 framebuffer default). Reuses the vetted `font8x8` data.
- VT output: printable ASCII plus CR, LF, Backspace, Tab; cursor motion `CUU/CUD/CUF/CUB`,
  `CUP` (`ESC[r;cH` / `;f`); erase `ED` (`ESC[nJ`) and `EL` (`ESC[nK`); `SGR` (`ESC[...m`)
  reset + foreground `30`-`37` + background `40`-`47`; right-margin wrap; bottom-of-screen
  scroll. Malformed / incomplete escapes are consumed defensively.
- Physical keys -> stdin: a shift/ctrl-aware Set-1 keymap producing printable bytes,
  control bytes (`Ctrl-<letter>`), and VT INPUT escape sequences for the arrow / Home /
  End / PageUp / PageDown / Delete / Insert keys.
- Full termios line discipline (`tcgetattr` / `tcsetattr` / `cfmakeraw`, SYS `$F1`/`$F2`):
  cooked mode (ICANON+ECHO) does line buffering, local echo, Backspace editing, and
  deliver-on-Enter with zero guest-side line editing; raw mode delivers each byte with no
  echo. Built to kilo's contract (maize-172).
- A blocking `read()` on empty stdin parks the CPU on the run-bit substrate (no busy-spin)
  and returns on the next keypress.

Console vs graphics: the text console owns the window by default; a raw-framebuffer program
(DOOM) takes it over for its run by explicitly programming the framebuffer base port,
mutually exclusive per run.

Out of scope (deferred): scrollback, window resize, SGR beyond the basic 8 colors, mouse,
a guest shell program, and the raw-mode + input-escape correctness proof against a real
editor (maize-172, kilo).

## Files

- `console_selfcheck.c`: the headless CI fixture (VT output + keymap + cooked/raw delivery).

The console engine itself is host C++ (`src/devices.cpp` `text_console`, `src/console_io.h`,
the termios dispatch in `src/sys.cpp`); the guest side is plain libc plus the termios slice
`toolchain/rt/termios.{h,c}`.

## Headless self-check (the CI gate)

Wired into `scripts/run-ctest.sh`. The fixture is a plain stdio + termios program (no
`--dev` shim). `--console-dump` binds the console with no window, sourcing scancodes from
host stdin, and dumps the rendered grid as text at exit for the harness to grep. To run it
directly:

    # Build the C toolchain first if needed: scripts/build-toolchain.sh
    scripts/cc-maize.sh -o /tmp/console_selfcheck.mzx demos/console/console_selfcheck.c
    printf '\043\054\016\027\034\055' \
        | build/<preset>/maize --no-root --console-dump /tmp/console_selfcheck.mzx
    # the grid dump contains the VT-output markers and a "console: PASS" line

The injected scancodes are `h Z Backspace i Enter x`: the cooked read delivers `hi\n`
(the Backspace erases the `Z`), then raw mode returns the single `x`.

## Interactive window (manual, not a CI gate)

The visible window uses the SDL2 backend, compiled in only with `MAIZE_DISPLAY=ON`:

    # 1. Build maize with the SDL2 display backend (needs SDL2 dev libraries).
    cmake --preset linux-debug -DMAIZE_DISPLAY=ON
    cmake --build build/linux-debug --target maize

    # 2. Run any ordinary stdio program in the window (no special build).
    build/linux-debug/maizeg --display <your-stdio-program>.mzx

Type and watch output render as glyphs; physical keys reach the program's stdin. Close the
window to exit.
