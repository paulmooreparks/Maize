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
# maize-268 adds four exit-path modes on the same harness. They assert the HOST EXIT STATUS
# and the presence or absence of the post-run "console build cannot display" diagnostic
# through main's real exit path, which needs the pty for the same reason the modes above do:
# the whole diagnostic block is armed only when stdin is a terminal, so a redirected CI run
# never reaches it. Their two extra fixtures are resolved as siblings of <fixture.mzb>, so
# the positional argument list below is unchanged.
#
# Usage: pty_presenter_check.py <maize> <maizeg> <fixture.mzb> <doorbell.mzb> <scratch-dir> <mode>
#   modes: checksum | doorbell | respawn | storm | stalesteal | teardown | input
#          | cleanexit | nodisplay_stop | nodisplay_reject | plainexit | all
# Exit 0 on PASS (prints "pty-presenter: PASS <mode>"), 1 on failure (with a diagnostic
# and the captured transcript).
import os, pty, select, sys, time, re, signal, subprocess, threading

if len(sys.argv) < 7:
    sys.stderr.write("usage: pty_presenter_check.py <maize> <maizeg> <fixture.mzb> "
                     "<doorbell.mzb> <scratch-dir> <mode>\n")
    sys.exit(2)

MAIZE, MAIZEG, FIXTURE, DOORBELL, SCRATCH, MODE = sys.argv[1:7]
os.makedirs(SCRATCH, exist_ok=True)

# maize-268: the two exit-path fixtures live beside the ones named on the command line, so
# the caller's argument list stays as it is. CLEANEXIT claims the framebuffer, presents, and
# exits 42; EXITSTATUS exits 42 without ever touching a framebuffer port.
CLEANEXIT = os.path.join(os.path.dirname(FIXTURE), "test_presenter_cleanexit.mzb")
EXITSTATUS = os.path.join(os.path.dirname(FIXTURE), "test_exit_status.mzb")

# The two lines of the maize-221 console diagnostic, matched literally. Their wording is
# part of the contract these modes assert, in both directions: nodisplay_stop and
# nodisplay_reject require them, cleanexit and plainexit forbid them.
DIAG_LINE_1 = "console build cannot display"
DIAG_LINE_2 = "run it with the graphical Maize binary"
GUEST_STATUS = 42        # what both exit-path fixtures pass to SYS $3C
NO_RESULT_STATUS = 3     # what main reports when the claim stopped the VM before it could

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
# Detect the session via the stub's own ready line (src/presenter_main.cpp, printed right after
# mark_presenter_ready), not maize's "run `maizeg --presenter <id>` to reattach" console notice:
# that notice is behind --verbose as of maize-371, whereas the stub ready line is unconditional
# and always precedes the checksum stream these scenarios wait on. The literal "maizeg --presenter
# <id>" used by presenter_pids() / the manual reattach below is the real process argv and is
# unaffected.
SESSION_RE = re.compile(r"presenter-stub: ready session=(\w+)")


class Session:
    """A `maize` session running under a pty, with a background reader draining the master."""
    def __init__(self, program, env_extra=None, extra_args=None):
        self.captured = bytearray()
        self.lock = threading.Lock()
        env = dict(os.environ)
        if env_extra:
            env.update(env_extra)
        # maize-268: extra_args lands between --resolution and the program path, which keeps
        # the argv shape the seven original modes are proven against when it is empty.
        argv = [MAIZE, "--resolution", "%dx%d" % (FB_W, FB_H)] + list(extra_args or []) + [program]
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

    def wait_exit(self, seconds):
        """maize-268: reap the session and return its host exit status, or None.

        Returns None when the session is still alive after `seconds` or when it died on a
        signal rather than exiting. This MUST run before close(): closing the pty master can
        SIGHUP the child, which would destroy the very status under test. It reaps the child
        itself, so the os.kill inside a later close() raises ESRCH and close() treats that as
        already-gone, which is the intended path and not an error.
        """
        end = time.time() + seconds
        while time.time() < end:
            try:
                w, status = os.waitpid(self.pid, os.WNOHANG)
            except OSError:
                return None
            if w == self.pid:
                return os.WEXITSTATUS(status) if os.WIFEXITED(status) else None
            time.sleep(0.05)
        return None

    def drain(self, seconds=5):
        """maize-268: wait for the reader thread to finish before the transcript is read.

        With the child reaped, the master read raises EIO and the loop ends on its own, so
        this joins rather than interrupts. close() sets running = False, which STOPS the
        reader mid-flight and can truncate the tail of the transcript. The exit-path
        diagnostic is the last thing a session ever prints, so a truncated transcript would
        satisfy an absence assertion for entirely the wrong reason.
        """
        self.reader.join(seconds)

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


# ---- maize-268 exit-path scenarios ------------------------------------------------------
# Each one runs a fixture that exits through SYS $3C with status 42 and then checks two
# things about main's exit path: the host status the session reported, and whether the
# maize-221 console diagnostic printed. The four cases differ only in which flags the
# session ran with, which is what selects the presenter posture under test.

def exit_case(what, program, extra_args, want_status, want_diag, need_presenter):
    """Run one session to completion and assert its exit status and diagnostic."""
    sess = Session(program, extra_args=extra_args)
    if need_presenter and not sess.session_id():
        fail(sess, "%s: the stub presenter never printed its ready line, so a failed spawn "
                   "would masquerade as the exit-status behavior under test" % what)
    status = sess.wait_exit(30)
    sess.drain()          # complete transcript first; the diagnostic is the last output
    text = sess.text()
    if status is None:
        fail(sess, "%s: the session did not exit normally within 30s (still running, or "
                   "killed by a signal)" % what)
    if status != want_status:
        fail(sess, "%s: host exit status %d, want %d" % (what, status, want_status))
    if want_diag:
        for needle in (DIAG_LINE_1, DIAG_LINE_2):
            if needle not in text:
                fail(sess, "%s: the console diagnostic is missing its %r line, and this run "
                           "must still print it" % (what, needle))
    elif DIAG_LINE_1 in text:
        fail(sess, "%s: the %r diagnostic printed on a run that must not produce it"
                   % (what, DIAG_LINE_1))
    sess.close()


def run_cleanexit():
    # The headline case (maize-268): an interactive session that bound a presenter, ran a
    # graphical guest through it, and exited cleanly reports the GUEST's status and says
    # nothing about console builds. Before the fix this exits 3 with the diagnostic, because
    # the teardown nulled the handle the guard was reading.
    exit_case("cleanexit", CLEANEXIT, [], GUEST_STATUS, False, True)
    ok()


def run_nodisplay_stop():
    # The diagnostic is legitimate here and must survive: no presenter was ever bound, the
    # guest claimed the framebuffer anyway, and --fb-stop-on-claim stopped the VM at that
    # claim. The guest never reached SYS $3C, so there is no guest status to report and 3 is
    # the signal. A fix that deleted, weakened, or flag-gated the message fails this mode.
    exit_case("nodisplay_stop", CLEANEXIT, ["--fb-no-display", "--fb-stop-on-claim"],
              NO_RESULT_STATUS, True, False)
    ok()


def run_nodisplay_reject():
    # No presenter, claim attempted, but the claim was only rejected per-exec (maize-236):
    # the guest handled -ENODEV and ran on to its own SYS $3C. The message is still true, so
    # it still prints; the status is the guest's, because the guest produced one. This is
    # also what makes the tty and non-tty runs of the same command agree.
    exit_case("nodisplay_reject", CLEANEXIT, ["--fb-no-display"], GUEST_STATUS, True, False)
    ok()


def run_plainexit():
    # A guest that never touches a framebuffer port. The first run has a presenter segment
    # bound (no --fb-no-display on an interactive session), so it pins maize-58 exit-status
    # pass-through on a tty. The second run has none, which leaves graphics_claim_attempted()
    # as the only term keeping the diagnostic quiet, so it is the run that actually pins that
    # term (maize-268 open question 10814).
    exit_case("plainexit", EXITSTATUS, [], GUEST_STATUS, False, False)
    exit_case("plainexit (no presenter bound)", EXITSTATUS, ["--fb-no-display"],
              GUEST_STATUS, False, False)
    ok()


SCENARIOS = {
    "checksum": run_checksum,
    "doorbell": run_doorbell,
    "respawn": run_respawn,
    "storm": run_storm,
    "stalesteal": run_stalesteal,
    "teardown": run_teardown,
    "input": run_input,
    "cleanexit": run_cleanexit,
    "nodisplay_stop": run_nodisplay_stop,
    "nodisplay_reject": run_nodisplay_reject,
    "plainexit": run_plainexit,
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
