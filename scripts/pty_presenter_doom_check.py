#!/usr/bin/env python3
# pty_presenter_doom_check.py (maize-251): the cross-process DOOM-under-quesOS checksum gate.
#
# Extends pty_presenter_check.py's Session-class harness (a console `maize` session under a
# REAL pty, so stdin_is_interactive() is true and the full maize-264 launch-or-attach machinery
# engages exactly as it would for the operator's own oksh session). Instead of that fixture's
# tiny synthetic 8x8 bare-VM pattern generator, it drives a full DOOM engine as a quesOS child:
#
#   maize --no-root --mount <progs>=/progs:ro --mount <wad>=/ro:ro <quesos.mzx> \
#         /progs/doom_render_selfcheck_quesos.mzx
#
# at the DEFAULT 320x200 framebuffer. The quesOS child's own sys_fb_register call (inside
# DG_Init) triggers the exact same launch-or-attach hook a bare-VM registration would -- the
# ONE thing genuinely specific to a quesOS-mediated registration that maize-264's bare-VM-only
# fixtures never exercised. It asserts:
#   (a) the session announces `maizeg --presenter <session-id>` (the launch-or-attach hook
#       fired for a quesOS child's registration);
#   (b) once DOOM's rendered frame has STABILIZED BY CONTENT (the presenter stub's printed
#       FNV-1a checksum holds identical across several consecutive DISTINCT presents, i.e. the
#       melt-wipe has finished and the frame is static), that stabilized checksum matches a
#       pinned expected value captured empirically at Implement/Test time (mirroring
#       doom_render_selfcheck.c's SAMPLE_TICKS=90 empirical-stability-pinning precedent).
#       Stabilization is detected by CONTENT, not by a wall-clock window: keying off "the
#       picture stopped changing" is invariant to per-tick execution speed. Paired with the
#       selfcheck child's deterministic clock (maize-251), linux-debug and linux-asan-ubsan
#       render byte-identical present streams and settle on the same pinned checksum.
#
# Doorbell/latest-frame semantics, respawn/stale-steal resilience, and teardown are already
# covered generically by maize-264's own fixtures (ACs 9410/9411/9441/9487) and are deliberately
# NOT re-proven DOOM-specifically here.
#
# CI-safe: stdlib pty on a Linux runner ONLY; the Windows leg rides AC 9308 (operator live check).
#
# Usage: pty_presenter_doom_check.py <maize> <quesos.mzx> <doom_child.mzx> <progs-dir> <wad-dir>
# The pinned expected checksum lives in EXPECTED_CSUM below (env MZ_DOOM_CSUM overrides, used to
# CAPTURE the value on first run: set it empty to print the observed steady-state checksum).
import os, pty, select, sys, time, re, signal, threading

if len(sys.argv) < 6:
    sys.stderr.write("usage: pty_presenter_doom_check.py <maize> <quesos.mzx> "
                     "<doom_child.mzx> <progs-dir> <wad-dir>\n")
    sys.exit(2)

MAIZE, QUESOS, DOOM_CHILD, PROGS, WADDIR = sys.argv[1:6]

# The pinned steady-state FNV-1a checksum of DOOM's 320x200 frame (empirically captured; see
# the file header). An env override lets the harness capture the value on a fresh render.
#
# maize-251 re-pin: was f61da9a5, captured under the OLD wall-clock render where the settled
# frame depended on how many real milliseconds a fixed tick count happened to span (so it
# differed per build config; f61da9a5 was the linux-debug value and was never reachable under
# linux-asan-ubsan). The selfcheck child now runs a deterministic tick-derived clock
# (DG_MaizeDeterministicClock, doomgeneric_maize.c), so a fixed tick count always produces the
# same animation state. The settled checksum re-derived from that deterministic render is
# fae9e8a5, captured identically on linux-debug AND linux-asan-ubsan (held 72 consecutive
# presents on each, 3x per leg). The pin moved because the clock became deterministic, not
# because the gate was relaxed: the assertion is unchanged (the stabilized frame must EQUAL
# this pin).
EXPECTED_CSUM = os.environ.get("MZ_DOOM_CSUM", "fae9e8a5")

# Content-based stabilization gate (maize-251). The presenter stub prints one line per DISTINCT
# present (it emits only when present_sequence advances; see src/presenter_main.cpp), so the
# checksum list is the ordered stream of rendered frames. DOOM's melt-wipe changes the frame every
# tick; only the settled post-wipe frame repeats. We require the pinned checksum to HOLD across
# STABLE_FRAMES consecutive distinct presents before accepting it, with a generous overall
# STABLE_DEADLINE cap (with the deterministic clock the render settles quickly on both legs; the
# cap only bites on a genuine failure).
STABLE_FRAMES = int(os.environ.get("MZ_DOOM_STABLE_FRAMES", "5"))
STABLE_DEADLINE = float(os.environ.get("MZ_DOOM_STABLE_DEADLINE", "150"))

CSUM_RE = re.compile(r"presenter-stub: slot=(\d+) seq=(\d+) checksum=([0-9a-f]{8}) t=(\d+)")
SESSION_RE = re.compile(r"maizeg --presenter (\w+)")


class Session:
    """A `maize` session running under a pty, with a background reader draining the master.
    Mirrors pty_presenter_check.py's Session, but launches a quesOS worklist child with the
    /progs + /ro mounts DOOM needs, at the default (320x200) framebuffer."""
    def __init__(self, argv):
        self.captured = bytearray()
        self.lock = threading.Lock()
        env = dict(os.environ)
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


def fail(sess, reason):
    text = sess.text() if sess else ""
    esc = text.replace("\x1b", "<ESC>")
    sys.stdout.write("pty-presenter-doom: FAIL (%s)\n" % reason)
    sys.stdout.write("  expected checksum=%s\n" % (EXPECTED_CSUM or "<capture-mode>"))
    sys.stdout.write("  observed checksums=%r\n" % (sess.checksums() if sess else []))
    sys.stdout.write("---captured---\n" + esc + "\n---end---\n")
    if sess:
        sess.close()
    sys.exit(1)


def main():
    argv = [MAIZE, "--no-root",
            "--mount", "%s=/progs:ro" % PROGS,
            "--mount", "%s=/ro:ro" % WADDIR,
            QUESOS, "/progs/%s" % os.path.basename(DOOM_CHILD)]
    sess = Session(argv)

    # (a) the launch-or-attach hook fired for the quesOS child's registration. fb_register fires
    # early in DG_Init, well before the tick loop, but give it ample margin for the slow
    # asan/ubsan leg's engine boot (returns as soon as it matches, so the cap only bites on a
    # genuine no-announcement failure).
    m = sess.wait_for(SESSION_RE, 60)
    if not m:
        fail(sess, "session never announced 'maizeg --presenter <id>' "
                   "(quesOS child's fb_register did not trigger the launch hook)")
    sid = m.group(1)

    # (b) the stub's CONTENT-STABILIZED checksum for DOOM's 320x200 frame. The first graphical
    # registration blocks the VM up to ~3s (kRegisterTimeoutMs) while the stub spawns, then DOOM
    # ticks its fixed SAMPLE_TICKS count; the melt-wipe changes the frame every tick and only the
    # settled post-wipe frame holds steady. Rather than sampling after a fixed wall-clock delay
    # (which, under ASan/UBSan's slower per-tick execution, could catch a still-animating frame),
    # poll the present stream and wait for the pinned checksum to HOLD across STABLE_FRAMES
    # consecutive distinct presents. This cannot false-positive on an in-flight frame for two
    # independent reasons: we accept only a run whose checksum EQUALS the pin, AND we require the
    # match to persist across STABLE_FRAMES distinct presents. With the selfcheck's deterministic
    # clock both legs produce the identical present stream, so this is robust by construction.
    def trailing_run(cs):
        """(length, checksum) of the trailing run of distinct presents sharing the last one's
        checksum. Each entry is a distinct present (the stub prints one line per present_sequence
        advance), so a run of identical checksums here means identical CONSECUTIVE frames, not a
        re-read of a single frame while the render is merely slow to advance."""
        if not cs:
            return 0, None
        last = cs[-1][2]
        n = 0
        for c in reversed(cs):
            if c[2] != last:
                break
            n += 1
        return n, last

    seen = None
    settled = None
    end = time.time() + STABLE_DEADLINE
    while time.time() < end:
        cs = sess.checksums()
        if cs:
            seen = cs[-1][2]   # latest reported checksum
            run, held = trailing_run(cs)
            if run >= STABLE_FRAMES:
                settled = held
                if EXPECTED_CSUM and held == EXPECTED_CSUM:
                    sys.stdout.write("pty-presenter-doom: PASS (session=%s checksum=%s "
                                     "stable-run=%d)\n" % (sid, EXPECTED_CSUM, run))
                    sess.close()
                    sys.exit(0)
        time.sleep(0.2)

    if not EXPECTED_CSUM:
        # Capture mode: print the content-stabilized checksum (plus every distinct one seen) for
        # pinning, then FAIL so the operator/implementer records the value in EXPECTED_CSUM.
        distinct = sorted({c[2] for c in sess.checksums()})
        sys.stdout.write("pty-presenter-doom: CAPTURE stabilized=%s distinct-checksums=%r "
                         "(last=%s)\n" % (settled, distinct, seen))
        fail(sess, "capture mode: pin the stabilized checksum into EXPECTED_CSUM")

    if settled is not None and settled != EXPECTED_CSUM:
        fail(sess, "frame stabilized on %s across >=%d consecutive presents but expected %s "
                   "(last observed %s)" % (settled, STABLE_FRAMES, EXPECTED_CSUM, seen))

    fail(sess, "expected checksum %s never stabilized within %.0fs (last observed %s)"
               % (EXPECTED_CSUM, STABLE_DEADLINE, seen))


if __name__ == "__main__":
    main()
