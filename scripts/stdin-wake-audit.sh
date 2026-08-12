#!/bin/sh
# maize-313: the structural audits, which are the half of this card's evidence that no
# functional test can produce.
#
# Three of the criteria here are about WHERE code sits rather than what it does, and each of
# them exists because a previous spec cycle got the placement wrong in a way every functional
# test passed anyway:
#
#   - the source's call sites were once written inside #ifdef MAIZE_CONSOLE_ONLY, and
#     CMakeLists.txt defines that macro on the `maize` target alone, so a headless maizeg run
#     compiled neither the mechanism nor the degradation that stands in for it;
#   - the instruction-tick pump was once retired, which reopened two holes that need two
#     parked readers and two closely spaced bytes to show themselves;
#   - the Windows source could drift back toward consuming fd 0, which is the one way this
#     card could reintroduce the undocumented-behaviour dependency it was written to remove.
#
# So these are greps by design. A later edit that moves a call site back inside the block is
# exactly the change this card was pushed back for, and it is invisible to every other test.
#
# Usage: scripts/stdin-wake-audit.sh [--against <ref>]

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "${SCRIPT_DIR}/.." && pwd)
. "${SCRIPT_DIR}/lib/harness-env.sh"
AGAINST='90b18b8'

while [ $# -gt 0 ]; do
    case "$1" in
        --against) AGAINST="$2"; shift 2 ;;
        *) echo "stdin-wake-audit: unknown argument '$1'" >&2; exit 2 ;;
    esac
done

FAILED=0
pass() { echo "[PASS] $*"; }
fail() { echo "[FAIL] $*"; FAILED=$((FAILED + 1)); }

# have_evidence <what> <path>...
#   Guard every check whose evidence is a named file. Returns 0 when all of them are
#   readable; otherwise records an INCONCLUSIVE failure naming the first that is not, and
#   returns 1 so the caller skips the check entirely rather than running a matcher on a
#   file that is not there.
#
# Every audit in this file is a grep or an awk over a source file, and grep's "I could not
# read it" is exit 2 while its "the pattern is absent" is exit 1. An `if grep ...` reads
# both as the same branch, so a renamed or deleted file scored as a clean audit at three
# sites in here, and a pipeline made it worse by discarding the 2 outright. The two
# questions are separated here on purpose: this function answers whether there is anything
# to audit, and the matcher below it answers what the file says.
#
# INCONCLUSIVE counts as a failure rather than passing quietly, matching the AC-21 git leg
# below. An audit that did not run has established nothing, and the run's own exit status
# is what a caller reads.
have_evidence() {
    _he_what="$1"; shift
    if _he_bad=$(maize_require_file "$@"); then
        return 0
    fi
    fail "INCONCLUSIVE: ${_he_bad} cannot be read, so ${_he_what} did not run and says"
    echo "       nothing about the code either way. Check whether the file was renamed or"
    echo "       removed, and update this audit to name the file that replaced it."
    return 1
}

cd "$REPO_ROOT"

# ---- AC-22: every call site this card adds is compiled into every binary ----------
# Membership of a conditional region cannot be decided by grep alone, so walk the file with a
# stack of open preprocessor conditionals and report, for each call site, how many enclosing
# regions select each binary. Both directions are counted, because both are defects and they
# are mirror images: a site inside #ifdef MAIZE_CONSOLE_ONLY compiles out of maizeg, and a
# site inside #ifndef MAIZE_CONSOLE_ONLY compiles out of maize. An earlier version of this
# walker tracked only the #ifdef form and passed the #ifndef case at depth 0, which under-
# implemented AC-22's own wording ("compiled into every binary") in exactly one direction.
#
# #else and #elif flip the top of the stack rather than leaving it, so a site in the other arm
# of a MAIZE_CONSOLE_ONLY conditional is attributed to the binary that arm actually selects.
echo "== AC-22: the source and park-hook call sites are compiled into maize AND maizeg"
if have_evidence "the AC-22 call-site walk" src/maize.cpp; then
    audit=$(awk '
        function counts(   i, d, n) {
            d = 0; n = 0
            for (i = 1; i <= sp; i++) { if (stack[i] == "D") d++; else if (stack[i] == "N") n++ }
            return d " " n
        }
        /^[ \t]*#[ \t]*ifdef[ \t]+MAIZE_CONSOLE_ONLY[ \t]*$/  { stack[++sp] = "D"; next }
        /^[ \t]*#[ \t]*ifndef[ \t]+MAIZE_CONSOLE_ONLY[ \t]*$/ { stack[++sp] = "N"; next }
        /^[ \t]*#[ \t]*(if|ifdef|ifndef)([ \t]|$)/            { stack[++sp] = "O"; next }
        /^[ \t]*#[ \t]*(else|elif)([ \t]|$)/ {
            if (sp > 0) { if (stack[sp] == "D") stack[sp] = "N"; else if (stack[sp] == "N") stack[sp] = "D" }
            next
        }
        /^[ \t]*#[ \t]*endif([ \t]|$)/ { if (sp > 0) sp--; next }
        /stdin_source::start\(/   { printf "start %d %s\n", NR, counts() }
        /stdin_source::stop\(/    { printf "stop %d %s\n", NR, counts() }
        /cpu::set_park_input\(/   { printf "set_park_input %d %s\n", NR, counts() }
    ' src/maize.cpp)
    if [ -z "$audit" ]; then
        fail "no call sites found at all in src/maize.cpp"
    else
        printf '%s\n' "$audit" | while IFS=' ' read -r what line din dnot; do
            if [ "$din" = '0' ] && [ "$dnot" = '0' ]; then
                echo "       ${what} at src/maize.cpp:${line}, outside every MAIZE_CONSOLE_ONLY region"
            elif [ "$din" != '0' ]; then
                echo "       ${what} at src/maize.cpp:${line}, INSIDE #ifdef (compiled out of maizeg)"
            else
                echo "       ${what} at src/maize.cpp:${line}, INSIDE #ifndef (compiled out of maize)"
            fi
        done
        bad=$(printf '%s\n' "$audit" | awk '$3 != 0 || $4 != 0' | wc -l)
        if [ "$bad" -eq 0 ]; then
            pass "every call site is compiled into both maize and maizeg"
        else
            fail "${bad} call site(s) are compiled out of one binary or the other"
        fi
    fi
fi
echo ""

# ---- AC-27: the instruction-tick pump is still wired, in both branches ------------
echo "== AC-27: the pump is wired in both stdin-owner branches, unconditional on start()"
if have_evidence "the AC-27 pump-wiring audit" src/maize.cpp; then
    pump=$(grep -n 'cpu::set_active_input(&console);' src/maize.cpp || true)
    n=$(printf '%s\n' "$pump" | grep -c . || true)
    printf '%s\n' "$pump" | sed 's/^/       src\/maize.cpp:/'
    if [ "$n" -eq 2 ]; then
        pass "both stdin-owner branches wire the pump"
    else
        fail "expected 2 wiring sites, found ${n}"
    fi
    # The wiring must not sit inside a branch conditional on the source having started, which is
    # the shape that made an earlier cycle's degraded path worse than the defect.
    if grep -n 'src_ok' src/maize.cpp | grep -q 'set_active_input'; then
        fail "a pump wiring site is conditional on stdin_source::start()"
    else
        pass "no wiring site is conditional on stdin_source::start()"
    fi
fi
echo ""

# ---- AC-21: the Windows read and readiness paths are untouched -------------------
echo "== AC-21: the shipped Windows probe TU is untouched since ${AGAINST}"
# git diff --quiet answers 0 for identical and 1 for changed, and anything else means it did
# not answer at all. Those three have to stay distinct: reporting "the probe TU changed" when
# git merely could not run turns an infrastructure failure into a substantive finding about
# the code, and a reader has no way to tell the two apart from the verdict line. The case is
# not hypothetical here. An isolated worktree's .git file names a Windows-absolute gitdir that
# a WSL-side git cannot resolve, so this audit answers 128 under WSL and 0 under Git Bash on
# the same tree.
git_rc=0
git diff --quiet "$AGAINST" -- src/console_probe_win32.h src/console_probe_win32.cpp 2>/dev/null \
    || git_rc=$?
if [ "$git_rc" -eq 0 ]; then
    pass "src/console_probe_win32.{h,cpp} are byte-identical to ${AGAINST}"
elif [ "$git_rc" -eq 1 ]; then
    fail "the probe TU changed, which is the signal the design drifted back toward consuming"
    git diff --stat "$AGAINST" -- src/console_probe_win32.h src/console_probe_win32.cpp | sed 's/^/       /'
else
    fail "INCONCLUSIVE: git exited ${git_rc}, so this audit did not run and says nothing about"
    echo "       the probe TU either way. The usual cause is running from a filesystem side"
    echo "       whose git cannot resolve this worktree's gitdir; re-run it from the other side."
fi

# This is the check that guards the operator's standing rule against resting a design on
# undocumented console behaviour, so an unreadable file here must never read as a pass.
if have_evidence "the AC-21 no-read audit" src/stdin_source_win32.cpp; then
    if grep -qE 'ReadFile|ReadConsole|ReadConsoleInput' src/stdin_source_win32.cpp; then
        fail "the Windows source TU issues a read"
        grep -nE 'ReadFile|ReadConsole|ReadConsoleInput' src/stdin_source_win32.cpp | sed 's/^/       /'
    else
        pass "src/stdin_source_win32.cpp contains no ReadFile, ReadConsole or ReadConsoleInput"
    fi
fi
echo ""

# ---- AC-8: fd 0 has exactly one set of readers, and the source is not among them --
echo "== AC-8: no source thread reads fd 0, and every host-stdin reader still funnels through the seam"
# The fd-0 check pipes grep into grep, which is the worst form of the conflation this guard
# removes: a pipeline reports the LAST command's status, so a missing source TU used to yield
# an empty stream, a status of 1 from the filter, and a printed pass.
if have_evidence "the AC-8 fd-0 audit" src/stdin_source_posix.cpp src/stdin_source_win32.cpp; then
    if grep -nE '(^|[^a-z_])read\(0[,)]' src/stdin_source_posix.cpp src/stdin_source_win32.cpp | grep -v 'g_wake_rd'; then
        fail "a source TU reads fd 0"
    else
        pass "neither source TU reads fd 0 (the POSIX self-pipe read is its own descriptor)"
    fi
fi
if have_evidence "the AC-8 device-reader count" src/devices.cpp; then
    readers=$(grep -c 'syscall::read(0' src/devices.cpp || true)
    echo "       ${readers} host-stdin readers in src/devices.cpp, plus the SYS \$00 path in src/sys.cpp"
    if [ "$readers" -eq 4 ]; then
        pass "the four device readers are unchanged"
    else
        fail "expected 4 device readers, found ${readers}"
    fi
fi
echo ""

echo "audits: ${FAILED} failed"
[ "$FAILED" -eq 0 ]
