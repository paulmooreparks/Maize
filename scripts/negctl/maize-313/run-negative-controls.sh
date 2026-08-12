#!/bin/sh
# maize-313: run every negative control this card's acceptance criteria name.
#
# A fixture that has never failed is not evidence, so each wake criterion pairs a green run
# from scripts/stdin-wake-check.sh against a build with one thing removed. Each removal is a
# real one-hunk patch in this directory rather than a runtime flag, because the criteria ask
# for a BUILD and because a patch is something a reviewer can read next to the code it
# deletes. The driver applies one patch, builds, runs the fixture the patch is supposed to
# break, asserts that it FAILS, and reverts. A control that passes is itself a failure here.
#
# The card's MAIZE_FAULT tokens are a different instrument and are deliberately not used for
# these: those BUILD states no ordinary run can be steered into, where these DELETE code.
#
# The guest images are reused across the VM-side controls rather than rebuilt, because a
# .mzx is guest code and none of those five patches touches guest code. Only the sixth,
# which parks the kernel unconditionally, needs quesOS itself relinked.
#
# The run has two passes, and the first is what makes the second mean anything. A control that
# only ever runs against a doctored build records that its fixture did not print a marker, not
# that the patch is why. Anything else that stops the fixture reads the same way: a missing
# guest image, a stale .mzb, a build directory that did not configure. So pass 1 builds
# NEG_DIR with the tree UNPATCHED and requires every selected check to PASS there, and pass 2
# applies each patch and requires the same check to FAIL. A control whose baseline did not hold
# is reported as a broken instrument and its patched leg is not run at all, because a red
# result from an instrument that cannot go green is not evidence either.
#
# Usage: scripts/negctl/maize-313/run-negative-controls.sh [--preset <name>] [control ...]

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "${SCRIPT_DIR}/../../.." && pwd)
. "${REPO_ROOT}/scripts/lib/harness-env.sh"
PRESET='linux-debug'

while [ $# -gt 0 ]; do
    case "$1" in
        --preset) PRESET="$2"; shift 2 ;;
        --preset=*) PRESET="${1#--preset=}"; shift ;;
        *) break ;;
    esac
done

BUILD_DIR="${REPO_ROOT}/build/${PRESET}"
NEG_DIR="${REPO_ROOT}/build/negctl-maize-313"
ART="${BUILD_DIR}/stdin-wake"
MZCC="${BUILD_DIR}/mzcc"
LOG="${NEG_DIR}/negctl.log"

WANT="${*:-no-relatch no-park-hook no-ack eof-fallthrough guest-park-always halt-always-parks}"
PASSED=0
FAILED=0

# Every binary any selected check actually runs, guarded up front and by name. The earlier
# form of this guard checked quesos.mzx alone, which left a missing or stale asm/test_relatch.mzb
# reading as a held control: the fixture would fail to run and the driver would score that as
# the mechanism's removal being caught. An artifact older than the source it was built from is
# the same defect one step subtler, so both conditions are checked here.
# The existence half goes through maize_require_file so the whole card has ONE definition of
# "this file is there and I can read it"; the staleness half is this driver's own, because
# only a control knows which source its artifact was built from.
require_artifact() {
    _art="$1"; _src="$2"
    if ! _bad=$(maize_require_file "$_art"); then
        echo "negctl: ${_bad} is missing or unreadable; run scripts/stdin-wake-check.sh first" >&2
        exit 2
    fi
    if [ -n "$_src" ] && [ -f "$_src" ] && [ "$_src" -nt "$_art" ]; then
        echo "negctl: ${_art} is older than ${_src}; re-run scripts/stdin-wake-check.sh" >&2
        exit 2
    fi
}

for _need in $WANT; do
    case "$_need" in
        no-relatch)        require_artifact "${REPO_ROOT}/asm/test_relatch.mzb" \
                                            "${REPO_ROOT}/asm/test_relatch.mazm" ;;
        no-park-hook)      require_artifact "${ART}/quesos.mzx" ''
                           require_artifact "${ART}/stdin_wake_readers.mzx" \
                                            "${REPO_ROOT}/os/quesos/stdin_wake_readers.c" ;;
        no-ack)            require_artifact "${ART}/quesos.mzx" ''
                           require_artifact "${ART}/stdin_wake_bytes.mzx" \
                                            "${REPO_ROOT}/os/quesos/stdin_wake_bytes.c" ;;
        eof-fallthrough)   require_artifact "${ART}/quesos.mzx" ''
                           require_artifact "${ART}/stdin_wake_eof.mzx" \
                                            "${REPO_ROOT}/os/quesos/stdin_wake_eof.c" ;;
        guest-park-always) require_artifact "${ART}/stdin_wake_selecttimeout.mzx" \
                                            "${REPO_ROOT}/os/quesos/stdin_wake_selecttimeout.c" ;;
        halt-always-parks) : ;;   # the driver writes its own zeroed image, so nothing to guard
        *) echo "negctl: unknown control '${_need}'" >&2; exit 2 ;;
    esac
done

mkdir -p "$NEG_DIR"
: > "$LOG"

# Revert whatever a control patched, whatever happened, so an interrupted run cannot leave a
# doctored tree behind. This is the one place that touches the working tree.
# patch(1) rather than `git apply`, deliberately. This card's work is done in an isolated
# worktree whose root .git FILE names a Windows-absolute gitdir, which a WSL-side git cannot
# resolve, so every git command in that tree dies with "not a git repository" while the
# Windows-side git is fine. patch(1) needs no repository at all, so the driver runs the same
# way from either side.
CURRENT_PATCH=''
cleanup() {
    if [ -n "$CURRENT_PATCH" ]; then
        ( cd "$REPO_ROOT" && patch -p1 -R -s < "$CURRENT_PATCH" ) 2>/dev/null || true
        CURRENT_PATCH=''
    fi
    # patch(1) leaves a .orig beside every file it touches, and a stray one in src/ reads as
    # an uncommitted change to whoever looks next. Clear them here rather than leaving the
    # tree dirty, and scope the sweep to the files these patches actually name.
    for _f in src/cpu.cpp src/sys.cpp src/devices.cpp os/quesos/quesos.c; do
        rm -f "${REPO_ROOT}/${_f}.orig" "${REPO_ROOT}/${_f}.rej"
    done
}
trap cleanup EXIT INT TERM

# The doctored VM goes in its own build directory, so the ordinary build stays green and
# usable and a control's binary can never be mistaken for the real one.
build_neg_vm() {
    cmake -S "$REPO_ROOT" -B "$NEG_DIR" -DCMAKE_BUILD_TYPE=Debug >>"$LOG" 2>&1 \
        && cmake --build "$NEG_DIR" --target maize >>"$LOG" 2>&1
}

run_guest() {
    _maize="$1"; _quesos="$2"; _prog="$3"; _feed="$4"; _fault="${5:-}"; _tmo="${6:-40}"
    MSYS2_ARG_CONV_EXCL='/progs'; export MSYS2_ARG_CONV_EXCL
    "feed_${_feed}" | MAIZE_FAULT="$_fault" timeout "$_tmo" "$_maize" --no-root \
        --mount "${ART}=/progs:ro" --rom "$_quesos" "/progs/${_prog}.mzx" 2>&1 || true
}

feed_two_bytes() { sleep 2; printf 'AB'; sleep 1; }
# 200 bytes, because stdin_wake_bytes.c reads exactly WANT=200 of them before it prints its
# marker. An earlier 40-byte version of this feed could not make the fixture pass on ANY
# build, doctored or clean, so the control it fed recorded a short read rather than the
# missing acknowledgement. The baseline pass is what surfaced that; keep the two counts equal.
feed_slow_200()  { _i=0; while [ "$_i" -lt 200 ]; do
                       printf '%s' "$(printf '%s' abcdefghijklmnopqrstuvwxyz | cut -c "$(( (_i % 26) + 1 ))")"
                       sleep 0.02; _i=$((_i + 1)); done; sleep 1; }
feed_late_eof()  { sleep 2; printf 'qq'; sleep 2; }
feed_silent()    { sleep 6; }

# $1 control name, $2 a function returning 0 when the fixture PASSES (the bad outcome in
# pass 2 and the required outcome in pass 1).
baseline() {
    _name="$1"; _check="$2"
    case " $WANT " in *" $_name "*) ;; *) return 0 ;; esac
    set +e
    "$_check"
    _rc=$?
    set -e
    if [ "$_rc" -eq 0 ]; then
        echo "[BASE] ${_name}: the check passes against the unpatched build, so it can go green"
    else
        echo "[FAIL] ${_name}: the check FAILS against the unpatched build, so it is a broken"
        echo "              instrument and its patched leg proves nothing. Not run."
        FAILED=$((FAILED + 1))
        WANT=$(printf '%s\n' "$WANT" | sed "s/${_name}//")
    fi
}

# $1 control name, $2 a function returning 0 when the fixture PASSES (the bad outcome here),
# $3 either 'vm' (relink the VM) or 'guest' (relink quesOS with the stock VM).
control() {
    _name="$1"; _check="$2"; _kind="$3"
    case " $WANT " in *" $_name "*) ;; *) return 0 ;; esac
    _patch="${SCRIPT_DIR}/${_name}.patch"
    if ! _bad=$(maize_require_file "$_patch"); then
        echo "[FAIL] ${_name}: no readable patch file at ${_bad}"; FAILED=$((FAILED + 1)); return 0
    fi
    if ! ( cd "$REPO_ROOT" && patch -p1 --dry-run -s < "$_patch" ) >/dev/null 2>&1; then
        echo "[FAIL] ${_name}: the patch no longer applies, so the code it removes has moved"
        FAILED=$((FAILED + 1)); return 0
    fi
    ( cd "$REPO_ROOT" && patch -p1 -s < "$_patch" )
    CURRENT_PATCH="$_patch"
    if [ "$_kind" = 'vm' ]; then
        if ! build_neg_vm; then
            echo "[FAIL] ${_name}: the doctored tree did not build"
            cleanup; FAILED=$((FAILED + 1)); return 0
        fi
    else
        if ! "$MZCC" build-quesos --preset "$PRESET" -o "${NEG_DIR}/quesos.mzx" >>"$LOG" 2>&1; then
            echo "[FAIL] ${_name}: the doctored quesOS did not link"
            cleanup; FAILED=$((FAILED + 1)); return 0
        fi
    fi
    set +e
    "$_check"
    _rc=$?
    set -e
    cleanup
    if [ "$_rc" -eq 0 ]; then
        echo "[FAIL] ${_name}: the fixture PASSED with the mechanism removed, so it proves nothing"
        FAILED=$((FAILED + 1))
    else
        echo "[PASS] ${_name}: the fixture fails with the mechanism removed, as it must"
        PASSED=$((PASSED + 1))
    fi
}

# ---- the checks. Each returns 0 when its fixture passes. -------------------------

# AC-28. The relatch is the only thing that can produce a second raise here, because the
# guest never reads the data port and host input never changes.
check_relatch() {
    head -c 64 /dev/urandom > "${NEG_DIR}/stdin.dat"
    timeout 12 "${NEG_DIR}/maize" --bare "${REPO_ROOT}/asm/test_relatch.mzb" \
        < "${NEG_DIR}/stdin.dat" 2>/dev/null | grep -qF 'relatch: PASS'
}

# AC-25. Input already pending AT the park, which only the park hook covers. The pump is
# silenced for this control, and that is not a weakening of it. The pump and the hook cover
# overlapping intervals by design, because D-26 keeps the pump precisely for being what makes
# master robust for a guest that keeps retiring instructions. With both live this fixture
# passes on a hookless build whenever the guest happens to retire 16384 instructions between
# servicing the first reader and parking again, which makes the control a race rather than a
# control. Silencing the pump isolates the one interval the hook exists to cover, which is the
# instant the CPU stops retiring instructions altogether.
check_park_hook() {
    run_guest "${NEG_DIR}/maize" "${ART}/quesos.mzx" stdin_wake_readers two_bytes no_pump 25 \
        | grep -qF 'stdin-wake-readers: PASS'
}

# AC-6. Input arriving AFTER the park, which only the acknowledgement covers.
check_no_ack() {
    run_guest "${NEG_DIR}/maize" "${ART}/quesos.mzx" stdin_wake_bytes slow_200 '' 60 \
        | grep -qF 'stdin-wake-bytes: PASS'
}

# AC-29. End of input taken with the readiness edge already spent.
check_eof() {
    run_guest "${NEG_DIR}/maize" "${ART}/quesos.mzx" stdin_wake_eof late_eof latch_ready,no_source 25 \
        | grep -qF 'stdin-wake-eof: PASS'
}

# AC-11. A finite poll deadline is served by the instruction-tick timer, and a parked CPU
# stops that timer, so a kernel that parks unconditionally never returns from the select.
check_guest_park() {
    run_guest "${BUILD_DIR}/maize" "${NEG_DIR}/quesos.mzx" stdin_wake_selecttimeout silent '' 25 \
        | grep -qF 'stdin-wake-selecttimeout: PASS'
}

# AC-31. A core that reaches HALT without ever having enabled interrupts powers off, so a
# runaway into zeroed memory terminates rather than parking with nothing able to wake it. The
# patch removes the power_off and every HALT parks, which turns this run into a hang the
# timeout collects. The counters are asserted alongside the exit code because a build that
# exits for some other reason should not read as the mechanism working.
# The tr strips the performance report's CR-LF carriage returns, without which the anchored
# patterns below match nothing and this control reports held having proved nothing. That is
# the exact shape the baseline pass exists to catch, and it caught this one.
check_zeroed_halt() {
    head -c 4096 /dev/zero > "${NEG_DIR}/zeroed.mzb"
    # The capture and the strip are two statements deliberately. Piping the run into tr would
    # put tr's exit status where timeout's belongs, and timeout's is the signal this control
    # rests on: the patched build hangs and 124 is how that arrives.
    _z=$(timeout 12 "${NEG_DIR}/maize" --show-perf --bare "${NEG_DIR}/zeroed.mzb" \
            < /dev/null 2>&1) || return 1
    _z=$(printf '%s\n' "$_z" | tr -d '\r')
    printf '%s\n' "$_z" | grep -qE '^ *instructions *: *1$' || return 1
    printf '%s\n' "$_z" | grep -qE '^ *relatches *: *0$' || return 1
    return 0
}

# ---- pass 1: the baseline, against a build with nothing removed --------------------
# Both artifacts the checks reach for are built here, unpatched, so pass 1 exercises exactly
# the code paths pass 2 will doctor. Only after every selected check has been seen to pass
# does a red result in pass 2 carry the patch's name.
echo "negctl: building the unpatched baseline in ${NEG_DIR}"
if ! build_neg_vm; then
    echo "negctl: the UNPATCHED tree did not build; nothing here can be trusted" >&2
    tail -30 "$LOG" >&2; exit 2
fi
case " $WANT " in
    *" guest-park-always "*)
        if ! "$MZCC" build-quesos --preset "$PRESET" -o "${NEG_DIR}/quesos.mzx" >>"$LOG" 2>&1; then
            echo "negctl: the UNPATCHED quesOS did not link" >&2; tail -30 "$LOG" >&2; exit 2
        fi ;;
esac

baseline no-relatch        check_relatch
baseline no-park-hook      check_park_hook
baseline no-ack            check_no_ack
baseline eof-fallthrough   check_eof
baseline guest-park-always check_guest_park
baseline halt-always-parks check_zeroed_halt

# ---- pass 2: each mechanism removed in turn ----------------------------------------
control no-relatch        check_relatch     vm
control no-park-hook      check_park_hook   vm
control no-ack            check_no_ack      vm
control eof-fallthrough   check_eof         vm
control guest-park-always check_guest_park  guest
control halt-always-parks check_zeroed_halt vm

echo ""
echo "negative controls: ${PASSED} held, ${FAILED} did not"
[ "$FAILED" -eq 0 ]
