# Self-hosted framebuffer terminal

A guest program that turns Maize into a visible text terminal: an 8x8-font 40x25
character grid blitted into the memory-backed framebuffer, a minimal ANSI/VT escape
subset, and shift-aware Set-1 keyboard input echoed to the screen (a typewriter loop).
It is written in guest C and compiled through the `cc-maize.sh` pipeline; the only
non-C piece is the small device-access asm shim `toolchain/rt/mzdev.{mazm,h}`, which
exports the `IN`/`OUT` port stubs C cannot emit. That shim is the shared guest-C device
foundation a future DOOM port reuses.

## Files

- `toolchain/rt/mzdev.mazm`, `toolchain/rt/mzdev.h`: device-access shim; C-callable
  stubs over the frozen framebuffer/keyboard ports (`kbd_status`, `kbd_read`, `fb_width`,
  `fb_height`, `fb_format`, `fb_set_base`, `fb_present`, `fb_present_valid`).
- `font8x8.h`: embedded public-domain `font8x8_basic` (Daniel Hepper's font8x8, derived
  from the IBM PC BIOS font), ASCII 0x20..0x7F.
- `term_core.h`: the terminal engine (grid, glyph blitter, ANSI parser, scroll, Set-1
  translator) as `static` functions + state, compiled into a single translation unit.
- `terminal.c`: the interactive `main` (poll keyboard, echo). The SDL2 demo entry point.
- `terminal_selfcheck.c`: the headless CI fixture (render/ANSI read-back + keyboard echo).

## What it supports

- 40 columns x 25 rows of 8x8 cells at the default 320x200 XRGB8888 framebuffer.
- Printable ASCII plus CR, LF, Backspace, and Tab.
- ANSI/VT subset: cursor motion `CUU/CUD/CUF/CUB` (`ESC[nA..D`, default 1, clamped),
  `CUP` (`ESC[r;cH` and `;f`, 1-based, clamped); erase `ED` (`ESC[nJ`, n=0/1/2) and
  `EL` (`ESC[nK`, n=0/1/2); `SGR` (`ESC[...m`): reset `0`, foreground `30`-`37`,
  background `40`-`47` over the 8 basic ANSI colors. Malformed / incomplete escapes are
  consumed defensively (never hang, never read out of bounds).
- Bottom-of-screen advance scrolls the grid up one line (no scrollback / history).
- Set-1 (US layout) scancode to ASCII, shift-aware: LShift/RShift set shift, their
  releases clear it; Enter emits CR then LF; Backspace, Tab, Space handled; other break
  codes ignored.

Out of scope (first cut): no fork/exec/pty/shell (hosting a real shell like oksh is a later
step), no scrollback, no
alternate screen, no UTF-8, no full VT100, no mouse, no IRQ-driven input (polled).

## Headless self-check (the CI gate)

The deterministic gate is `terminal_selfcheck.c`, wired into `scripts/run-ctest.sh` (the
single cross-platform C harness, which self-dispatches under MSYS on the Windows runner).
It runs on both Linux CI and Windows. To run it directly:

    # Build the C toolchain first if needed: scripts/refresh-c-toolchain.sh
    scripts/cc-maize.sh --dev -o /tmp/terminal_selfcheck.mzx demos/terminal/terminal_selfcheck.c
    printf '\036\052\036\252\002\052\002\252\071' \
        | build/<preset>/maize --input=keyboard /tmp/terminal_selfcheck.mzx
    # expected stdout: terminal: PASS

Phase A drives `term_write` with a fixed ASCII+escape script and verifies the rendered
pixels by reading the guest-RAM framebuffer back (the `asm/test_framebuffer.mazm`
pattern). Phase B injects the Set-1 scancode sequence above (`1E`, shifted `1E`, `02`,
shifted `02`, `39` = `a A 1 ! space`) and checks the echoed glyph cells.

The `--dev` flag is an opt-in that appends `mzdev.mzo` to the link; the default C link
for every other fixture is unchanged.

## Interactive window demo (manual, not a CI gate)

The visible window uses the SDL2 backend, which is compiled in only with
`MAIZE_DISPLAY=ON` (it is never part of the default headless build, so the gate needs no
external dependency). Build a display-enabled `maize`, build the terminal image, and run:

    # 1. Build maize with the SDL2 display backend (needs SDL2 dev libraries).
    cmake --preset linux-debug -DMAIZE_DISPLAY=ON
    cmake --build build/linux-debug --target maize

    # 2. Build the interactive terminal image (guest C + the mzdev shim).
    scripts/cc-maize.sh --dev -o demos/terminal/terminal.mzx demos/terminal/terminal.c

    # 3. Open the window; type and watch characters echo as glyph cells.
    build/linux-debug/maizeg --display --input=keyboard demos/terminal/terminal.mzx

On Windows, build the SDL2-enabled `maize.exe` per the Windows preset and run the same
`--display --input=keyboard` command against `terminal.mzx`. Close the window to exit.
