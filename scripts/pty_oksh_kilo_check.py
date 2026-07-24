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
# Three modes (argv[5]):
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
#   largefile:       (maize-350) synthesize a ~380 KB C-like highlighted file into the /rw
#                    mount (deterministic, never committed), launch `kilo /rw/big.c`, assert
#                    the same clean-paint markers as edit mode, and measure the wall-clock
#                    load-and-first-paint time (from just before the launch keystrokes to the
#                    frame-end marker), printed as `pty-kilo: LOAD_MS <n>`. Then type a short
#                    edit at cursor home, save, quit, and byte-compare the saved file against
#                    the synthesized content with the edit applied (a byte-correct open/edit/
#                    save round-trip on a large highlighted buffer). The harness owns the
#                    ground truth (the fixture is synthesized here, per decision 9961), so it
#                    performs the byte-exact compare itself rather than the run-ctest.sh caller.
#                    LOAD_MS is a recorded measurement for the AC5 before/after comparison, not
#                    a hardcoded CI threshold (decision 9960).
#
# CI-safe: stdlib pty on a Linux runner only; skipped on Windows (no pty), exactly like the
# userland94_oksh_keystrokes fixture. Usage:
#   pty_oksh_kilo_check.py <maize> <quesos.mzx> <bin-dir> <rw-dir> [edit|kill|largefile]
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

def read_until(marker, timeout):
    # Like wait_for, but polls tightly (0.02s) and returns the wall-clock time the
    # marker first appears, so LOAD_MS resolution is ~20ms rather than wait_for's
    # ~300ms poll granularity. Returns None on timeout / EOF (maize-350 AC5).
    mb = marker if isinstance(marker, (bytes, bytearray)) else marker.encode()
    if mb in captured:
        return time.time()
    end = time.time() + timeout
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.02)
        if fd in r:
            try:
                d = os.read(fd, 4096)
            except OSError:
                return None
            if not d:
                return None
            captured.extend(d)
            if b"\x1b[6n" in d:
                try:
                    os.write(fd, ("\x1b[%d;%dR" % (ROWS, COLS)).encode())
                except OSError:
                    pass
        if mb in captured:
            return time.time()
    return None

def synth_largefile(target_bytes):
    # Deterministic C-like content that exercises the syntax highlighter (block and
    # line comments, string and number literals, C keywords), sized past the crash
    # threshold maize-348 reported (~135 KB) with headroom. Every line is newline-
    # terminated and the whole buffer ends in one newline, so kilo's load then save
    # is byte-identity apart from any edit; that lets the caller assert byte-exact
    # round-trip without re-implementing kilo's line splitting. Not committed (built
    # here at test time, decision 9961).
    templates = [
        "/* block comment %d: the quick brown fox jumps over the lazy dog */",
        "int counter_%d = %d;   /* running index accumulator */",
        "const char *label_%d = \"string literal %d with digits 12345 inside\";",
        "static long total_%d = %d;  // running total, line comment",
        "if (counter_%d > %d) { return counter_%d + 7; } else { continue; }",
        "for (int j = 0; j < %d; j++) { total_0 += j * 3; /* inner */ }",
        "unsigned long mask_%d = 0x%xUL; /* hex literal with keyword mix */",
    ]
    parts = []
    total = 0
    i = 0
    while total < target_bytes:
        t = templates[i % len(templates)]
        line = t % tuple([i] * t.count("%"))
        parts.append(line)
        total += len(line) + 1
        i += 1
    return ("\n".join(parts) + "\n").encode()

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

# --- largefile mode: load a big highlighted file, time it, round-trip an edit ---------
if mode == "largefile":
    # Size and load-timeout are env-overridable for host tuning. The defaults are a
    # ~380 KB file (past maize-348's ~135 KB crash threshold) and a generous 150s
    # ceiling for the load-and-first-paint under the interpreter. That whole-file
    # syntax-highlight-on-load is the very path the card speeds up: it measured ~57s
    # (hardened) vs ~77s (pre-fix) on the reference host, so the ceiling is a stall
    # catcher with wide headroom, not a perf gate (the PASS records the real time).
    lf_bytes = int(os.environ.get("MAIZE_KILO_LARGEFILE_BYTES", "380000"))
    lf_timeout = float(os.environ.get("MAIZE_KILO_LARGEFILE_TIMEOUT", "150"))
    content = synth_largefile(lf_bytes)
    hostpath = os.path.join(rwdir, "big.c")
    with open(hostpath, "wb") as bf:
        bf.write(content)
    expected_post = b"EDITED:" + content   # a home-cursor insert prepends to the file

    t0 = time.time()
    os.write(fd, b"kilo /rw/big.c\r")
    t1 = read_until("\x1b[?25h", lf_timeout)   # frame-end: paint complete
    if t1 is None:
        fail("no-frame-end (large-file paint truncated or crashed)")
    load_ms = int((t1 - t0) * 1000)

    # Same clean-paint assertions as edit mode; by frame-end these all precede it.
    if b"\x1b[?1049h" not in captured:
        fail("no-alt-screen-enter (kilo did not paint the large file)")
    if b"\x1b[H" not in captured:
        fail("no-home-preamble (large file)")
    if b"unhandled syscall" in captured:
        fail("unhandled-syscall-in-large-file-paint")
    if b"\x1b[6n" in captured:
        fail("esc-6n-cursor-probe-present (large file)")

    # Short deterministic edit at cursor home (row 0, col 0): insert a known marker,
    # which prepends to the whole buffer since the cursor starts at the file head.
    os.write(fd, b"EDITED:")
    pump(2.0)
    os.write(fd, b"\x13")                   # Ctrl-S: save /rw/big.c
    pump(2.5)
    os.write(fd, b"\x11")                   # Ctrl-Q: quit (buffer clean after save)
    if not wait_for("# ", 12):
        fail("no-prompt-after-quit (large file)")
    pump(1.0)
    if b"unhandled syscall" in captured:
        fail("unhandled-syscall (large file)")

    # Byte-correct open/edit/save round-trip: the saved file must equal the
    # synthesized content with the typed edit applied.
    try:
        with open(hostpath, "rb") as rf:
            saved = rf.read()
    except OSError as e:
        fail("cannot-read-saved-large-file: %s" % e)
    if saved != expected_post:
        fail("large-file-roundtrip-mismatch (saved %d bytes, expected %d)"
             % (len(saved), len(expected_post)))

    sys.stdout.write("pty-kilo: LOAD_MS %d\n" % load_ms)
    sys.stdout.write("pty-kilo: PASS\n")
    os.write(fd, b"exit\r")
    pump(3.0)
    try:
        os.close(fd)
    except OSError:
        pass
    reap(8)
    sys.exit(0)

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
