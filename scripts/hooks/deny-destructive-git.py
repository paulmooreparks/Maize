#!/usr/bin/env python3
"""PreToolUse guard: deny destructive git commands outside .claude/worktrees/.

Motivation (maize-432): worktree-isolated subagents three times ran git state
mutations (two detach-and-commit passes and a path-scoped `git checkout -q
origin/card/maize-431 -- .`) whose Bash cwd was the MAIN checkout, not their
worktree, and the harness worktree guard did not fire. All three happened to
be benign; this hook makes the failure mode deterministic instead of lucky.

Ported from Andoneer's scripts/hooks/deny-destructive-git.py (committed
2026-07-31) rather than redesigned, so both boards stay on one guard shape
until andon-735 rules on whether to replace it (maize-432 D-1).

Reads the Claude Code hook payload on stdin. Denies when the command
matches a destructive git pattern AND the effective target is not clearly
a worktree. Fail-closed: missing cwd counts as the main checkout.

Deny set (deliberately narrow):
  - git reset --hard / --merge / --keep
  - git clean with -f/-d/-x style flags
  - git checkout -- <paths> (working-tree discard) and git checkout -f
  - git restore (working-tree discard without --staged-only usage)
  - git push --force / -f, and --force-with-lease when the refspec
    mentions main or master

Allow: anything else, and any denied pattern whose cwd or -C target sits
under .claude/worktrees/.
"""

import json
import re
import sys


def norm(p):
    return (p or "").replace("\\", "/").lower()


def in_worktree(path):
    return "/.claude/worktrees/" in norm(path) or norm(path).endswith("/.claude/worktrees")


# A newline separates commands exactly as `;` does, so it belongs in every
# negated class below. Without it a script whose first line runs `git push`
# and whose fifth line runs an unrelated `git worktree remove --force` reads
# as one invocation and is denied, which is a false positive that teaches
# agents to work around the guard rather than respect it. Observed
# 2026-07-31. Genuine multi-line invocations use a backslash continuation,
# and join_continuations folds those back before this runs, so nothing real
# escapes by spanning lines.
DESTRUCTIVE = [
    (re.compile(r"\bgit\b[^|;&\n]*\breset\b[^|;&\n]*(--hard|--merge|--keep)\b"), "git reset --hard/--merge/--keep"),
    (re.compile(r"\bgit\b[^|;&\n]*\bclean\b[^|;&\n]*\s-[a-z]*[fdx]"), "git clean -f/-d/-x"),
    (re.compile(r"\bgit\b[^|;&\n]*\bcheckout\b[^|;&\n]*(\s--\s|\s-f\b|\s--force\b)"), "git checkout -- / -f"),
    (re.compile(r"\bgit\b[^|;&\n]*\brestore\b(?![^|;&\n]*--staged\b(?![^|;&\n]*--worktree\b))"), "git restore (working tree)"),
    (re.compile(r"\bgit\b[^|;&\n]*\bpush\b[^|;&\n]*(\s--force(?!-with-lease)\b|\s-f\b)"), "git push --force"),
    (re.compile(r"\bgit\b[^|;&\n]*\bpush\b[^|;&\n]*--force-with-lease[^|;&\n]*\b(main|master)\b"), "git push --force-with-lease to main/master"),
]

CONTINUATION = re.compile(r"\\[ \t]*\r?\n")

C_FLAG = re.compile(r"\bgit\s+-C\s+(\"[^\"]+\"|'[^']+'|\S+)")

QUOTED = re.compile(r"\"[^\"]*\"|'[^']*'")


def join_continuations(command):
    """Fold shell line-continuations back onto one line.

    The destructive patterns stop at a newline so that two commands in one
    script are not read as one. A backslash continuation is the single case
    where one real invocation legitimately spans lines, so it is rejoined
    first and stays detectable: `git reset \\` on one line followed by
    `--hard` on the next is still `git reset --hard`."""
    return CONTINUATION.sub(" ", command)


def strip_quoted(command):
    """Remove quoted spans so prose (commit messages, heredoc text, echo
    strings) cannot false-positive the destructive patterns. Real
    destructive invocations carry their flags outside quotes. An unmatched
    trailing quote leaves the remainder in place, which errs toward
    matching (fail-closed)."""
    return QUOTED.sub(" ", command)


def main():
    try:
        payload = json.load(sys.stdin)
    except Exception:
        return  # unparseable payload: stay out of the way

    tool_input = payload.get("tool_input") or {}
    command = tool_input.get("command") or ""
    if "git" not in command:
        return

    scannable = strip_quoted(join_continuations(command))
    matched = None
    for pattern, label in DESTRUCTIVE:
        if pattern.search(scannable):
            matched = label
            break
    if not matched:
        return

    # Where will this run? Explicit -C wins; otherwise the session cwd.
    c_targets = [m.group(1).strip("\"'") for m in C_FLAG.finditer(command)]
    cwd = payload.get("cwd") or ""
    if c_targets:
        if all(in_worktree(t) for t in c_targets):
            return  # explicitly worktree-scoped
    elif in_worktree(cwd):
        return  # running inside an isolation worktree

    where = c_targets[0] if c_targets else (cwd or "<cwd not reported; failing closed>")
    reason = (
        "Blocked destructive git ({0}) outside .claude/worktrees/ (target: {1}). "
        "Worktree-isolated agents must run this inside their own worktree "
        "(pwd-check first; use git -C <worktree-path> ...). In the main checkout this "
        "class of command is operator-only: ask the operator to run it, or the operator "
        "can disable this guard via /hooks (.claude/settings.json, maize-432)."
    ).format(matched, where)

    print(json.dumps({
        "hookSpecificOutput": {
            "hookEventName": "PreToolUse",
            "permissionDecision": "deny",
            "permissionDecisionReason": reason,
        }
    }))


if __name__ == "__main__":
    main()
