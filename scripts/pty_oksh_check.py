#!/usr/bin/env python3
# pty_oksh_check.py (maize-94): the real-KEYSTROKE end-to-end acceptance proof for the
# interactive oksh shell on the DEFAULT input path (AC 8930). Piped-stdin and `oksh -c`
# fixtures have now missed the interactive input-routing class of bug twice, so this
# fixture PRESSES KEYS: it forks `maize <quesos.mzx> /bin/oksh.mzx` into a real pseudo
# terminal (no --input flag, exactly the operator's invocation), waits for the prompt,
# then writes keystrokes ("pwd", a distinctive echo, "exit") and asserts the shell echoed
# them, executed the commands (pwd prints "/", echo prints the marker), and exited clean.
#
# CI-safe: uses only the stdlib pty module on a Linux runner. On Windows the equivalent is
# a ConPTY harness (not wired into CI here); this Linux variant is the gating acceptance
# check. Usage: pty_oksh_check.py <maize> <quesos.mzx> <bin-dir> <rw-dir>
#
# Exit 0 on PASS (prints "pty-oksh: PASS"), 1 on any failure (prints a diagnostic + the
# full captured transcript so a regression is debuggable from the CI log).
import os, pty, select, sys, time

if len(sys.argv) != 5:
    sys.stderr.write("usage: pty_oksh_check.py <maize> <quesos.mzx> <bin-dir> <rw-dir>\n")
    sys.exit(2)

maize, quesos, bindir, rwdir = sys.argv[1:5]
marker = "hi-from-keystrokes"
argv = [maize, "--no-root",
        "--mount", bindir + "=/bin:ro",
        "--mount", rwdir + "=/rw:rw",
        quesos, "/bin/oksh.mzx"]

pid, fd = pty.fork()
if pid == 0:
    # Child: the pty slave is stdin/stdout/stderr. No --input flag: the DEFAULT path.
    os.execv(argv[0], argv)
    os._exit(127)

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
    return True

def wait_for(substr, seconds):
    end = time.time() + seconds
    while time.time() < end:
        if substr.encode() in captured:
            return True
        pump(0.3)
    return substr.encode() in captured

ok = True
# Boot + first prompt. oksh's default PS1 ends in "# "; require it before typing.
if not wait_for("# ", 20):
    ok = False
# Type commands as keystrokes. CR (\r) is Enter on a raw tty.
os.write(fd, b"pwd\r")
wait_for("/", 10)
os.write(fd, ("echo " + marker + "\r").encode())
wait_for(marker, 10)
os.write(fd, b"exit\r")
pump(5.0)

try:
    os.close(fd)
except OSError:
    pass
try:
    _, status = os.waitpid(pid, 0)
except OSError:
    status = -1

text = bytes(captured).decode("utf-8", "replace")

# Acceptance: the prompt was reached, both typed commands executed, shell reaped clean.
fail = None
if "# " not in text:
    fail = "no-prompt"
elif marker not in text:
    fail = "echo-keystroke-not-executed"
elif text.count("/") < 2:   # the /bin/oksh.mzx warning has one '/'; pwd's own "/" adds more
    fail = "pwd-not-executed"
elif "unhandled syscall" in text:
    fail = "unhandled-syscall"

if fail is not None or not ok:
    sys.stdout.write("pty-oksh: FAIL %s\n" % (fail or "no-prompt"))
    sys.stdout.write("---captured---\n" + text + "\n---end---\n")
    sys.exit(1)

sys.stdout.write("pty-oksh: PASS\n")
sys.exit(0)
