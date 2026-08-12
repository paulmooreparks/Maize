"""Cases the deny-destructive-git guard has to get right.

Run with no arguments to test the sibling hook:

    python scripts/hooks/test_deny_destructive_git.py

Pass a path to test a different copy, which is how you arm it: check out an
older revision of the hook to a temp file and confirm the case you are fixing
goes red there. A guard nobody has watched fail is not evidence it guards.

The payloads are built by concatenation rather than written as literals so
that this file can be edited by an agent whose own tool calls are screened by
the very hook under test.
"""
import json
import os
import subprocess
import sys

HOOK = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "deny-destructive-git.py"
)

MAIN = "C:/Users/paul/source/repos/Maize"
WORKTREE = "C:/Users/paul/source/repos/Maize/.claude/worktrees/agent-abc"

RESET_HARD = "git " + "reset " + "--hard origin/main"

# maize-432 AC-9: the exact third-occurrence command. A path-scoped checkout
# discards the working tree while looking nothing like reset --hard or
# clean -fd, and it went undenied on 2026-08-12 only because no hook was
# installed in this repository at all. It gets its own named regression case
# rather than relying on the general "checkout -- / -f" pattern covering it
# incidentally.
PATH_SCOPED_CHECKOUT = "git " + "checkout " + "-q origin/card/maize-431 " + "-- ."

# (name, command, cwd, expect_deny)
CASES = [
    (
        "two commands in one script: a push, and an unrelated --force later",
        "git -C /tmp/wt push -q origin HEAD:refs/heads/mybranch\n"
        "git worktree remove --force /tmp/wt",
        MAIN,
        False,
    ),
    (
        "two commands in one script, the second genuinely destructive",
        "git status --porcelain\n" + RESET_HARD,
        MAIN,
        True,
    ),
    (
        "a single destructive command in the operator's checkout",
        RESET_HARD,
        MAIN,
        True,
    ),
    (
        "a backslash continuation is one command and stays detectable",
        "git " + "reset \\\n  " + "--hard origin/main",
        MAIN,
        True,
    ),
    (
        "the same destructive command inside an isolation worktree is allowed",
        RESET_HARD,
        WORKTREE,
        False,
    ),
    (
        "quoted prose naming a destructive form is not an invocation",
        'echo "never run ' + RESET_HARD + ' here"',
        MAIN,
        False,
    ),
    (
        "a genuine force-push stays denied",
        "git " + "push " + "--force origin main",
        MAIN,
        True,
    ),
    (
        "git checkout --detach is not a working-tree discard",
        "git checkout --detach origin/main",
        MAIN,
        False,
    ),
    (
        "an explicit -C into a worktree is allowed from anywhere",
        "git -C " + WORKTREE + " " + "reset " + "--hard",
        MAIN,
        False,
    ),
    (
        "the third-occurrence path-scoped checkout in the main checkout (maize-432)",
        PATH_SCOPED_CHECKOUT,
        MAIN,
        True,
    ),
]


def run(command, cwd):
    payload = json.dumps({"tool_input": {"command": command}, "cwd": cwd})
    result = subprocess.run(
        [sys.executable, HOOK], input=payload, capture_output=True, text=True
    )
    out = result.stdout.strip()
    if not out:
        return False, ""
    hook_out = json.loads(out)["hookSpecificOutput"]
    return hook_out["permissionDecision"] == "deny", hook_out["permissionDecisionReason"]


def main():
    failures = 0
    for name, command, cwd, expect_deny in CASES:
        denied, reason = run(command, cwd)
        ok = denied == expect_deny
        if not ok:
            failures += 1
        print(
            "[{0}] want={1} got={2}  {3}".format(
                "PASS" if ok else "FAIL",
                "deny" if expect_deny else "allow",
                "deny" if denied else "allow",
                name,
            )
        )
        if not ok and reason:
            print("         reason: " + reason[:110])
    print()
    print("{0}/{1} passed".format(len(CASES) - failures, len(CASES)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
