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
#   (b) after the SAMPLE_TICKS stabilization window, the presenter stub's printed FNV-1a
#       checksum for DOOM's rendered 320x200 frame matches a pinned expected value, captured
#       empirically at Implement/Test time (mirroring doom_render_selfcheck.c's SAMPLE_TICKS=90
#       empirical-stability-pinning precedent).
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
EXPECTED_CSUM = os.environ.get("MZ_DOOM_CSUM", "f61da9a5")

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

    # (a) the launch-or-attach hook fired for the quesOS child's registration.
    m = sess.wait_for(SESSION_RE, 30)
    if not m:
        fail(sess, "session never announced 'maizeg --presenter <id>' "
                   "(quesOS child's fb_register did not trigger the launch hook)")
    sid = m.group(1)

    # (b) the stub's steady-state checksum for DOOM's 320x200 frame. Give it ample time: the
    # first graphical registration blocks the VM up to ~3s (kRegisterTimeoutMs) while the stub
    # spawns, then DOOM ticks the SAMPLE_TICKS stabilization window before the frame settles.
    seen = None
    end = time.time() + 40
    while time.time() < end:
        cs = sess.checksums()
        if cs:
            seen = cs[-1][2]   # latest reported checksum
            if EXPECTED_CSUM and any(c[2] == EXPECTED_CSUM for c in cs):
                sys.stdout.write("pty-presenter-doom: PASS (session=%s checksum=%s)\n"
                                 % (sid, EXPECTED_CSUM))
                sess.close()
                sys.exit(0)
        time.sleep(0.2)

    if not EXPECTED_CSUM:
        # Capture mode: print every distinct steady-state checksum for pinning, then FAIL so
        # the operator/implementer records the value in EXPECTED_CSUM.
        distinct = sorted({c[2] for c in sess.checksums()})
        sys.stdout.write("pty-presenter-doom: CAPTURE distinct-checksums=%r (last=%s)\n"
                         % (distinct, seen))
        fail(sess, "capture mode: pin one of the distinct checksums into EXPECTED_CSUM")

    fail(sess, "expected checksum %s never seen (last observed %s)" % (EXPECTED_CSUM, seen))


if __name__ == "__main__":
    main()
