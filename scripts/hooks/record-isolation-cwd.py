#!/usr/bin/env python3
"""PreToolUse recorder: capture where a shell call actually ran (maize-432).

This is evidence capture, not a guard. It emits nothing on stdout, carries no
permissionDecision, and cannot deny anything. Every path is wrapped so that an
unparseable payload, an empty stdin, or an unwritable log directory exits 0
with empty stdout. A recorder that blocks a session is worse than a gap in a
log, which is the exact opposite of the posture its sibling
deny-destructive-git.py takes, and maize-432 D-6 is why they stay in separate
files: one broad try/except in a shared script would quietly turn the guard
advisory.

What it is for. A worktree-isolated subagent has three times run with its
shell working directory pointing at the operator's main checkout, and every
detection to date was an accident: somebody happened to read a reflog, or
happened to notice a push failing non-fast-forward. The in-session self-check
in the shell-granting agent definitions is the detector. This file is the
measurement, which is a different job. It exists so that a fourth occurrence
is measurable rather than accidental, and so that the question "how often does
this actually happen on this board?" has an answer drawn from records instead
of anecdotes. Andoneer's own copy of this log turned two anecdotes into a
known rate of roughly seven percent (4,935 dispatches inside a worktree
against 363 outside).

One caveat on attribution, inherited from the port and worth keeping: the
PreToolUse payload carries the PARENT session_id for a subagent, so a record
showing cwd_in_worktree false cannot on its own be attributed to a subagent
rather than to the orchestrator legitimately working in the main checkout.
What might attribute it is the hook process's own environment, so this file
keeps the value of CLAUDE_CODE_CHILD_SESSION and derives is_child_session
from it.

One property worth naming, because maize-432 D-8 leans on it: the hook command
in .claude/settings.json is cwd-relative, so a hook process launched with a
working directory in neither tree silently does not run. A dispatch that
produces zero records is therefore itself the signal that no hook process
launched. That signal is passive; nothing here alerts on it.

How to ask the log a question, written down here because nobody will invent it
under pressure during a fourth occurrence:

    python -c "
    import json
    total = in_wt = out_wt = 0
    with open('.claude/isolation-audit.jsonl') as f:
        for line in f:
            rec = json.loads(line)
            total += 1
            if rec.get('cwd_in_worktree') is True: in_wt += 1
            elif rec.get('cwd_in_worktree') is False: out_wt += 1
    print(f'{total} records, {out_wt} outside a worktree, {in_wt} inside, {total-in_wt-out_wt} unresolved')
    "
"""

import datetime
import json
import os
import sys

# Values kept verbatim. Everything else CLAUDE_-prefixed is recorded as a JSON
# null, meaning the name was present and its value was not kept, which stays
# distinguishable from the name being absent entirely.
#
# The DIR/PATH suffix rule below was the whole filter in andon-730's first
# spec cycle, and on its own it discarded the value of the one variable that
# can attribute a record to a child session.
IDENTITY_BEARING = frozenset((
    "CLAUDECODE",
    "CLAUDE_CODE_CHILD_SESSION",
    "CLAUDE_CODE_ENTRYPOINT",
    "CLAUDE_CODE_SESSION_ID",
    "CLAUDE_PID",
))

CHILD_SESSION_VAR = "CLAUDE_CODE_CHILD_SESSION"

WORKTREE_MARKER = "/.claude/worktrees/"

LOG_NAME = "isolation-audit.jsonl"

# Two files, bounded. Past this size the live log is renamed over any existing
# .1 and a fresh one starts.
MAX_LOG_BYTES = 8 * 1024 * 1024

# A payload value longer than this, or one that is not a scalar, is replaced by
# its type name so the log does not become a transcript.
MAX_VALUE_CHARS = 512

# The command is excluded from the payload block and truncated to this many
# characters into command_head instead, for the same reason.
MAX_COMMAND_CHARS = 200


def load_in_worktree():
    """Return the deny hook's own in_worktree predicate, or None.

    The two hooks agree on what counts as a worktree by construction rather
    than by two copies of one rule staying in step. The sibling is imported
    for its predicate; its main() is guarded by __name__ == "__main__", so
    importing it runs no policy. When the import fails the record still gets
    written, with cwd_in_worktree null and a degraded field naming why, which
    is better than a second copy of the predicate silently drifting from the
    one the guard enforces.
    """
    import importlib.util

    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "deny-destructive-git.py")
    spec = importlib.util.spec_from_file_location("andon_deny_destructive_git", path)
    if spec is None or spec.loader is None:
        return None
    module = importlib.util.module_from_spec(spec)
    # This hook runs on EVERY Bash and PowerShell call, and an import writes a
    # __pycache__ directory beside the source by default. That would mean a
    # repository silently grows a directory nobody asked for, inside a tree the
    # port instructions tell you to copy. Nothing here is hot enough for the
    # cached bytecode to be worth that.
    previous = sys.dont_write_bytecode
    sys.dont_write_bytecode = True
    try:
        spec.loader.exec_module(module)
    finally:
        sys.dont_write_bytecode = previous
    return getattr(module, "in_worktree", None)


def slashes(path):
    """Normalize separators without touching case, so a recorded path stays
    readable while the marker search below can be case-insensitive."""
    return (path or "").replace("\\", "/")


def main_checkout_of(cwd):
    """The tree a worktree belongs to: cwd truncated at the worktree marker.

    Derived from the payload rather than written relative, because a relative
    .claude/... resolves inside whichever tree the process happens to be in and
    that is the variable under investigation. A cwd with no marker in it is
    already a main checkout and is returned unchanged.
    """
    normalized = slashes(cwd)
    idx = normalized.lower().find(WORKTREE_MARKER)
    if idx < 0:
        return normalized
    return normalized[:idx]


def scalar(value):
    """One payload value, reduced to something safe to log.

    Non-scalars and over-long strings become their type name, which records
    that the key was present and carried something without copying it.
    """
    if value is None or isinstance(value, bool) or isinstance(value, (int, float)):
        return value
    if isinstance(value, str):
        return value if len(value) <= MAX_VALUE_CHARS else type(value).__name__
    return type(value).__name__


def env_claude():
    """Every CLAUDE_-prefixed name plus CLAUDECODE, values kept selectively."""
    out = {}
    for name, value in os.environ.items():
        upper = name.upper()
        if upper != "CLAUDECODE" and not upper.startswith("CLAUDE_"):
            continue
        keep = upper in IDENTITY_BEARING or upper.endswith("DIR") or upper.endswith("PATH")
        out[name] = value if keep else None
    return out


def rotate(log_path):
    try:
        if os.path.getsize(log_path) <= MAX_LOG_BYTES:
            return
    except OSError:
        return
    os.replace(log_path, log_path + ".1")


def build_record(payload, in_worktree):
    tool_input = payload.get("tool_input")
    command = ""
    if isinstance(tool_input, dict):
        command = tool_input.get("command") or ""
        if not isinstance(command, str):
            command = ""

    cwd = slashes(payload.get("cwd") or "")
    record = {
        "ts": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "payload_keys": sorted(payload.keys()),
        "payload": {k: scalar(v) for k, v in payload.items()},
        "env_claude": env_claude(),
        "is_child_session": bool(os.environ.get(CHILD_SESSION_VAR)),
        "cwd": cwd,
        "cwd_in_worktree": in_worktree(cwd) if in_worktree else None,
        "main_checkout": main_checkout_of(cwd),
        "command_head": command[:MAX_COMMAND_CHARS],
    }
    if in_worktree is None:
        record["degraded"] = "deny-destructive-git.py could not be imported; cwd_in_worktree not computed"
    return record


def run():
    raw = sys.stdin.read()
    if not raw.strip():
        return
    payload = json.loads(raw)
    if not isinstance(payload, dict):
        return

    try:
        in_worktree = load_in_worktree()
    except Exception:
        in_worktree = None

    record = build_record(payload, in_worktree)

    log_dir = os.path.join(record["main_checkout"], ".claude")
    log_path = os.path.join(log_dir, LOG_NAME)
    os.makedirs(log_dir, exist_ok=True)
    rotate(log_path)
    with open(log_path, "a", encoding="utf-8") as fh:
        fh.write(json.dumps(record) + "\n")


def main():
    try:
        run()
    except Exception:
        # Fail open, always. This hook has no opinion worth a blocked session.
        pass


if __name__ == "__main__":
    main()
