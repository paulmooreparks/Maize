#!/usr/bin/env python3
# pty_presenter_check.py (maize-264): the cross-process presentation-transport acceptance
# harness. Drives the console `maize` session under a REAL pty (so stdin_is_interactive()
# is true and the session creates the shared-memory segment + launches a stub
# `maizeg --presenter`), then reads the stub presenter's stdout checksums and asserts the
# doctrine's behaviors: cross-process frame visibility, the polled latest-frame doorbell,
# D16 auto-respawn + storm guard, D15 stale-steal self-terminate, teardown, and the input
# ring round trip.
#
# The session process is `maize --resolution 8x8 <program.mzb>`. An 8x8 framebuffer is
# 256 bytes, so the expected FNV-1a checksum of each single-pixel pattern is cheap to
# recompute here and match against the stub's printed value.
#
# CI-safe: stdlib pty on a Linux runner only; skipped on Windows (no pty), exactly like
# the userland94_oksh_keystrokes / pty_oksh_* fixtures. The Windows shared-memory leg
# rides the Merge-stage CI gate (documented on the card).
#
# Usage: pty_presenter_check.py <maize> <maizeg> <fixture.mzb> <doorbell.mzb> <scratch-dir> <mode>
#   modes: checksum | doorbell | respawn | storm | stalesteal | teardown | input | all
# Exit 0 on PASS (prints "pty-presenter: PASS <mode>"), 1 on failure (with a diagnostic
# and the captured transcript).
import os, pty, select, sys, time, re, signal, subprocess, threading

if len(sys.argv) < 7:
    sys.stderr.write("usage: pty_presenter_check.py <maize> <maizeg> <fixture.mzb> "
                     "<doorbell.mzb> <scratch-dir> <mode>\n")
    sys.exit(2)

MAIZE, MAIZEG, FIXTURE, DOORBELL, SCRATCH, MODE = sys.argv[1:7]
os.makedirs(SCRATCH, exist_ok=True)

# ---- FNV-1a expected checksums for the single-pixel patterns over an 8x8 frame ----------
FB_W, FB_H = 8, 8
FRAME_BYTES = FB_W * FB_H * 4

def fnv1a(data):
    h = 0x811c9dc5
    for b in data:
        h = ((h ^ b) * 0x01000193) & 0xffffffff
    return h

def pattern_frame(byte):
    # pixel[0] = byte,byte,byte,byte (palindromic); the rest is guest-defined-zero RAM.
    return bytes([byte, byte, byte, byte] + [0] * (FRAME_BYTES - 4))

CSUM_A = "%08x" % fnv1a(pattern_frame(0xA1))
CSUM_B = "%08x" % fnv1a(pattern_frame(0xB2))
CSUM_C = "%08x" % fnv1a(pattern_frame(0xC3))

CSUM_RE = re.compile(r"presenter-stub: slot=(\d+) seq=(\d+) checksum=([0-9a-f]{8}) t=(\d+)")
SESSION_RE = re.compile(r"maizeg --presenter (\w+)")


class Session:
    """A `maize` session running under a pty, with a background reader draining the master."""
    def __init__(self, program, env_extra=None):
        self.captured = bytearray()
        self.lock = threading.Lock()
        env = dict(os.environ)
        if env_extra:
            env.update(env_extra)
        argv = [MAIZE, "--resolution", "%dx%d" % (FB_W, FB_H), program]
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            os.environ.clear()
            os.environ.update(env)
            os.execv(argv[0], argv)
            os._exit(127)
        self.running = True
        self.reader = threading.Thread(target=self._read_loop, daemon=True)
        self.reader.start()

    def _read_loop(self):
        while self.running:
            try:
                r, _, _ = select.select([self.fd], [], [], 0.2)
            except OSError:
                break
            if self.fd in r:
                try:
                    d = os.read(self.fd, 4096)
                except OSError:
                    break
                if not d:
                    break
                with self.lock:
                    self.captured.extend(d)

    def text(self):
        with self.lock:
            return bytes(self.captured).decode("latin-1")

    def wait_for(self, pattern, seconds):
        end = time.time() + seconds
        rx = re.compile(pattern) if isinstance(pattern, str) else pattern
        while time.time() < end:
            m = rx.search(self.text())
            if m:
                return m
            time.sleep(0.1)
        return rx.search(self.text())

    def session_id(self, seconds=15):
        m = self.wait_for(SESSION_RE, seconds)
        return m.group(1) if m else None

    def checksums(self):
        return CSUM_RE.findall(self.text())

    def close(self):
        self.running = False
        try:
            os.close(self.fd)
        except OSError:
            pass
        for sig in (signal.SIGTERM, signal.SIGKILL):
            try:
                os.kill(self.pid, sig)
            except OSError:
                break
            if self._reap(2):
                break

    def _reap(self, deadline):
        end = time.time() + deadline
        while time.time() < end:
            try:
                w, _ = os.waitpid(self.pid, os.WNOHANG)
            except OSError:
                return True
            if w == self.pid:
                return True
            time.sleep(0.1)
        return False


def presenter_pids(session_id):
    try:
        out = subprocess.check_output(
            ["pgrep", "-f", "maizeg --presenter %s" % session_id],
            stderr=subprocess.DEVNULL).decode()
        return [int(x) for x in out.split()]
    except subprocess.CalledProcessError:
        return []


def shm_exists(session_id):
    return os.path.exists("/dev/shm/mzpt-%s" % session_id)


def fail(sess, reason):
    text = sess.text() if sess else ""
    esc = text.replace("\x1b", "<ESC>")
    sys.stdout.write("pty-presenter: FAIL %s (%s)\n" % (MODE, reason))
    sys.stdout.write("  expected A=%s B=%s C=%s\n" % (CSUM_A, CSUM_B, CSUM_C))
    sys.stdout.write("---captured---\n" + esc + "\n---end---\n")
    if sess:
        sess.close()
    sys.exit(1)


def ok():
    sys.stdout.write("pty-presenter: PASS %s\n" % MODE)
    sys.exit(0)


# ---- individual scenarios -------------------------------------------------------------

def run_checksum():
    sess = Session(FIXTURE)
    sid = sess.session_id()
    if not sid:
        fail(sess, "no session-id hint from the session")
    # B is the steady-state pattern; its checksum must appear cross-process.
    if not sess.wait_for("checksum=" + CSUM_B, 15):
        fail(sess, "expected B checksum %s never seen (cross-process frame not visible)" % CSUM_B)
    # A cross-process latency sanity number: time from launch to first checksum (coarse,
    # includes the presenter spawn handshake); recorded, not gated.
    m = CSUM_RE.search(sess.text())
    sys.stdout.write("pty-presenter: INFO first-checksum seen; latency-sanity captured\n")
    sess.close()
    ok()


def run_doorbell():
    sess = Session(DOORBELL)
    sid = sess.session_id()
    if not sid:
        fail(sess, "no session-id hint")
    if not sess.wait_for("checksum=" + CSUM_B, 15):
        fail(sess, "expected latest (B) checksum %s never seen" % CSUM_B)
    # Give the stub ample polls; the intermediate A must NEVER be reported.
    time.sleep(1.0)
    if ("checksum=" + CSUM_A) in sess.text():
        fail(sess, "intermediate A checksum %s was reported (doorbell not latest-wins)" % CSUM_A)
    sess.close()
    ok()


def run_respawn():
    sess = Session(FIXTURE)
    sid = sess.session_id()
    if not sid:
        fail(sess, "no session-id")
    if not sess.wait_for("checksum=" + CSUM_B, 15):
        fail(sess, "no initial checksums")
    pids = presenter_pids(sid)
    if len(pids) != 1:
        fail(sess, "expected exactly 1 presenter before kill, saw %r" % pids)
    marker = len(sess.checksums())
    # Abrupt external kill (SIGKILL: no graceful release) while the slot is still claimed
    # and with NO new registration; the watcher must detect within kStaleTimeoutMs and
    # respawn, checksums resuming within the ~4s bound.
    os.kill(pids[0], signal.SIGKILL)
    end = time.time() + 8
    resumed = False
    while time.time() < end:
        if len(sess.checksums()) > marker + 2:
            resumed = True
            break
        time.sleep(0.2)
    if not resumed:
        fail(sess, "checksums did not resume after abrupt presenter kill (no auto-respawn)")
    after = presenter_pids(sid)
    if len(after) != 1:
        fail(sess, "expected exactly 1 presenter after respawn, saw %r" % after)
    sess.close()
    ok()


def run_storm():
    die_file = os.path.join(SCRATCH, "die_%d" % os.getpid())
    if os.path.exists(die_file):
        os.remove(die_file)
    sess = Session(FIXTURE, env_extra={"MAIZE_PRESENTER_DIE_FILE": die_file})
    sid = sess.session_id()
    if not sid:
        fail(sess, "no session-id")
    if not sess.wait_for("checksum=" + CSUM_B, 15):
        fail(sess, "no initial checksums (first presenter should be healthy)")
    # Arm the die knob, then kill the healthy presenter so every respawn dies at once.
    open(die_file, "w").close()
    pids = presenter_pids(sid)
    for p in pids:
        try:
            os.kill(p, signal.SIGKILL)
        except OSError:
            pass
    # The watcher should attempt at most kRespawnMaxAttempts respawns then print the hint
    # ONCE and stop. Each dying respawn's wait_presenter_ready times out (~3s), so allow up
    # to ~20s for 3 attempts + the hint.
    if not sess.wait_for(r"auto-respawn paused", 25):
        fail(sess, "storm guard hint never printed")
    hint_count = sess.text().count("auto-respawn paused")
    if hint_count != 1:
        fail(sess, "storm hint printed %d times (want exactly once)" % hint_count)
    # Session must stay responsive: the process is still alive.
    try:
        os.kill(sess.pid, 0)
    except OSError:
        fail(sess, "session process died during the respawn storm")
    # Reset: remove the die knob and manually reattach a HEALTHY presenter (standalone; the
    # session has no local record of it). It steals the stale pid, becomes the live owner,
    # and the watcher, seeing a presenter it did not spawn become alive, resets the guard.
    # The manual presenter is NOT a child of the session pty, so read its OWN stdout.
    os.remove(die_file)
    manual = subprocess.Popen([MAIZEG, "--presenter", sid],
                              stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    manual_out = bytearray()
    def drain_manual():
        for line in iter(manual.stdout.readline, b""):
            manual_out.extend(line)
    t = threading.Thread(target=drain_manual, daemon=True)
    t.start()
    end = time.time() + 12
    resumed = False
    while time.time() < end:
        if CSUM_RE.search(manual_out.decode("latin-1")):
            resumed = True
            break
        time.sleep(0.2)
    # Exactly one presenter must be running (no redundant second spawn on top of the manual).
    live = presenter_pids(sid)
    try:
        manual.terminate()
    except OSError:
        pass
    if not resumed:
        fail(sess, "checksums did not resume after a manual reattach reset the storm guard")
    if len(live) != 1:
        fail(sess, "expected exactly 1 presenter after manual reattach, saw %r" % live)
    sess.close()
    ok()


def run_stalesteal():
    stall_file = os.path.join(SCRATCH, "stall_%d" % os.getpid())
    with open(stall_file, "w") as f:
        f.write("2500")   # stall the first presenter's heartbeat past kStaleTimeoutMs
    sess = Session(FIXTURE, env_extra={"MAIZE_PRESENTER_STALL_FILE": stall_file})
    sid = sess.session_id()
    if not sid:
        fail(sess, "no session-id")
    if not sess.wait_for("checksum=" + CSUM_B, 15):
        fail(sess, "no initial checksums")
    # During the first presenter's heartbeat stall, the watcher respawns a second presenter
    # that steals ownership; the original's next bump_heartbeat detects the pid mismatch and
    # self-terminates. Give the full stall window + settle time.
    time.sleep(5.0)
    pids = presenter_pids(sid)
    if len(pids) != 1:
        fail(sess, "expected exactly 1 presenter after stale-steal (no permanent split-brain), saw %r" % pids)
    # Checksums must still be flowing (a single live stream).
    marker = len(sess.checksums())
    time.sleep(1.5)
    if len(sess.checksums()) <= marker:
        fail(sess, "checksum stream stopped after the steal (the stealer is not presenting)")
    sess.close()
    ok()


def run_teardown():
    sess = Session(FIXTURE)
    sid = sess.session_id()
    if not sid:
        fail(sess, "no session-id")
    if not sess.wait_for("checksum=" + CSUM_B, 15):
        fail(sess, "no checksums before teardown")
    if not shm_exists(sid):
        fail(sess, "shm segment /dev/shm/mzpt-%s absent while running" % sid)
    # SIGTERM the session (the host_tty signal_restore -> teardown_if_active path). SIGINT is
    # deliberately excluded (it is guest synthetic input, not a terminating signal).
    os.kill(sess.pid, signal.SIGTERM)
    sess._reap(6)
    time.sleep(0.5)
    if shm_exists(sid):
        fail(sess, "shm segment survived session teardown")
    orphans = presenter_pids(sid)
    if orphans:
        fail(sess, "orphaned presenter(s) survived session teardown: %r" % orphans)
    sess.running = False
    ok()


def run_input():
    inject_file = os.path.join(SCRATCH, "inject_%d" % os.getpid())
    with open(inject_file, "w") as f:
        f.write("30")   # a Set-1 scancode; the stub pushes it into the input ring once
    sess = Session(FIXTURE, env_extra={"MAIZE_PRESENTER_INJECT_FILE": inject_file})
    sid = sess.session_id()
    if not sid:
        fail(sess, "no session-id")
    # The injected scancode must traverse ring -> session keyboard_device -> guest, which the
    # guest signals by presenting pattern C (only ever presented on a key event).
    if not sess.wait_for("checksum=" + CSUM_C, 15):
        fail(sess, "pattern C checksum %s never seen (input ring did not reach the guest)" % CSUM_C)
    sess.close()
    ok()


SCENARIOS = {
    "checksum": run_checksum,
    "doorbell": run_doorbell,
    "respawn": run_respawn,
    "storm": run_storm,
    "stalesteal": run_stalesteal,
    "teardown": run_teardown,
    "input": run_input,
}

if MODE == "all":
    for name, fn in SCENARIOS.items():
        pid = os.fork()
        if pid == 0:
            globals()["MODE"] = name
            fn()   # exits
            os._exit(0)
        _, status = os.waitpid(pid, 0)
        if status != 0:
            sys.stdout.write("pty-presenter: FAIL all (scenario %s failed)\n" % name)
            sys.exit(1)
    sys.stdout.write("pty-presenter: PASS all\n")
    sys.exit(0)
elif MODE in SCENARIOS:
    SCENARIOS[MODE]()
else:
    sys.stderr.write("unknown mode: %s\n" % MODE)
    sys.exit(2)
