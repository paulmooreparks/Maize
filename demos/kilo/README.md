# demos/kilo: antirez's kilo editor on Maize

This directory hosts the Maize port of `kilo`, Salvatore Sanfilippo's ~1000-line
single-file terminal text editor. kilo is the forcing function that proves the
Maize graphical console (maize-140) is a real terminal, not a print-only glass
TTY: a hand-rolled shell exercises only cooked-line input and printf, but a
full-screen editor demands the scary half of the terminal contract (raw mode,
cursor-addressing escapes out, arrow-key escapes in, and a blocking read that
parks the CPU).

## Layout

- `kilo.c`: the vendored editor, `github.com/antirez/kilo`, BSD-2-Clause. The PORT
  needed NO source changes: it compiled clean through cproc + qbe with no
  register-pressure wall, and every build/run accommodation lives OUTSIDE this file,
  in the shared C runtime (`toolchain/rt`) and the host console (`src/devices.cpp`).
  The ONLY edits to kilo.c are a small, clearly-marked "Maize-local patch" block in
  editorProcessKeypress (maize-172 acceptance round) for basic editor usability:
  a real forward-delete on the DEL key, plus Home/End handlers, three keys upstream
  kilo left unhandled (DEL was aliased to Backspace; Home/End fell to default-insert).
  The upstream license header and the rest of the body are preserved verbatim.
- `LICENSE`: kilo's upstream BSD-2-Clause license, copied verbatim.

## Build

kilo compiles clean through the Maize C toolchain (cproc + qbe -t maize) with no
source changes and no qbe register-pressure wall, into a single linked image:

    scripts/cc-maize.sh -o kilo.mzx demos/kilo/kilo.c

## Run in the window (operator)

Build a `maize` with the SDL display backend (`cmake -DMAIZE_DISPLAY=ON ...`), then
open a file from a mounted sandbox directory. The console is an 80x50 cell grid
(640x400 at the 8x8 font):

    maizeg --display --mount "<your-dir>=/work:rw" demos/kilo/kilo.mzx /work/somefile.c

Controls are kilo's own: arrow keys / Home / End / PageUp / PageDown / Delete to
move, printable keys to insert, Backspace to delete, Ctrl-O to open a file,
Ctrl-S to save, Ctrl-F to find, Ctrl-Q to quit (three times if there are
unsaved changes).

## Headless smoke run

The window console can be bound headlessly with `--console-dump`, which reads Set-1
scancodes from stdin and dumps the final text grid on exit. This opens a file,
inserts "hi" at the top, saves (Ctrl-S), and quits (Ctrl-Q):

    printf '\043\027\035\037\020' \
        | maize --console-dump --mount "<dir>=/work:rw" demos/kilo/kilo.mzx /work/file.c

Ctrl-O opens a different file mid-session without restarting the VM. Because
Ctrl is a sticky modifier on this console (the break code, not key-up timing,
clears it), a Ctrl-O test that goes on to type a path must send the Ctrl break
code before the path's letters, or every letter folds through the same
ctrl-mask as Ctrl-O did. This sequence opens with `/work/file.c` on the
command line, presses Ctrl-O on the clean (unmodified) buffer so the prompt
appears immediately, retypes the same in-mount path, accepts with Enter, and
quits:

    printf '\035\030\235\065\021\030\023\045\065\041\027\046\022\064\056\034\035\020' \
        | maize --console-dump --mount "<dir>=/work:rw" demos/kilo/kilo.mzx /work/file.c

## The terminal contract kilo established

kilo is the first program to exercise the console's non-cooked path end to end.
The subset it actually drove, all honored by the console and libc (none stubbed to
a no-op), is the frozen contract the console now commits to:

### termios (line discipline)

kilo hand-rolls raw mode instead of calling `cfmakeraw`: it reads the current
attributes with `tcgetattr`, clears the canonical/echo/signal and translation
flags, and installs them with `tcsetattr(TCSAFLUSH)`. The flags whose clearing the
console honors materially are `ICANON` (canonical line editing off, so each byte is
delivered as it arrives), `ECHO` (no local echo, kilo paints the screen itself),
and `ISIG` (Ctrl-C / Ctrl-Z are delivered as bytes, not turned into signals). The
remaining raw-mode flags kilo clears (`BRKINT`, `INPCK`, `ISTRIP`, `IXON`,
`IEXTEN`, `OPOST`, and `CS8`) have no host layer to affect (there is no parity,
break, flow-control, or output-post-processing stage), so clearing them is a
defined no-op; the flag bits are defined in `toolchain/rt/termios.h` only so a
Linux-written editor compiles without field surgery. `VMIN`/`VTIME` are carried in
the termios image; the console's blocking read returns each byte as it arrives.

### VT100 escape sequences OUT (guest -> console)

kilo emits these directly, and the console's VT engine (`src/devices.cpp`
`csi_dispatch` / `out_byte`) interprets them:

- Cursor addressing: `ESC[<row>;<col>H` (CUP), and relative `ESC[<n>A/B/C/D` (up /
  down / right / left, used by the window-size probe to jam the cursor to the
  bottom-right corner).
- Erase: `ESC[K` / `ESC[0K` (erase to end of line) and `ESC[2J` (erase screen).
- Cursor visibility: `ESC[?25l` (hide) and `ESC[?25h` (show) during a repaint, so
  the screen does not flicker mid-frame.
- Color: `ESC[<n>m` SGR for the syntax highlighter (foreground 30-37, background
  40-47, and reset 0 / 39).
- The control bytes CR, LF, Backspace, and Tab.

Two output-side details kilo forced into existence in this card:

- Device Status Report: `ESC[6n` now makes the console reply on the INPUT stream
  with `ESC[<row>;<col>R`. This is how kilo discovers the window size when there is
  no `ioctl(TIOCGWINSZ)` (Maize has no ioctl multiplexor): it homes the cursor to
  the far bottom-right and reads back the clamped position. Without this the editor
  could not size its screen and would abort at startup.
- DEC deferred wrap (VT100 auto-margin): writing a glyph to the last column no
  longer advances the cursor immediately; the wrap is deferred until the next glyph.
  kilo's status bar spans the full width of the last row, and without deferred wrap
  that full-width write scrolled the whole screen up by one line. This is the
  standard right-margin behavior of a real VT100/xterm.

### Escape sequences IN (console -> guest)

Physical keys arrive as Set-1 scancodes and the console's `keymap` encodes the
navigation keys as the exact multi-byte escapes kilo parses: arrows as
`ESC[A/B/C/D`, Home as `ESC[H`, End as `ESC[F`, PageUp/PageDown as `ESC[5~`/`ESC[6~`,
Delete as `ESC[3~`. Enter arrives as CR, Backspace as DEL (0x7F), and Ctrl-<key>
folds to the matching control byte. kilo's `editorReadKey` reassembles these into
its internal key codes.

### Blocking read

kilo's read loop blocks on stdin until a key is available. The console's `read_in`
parks the CPU (HALT on the run-bit substrate) while the input queue is empty and
wakes on a keyboard IRQ, so an idle editor burns no cycles (no busy-spin).

## File save on shrink (resolved, maize-179)

`ftruncate` is a real syscall (`SYS $4D`, the Linux x86-64 number 77) routed through
the confined hostfs backend: it resizes the open file to exactly the new length, `:rw`
gated (a `:ro` mount returns `EROFS`) and confined like the other mutating ops. kilo's
save rewrites the whole buffer after `ftruncate`, so saving a file that has become
SHORTER is now byte-exact with no stale tail. (Earlier this was a documented PARTIAL
shim that did not resize the file; that limitation is gone.)
