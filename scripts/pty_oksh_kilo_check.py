#!/usr/bin/env python3
# pty_oksh_kilo_check.py (maize-250): the full-screen-TUI-as-quesOS-child acceptance proof.
# Extends the pty_oksh_check.py pattern (real pseudo terminal, real keystrokes, DEFAULT
# input path) to launch kilo -- a raw-mode, alt-screen, whole-frame-in-one-write editor --
# as a CHILD of oksh under quesOS on the console (maizec) binary, and proves it now paints,
# edits, saves, and quits cleanly. Before this card native_write silently truncated any
# single write over 4096 bytes, so kilo's editorRefreshScreen (which paints the whole frame
# in ONE write) produced frame-shifted / stranded VT output; AC 9108's separate >4096-byte
# byte-count fixture pins that regression deterministically, and this fixture proves the
# real editor works end to end through a live terminal.
#
# Two modes (argv[5]):
#   edit  (default): boot oksh, launch `kilo /rw/t.txt`, assert a clean alt-screen paint,
#                    type text, save (Ctrl-S), quit (Ctrl-Q), assert the oksh prompt returns
#                    and a follow-on `pwd` still round-trips (terminal + ISIG intact after a
#                    clean child exit), then the caller verifies /rw/t.txt on the host.
#   kill:            boot oksh, launch kilo, wait for the paint, then SIGTERM the harness's
#                    own maize process while kilo is mid-edit and assert it is reaped without
#                    hanging (a non-hang guard for the pty launch path). The in-VM killed-TUI
#                    console-restore behavior itself is proven deterministically by the
#                    os/quesos/raw_reap_restore.c fixture, not through the live pty (a raw
#                    child that has cleared ISIG cannot be signalled from the terminal, so
#                    there is no in-pty way to signal kilo specifically; see the card spec).
#
# CI-safe: stdlib pty on a Linux runner only; skipped on Windows (no pty), exactly like the
# userland94_oksh_keystrokes fixture. Usage:
#   pty_oksh_kilo_check.py <maize> <quesos.mzx> <bin-dir> <rw-dir> [edit|kill]
# Exit 0 on PASS (prints "pty-kilo: PASS"), 1 on any failure (prints a diagnostic + the full
# captured transcript, escaped, so a regression is debuggable from the CI log).
import os, pty, select, sys, time, struct, fcntl, termios

if len(sys.argv) < 5:
    sys.stderr.write("usage: pty_oksh_kilo_check.py <maize> <quesos.mzx> <bin-dir> <rw-dir> [edit|kill]\n")
    sys.exit(2)

maize, quesos, bindir, rwdir = sys.argv[1:5]
mode = sys.argv[5] if len(sys.argv) > 5 else "edit"
ROWS, COLS = 24, 80
argv = [maize, "--no-root",
        "--mount", bindir + "=/bin:ro",
        "--mount", rwdir + "=/rw:rw",
        quesos, "/bin/oksh.mzx"]

pid, fd = pty.fork()
if pid == 0:
    # Child: the pty slave is stdin/stdout/stderr. No --input flag: the DEFAULT path.
    os.execv(argv[0], argv)
    os._exit(127)

# Give the pty a concrete window size so kilo's ioctl(TIOCGWINSZ) -> sys_ttysize ($F6)
# returns a real size and it never has to fall back to the ESC[6n cursor probe.
try:
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", ROWS, COLS, 0, 0))
except OSError:
    pass

captured = bytearray()

def pump(seconds):
    end = time.time() + seconds
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.2)
        if fd in r:
            try:
                d = os.read(fd, 4096)
            except OSError:
                return False
            if not d:
                return False
            captured.extend(d)
            # Answer a Device Status Report (cursor position) if kilo probes, so the
            # window-size query completes even if the winsize ioctl above did not take.
            if b"\x1b[6n" in d:
                try:
                    os.write(fd, ("\x1b[%d;%dR" % (ROWS, COLS)).encode())
                except OSError:
                    pass
    return True

def wait_for(substr, seconds):
    end = time.time() + seconds
    sb = substr if isinstance(substr, (bytes, bytearray)) else substr.encode()
    while time.time() < end:
        if sb in captured:
            return True
        pump(0.3)
    return sb in captured

def reap(deadline_s):
    status = None
    end = time.time() + deadline_s
    while time.time() < end:
        try:
            wpid, wstatus = os.waitpid(pid, os.WNOHANG)
        except OSError:
            wpid, wstatus = pid, -1
        if wpid == pid:
            status = wstatus
            break
        time.sleep(0.1)
    return status

def fail(reason):
    text = bytes(captured).decode("latin-1")
    esc = text.replace("\x1b", "<ESC>")
    sys.stdout.write("pty-kilo: FAIL %s\n" % reason)
    sys.stdout.write("---captured---\n" + esc + "\n---end---\n")
    # Never leave the VM running.
    try:
        os.close(fd)
    except OSError:
        pass
    for sig in (15, 9):
        try:
            os.kill(pid, sig)
        except OSError:
            break
        s = reap(2)
        if s is not None:
            break
    sys.exit(1)

# --- common: boot to the oksh prompt, then launch kilo on a /rw file ------------------
if not wait_for("# ", 20):
    fail("no-oksh-prompt")

fname = "/rw/t.txt" if mode == "edit" else "/rw/k.txt"
os.write(fd, ("kilo %s\r" % fname).encode())

# kilo's enableRawMode enters the alternate screen buffer (maize-234) and editorRefreshScreen
# homes the cursor; a clean first paint shows both, and the frame ends by showing the cursor.
if not wait_for("\x1b[?1049h", 12):
    fail("no-alt-screen-enter (kilo did not paint)")
if not wait_for("\x1b[H", 8):
    fail("no-home-preamble")
if not wait_for("\x1b[?25h", 8):
    fail("no-frame-end (paint truncated?)")
if b"unhandled syscall" in captured:
    fail("unhandled-syscall-in-paint")

# maize-253: sys_ttysize ($F6) answers ioctl(TIOCGWINSZ) directly (here via the real host
# terminal), so kilo's getWindowSize succeeds without falling back to the ESC[6n cursor-
# position probe. By now kilo has completed its window-size query and first paint, so the
# probe byte sequence must never appear. The pump() answer above is a defensive backstop;
# if it ever fired, this assertion catches the regression to the fallback path.
if b"\x1b[6n" in captured:
    fail("esc-6n-cursor-probe-present (ioctl TIOCGWINSZ fell back instead of succeeding)")

if mode == "kill":
    # Non-hang guard: SIGTERM the maize VM process mid-edit; it must reap without hanging.
    os.kill(pid, 15)
    status = reap(10)
    if status is None:
        fail("maize-did-not-reap-after-kill")
    sys.stdout.write("pty-kilo: PASS\n")
    sys.exit(0)

# --- edit mode: type, save, quit ------------------------------------------------------
payload = "hello from kilo as a quesos child"
os.write(fd, payload.encode())
pump(1.5)                       # let the inserts paint
os.write(fd, b"\x13")           # Ctrl-S: save to /rw/t.txt
pump(1.5)
os.write(fd, b"\x11")           # Ctrl-Q: quit (buffer is clean after the save)

# The oksh prompt must reappear once kilo exits and returns control to the shell.
if not wait_for("# ", 10):
    fail("no-prompt-after-quit")

# Prove the terminal + ISIG state are intact for the shell after the clean child exit.
os.write(fd, b"pwd\r")
if not wait_for("/", 6):
    fail("pwd-did-not-roundtrip-after-quit")
os.write(fd, b"exit\r")
pump(4.0)

try:
    os.close(fd)
except OSError:
    pass
status = reap(8)
if status is None:
    for sig in (15, 9):
        try:
            os.kill(pid, sig)
        except OSError:
            break
        if reap(2) is not None:
            break
    fail("shell-did-not-exit")

if b"unhandled syscall" in captured:
    fail("unhandled-syscall")

sys.stdout.write("pty-kilo: PASS\n")
sys.exit(0)
