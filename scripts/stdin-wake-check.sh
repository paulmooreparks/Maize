#!/bin/sh
# maize-313: the stdin-wake fixture group.
#
# A HALT-parked CPU retires no instructions, so the console device's input pump, which runs
# on the instruction tick, cannot see the next stdin byte. This card gives the VM a host stdin
# source that raises IRQ 33 from its own thread and a park hook that arms it, and lets
# quesos_idle park instead of burning a core. These fixtures are the evidence for that.
#
# Every one of them is a wake test rather than a shell test, so every one has a matching
# negative control in scripts/negctl/maize-313, which deletes the mechanism and requires the
# fixture to fail. Run those with that directory's own driver; a green run here alone does not
# establish anything, because a design that never parked would also pass.
#
# Usage: scripts/stdin-wake-check.sh [--preset <name>] [--work <dir>]

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "${SCRIPT_DIR}/.." && pwd)
. "${SCRIPT_DIR}/lib/harness-env.sh"
PRESET='linux-debug'
WORK=''
MAIZE_OVERRIDE=''

while [ $# -gt 0 ]; do
    case "$1" in
        --preset) PRESET="$2"; shift 2 ;;
        --preset=*) PRESET="${1#--preset=}"; shift ;;
        --work) WORK="$2"; shift 2 ;;
        --work=*) WORK="${1#--work=}"; shift ;;
        --maize) MAIZE_OVERRIDE="$2"; shift 2 ;;
        *) echo "stdin-wake-check: unknown argument '$1'" >&2; exit 2 ;;
    esac
done

BUILD_DIR="${REPO_ROOT}/build/${PRESET}"
if [ -n "$MAIZE_OVERRIDE" ]; then MAIZE="$MAIZE_OVERRIDE"; else MAIZE="${BUILD_DIR}/maize"; fi
MAZM="${BUILD_DIR}/mazm"
MZCC="${BUILD_DIR}/mzcc"
CC_MAIZE="${SCRIPT_DIR}/cc-maize.sh"
[ -n "$WORK" ] || WORK="${BUILD_DIR}/stdin-wake"

TOTAL=0
FAILED=0
LOG="${WORK}/build.log"

pass() { echo "[PASS] $*"; }
fail() { echo "[FAIL] $*"; FAILED=$((FAILED + 1)); }

# have_evidence <what> <path>...
#   Returns 0 when every named path is a readable regular file; otherwise fails the leg
#   naming the first that is not, and returns 1 so the caller skips it. A leg that runs a
#   guest image measures nothing when the image is absent, and several here bound a wall
#   clock, which a run that never started passes easily.
#
#   The failure path counts itself into TOTAL, because the legs it guards never reach their
#   own increment and a report whose failure count exceeds its total is unreadable.
have_evidence() {
    _he_what="$1"; shift
    if _he_bad=$(maize_require_file "$@"); then
        return 0
    fi
    TOTAL=$((TOTAL + 1))
    fail "${_he_what} (INCONCLUSIVE: ${_he_bad} is missing, so this leg did not run)"
    return 1
}

for tool in "$MAIZE" "$MAZM" "$MZCC"; do
    if [ ! -x "$tool" ]; then
        echo "stdin-wake-check: ${tool} is missing; build the ${PRESET} preset first" >&2
        exit 2
    fi
done

rm -rf "$WORK"; mkdir -p "$WORK"
: > "$LOG"

QUESOS="${WORK}/quesos.mzx"
if ! "$MZCC" build-quesos --preset "$PRESET" -o "$QUESOS" >>"$LOG" 2>&1 || [ ! -f "$QUESOS" ]; then
    echo "stdin-wake-check: quesOS link failed" >&2; tail -30 "$LOG" >&2; exit 2
fi
for src in stdin_wake_readers stdin_wake_bytes stdin_wake_eof stdin_wake_mixed stdin_wake_selecttimeout; do
    if ! "$CC_MAIZE" --preset "$PRESET" -o "${WORK}/${src}.mzx" \
            "${REPO_ROOT}/os/quesos/${src}.c" >>"$LOG" 2>&1; then
        echo "stdin-wake-check: ${src}.c did not compile" >&2; tail -30 "$LOG" >&2; exit 2
    fi
done
if ! "$MAZM" "${REPO_ROOT}/asm/test_relatch.mazm" >>"$LOG" 2>&1; then
    echo "stdin-wake-check: asm/test_relatch.mazm did not assemble" >&2; exit 2
fi

# Run one quesOS guest with $1 as its stdin producer. The producer is a shell function name,
# so a fixture whose whole point is the TIMING of its input can express that timing.
guest_run() {
    _prog="$1"; _feed="$2"; _fault="${3:-}"; _tmo="${4:-40}"
    MSYS2_ARG_CONV_EXCL='/progs'; export MSYS2_ARG_CONV_EXCL
    "feed_${_feed}" \
        | MAIZE_FAULT="$_fault" timeout "$_tmo" "$MAIZE" --no-root --show-perf \
            --mount "${WORK}=/progs:ro" --rom "$QUESOS" "/progs/${_prog}.mzx" 2>&1 || true
}

# Every fixture asserts the park-hook counter equality as well as its own marker. That
# equality is the machine-checkable form of "every path into the park runs the park hook
# first", which is the property three spec cycles asserted and the code falsified, and it is
# invisible to every functional criterion here: the hang it guards against needs two parked
# readers and two closely spaced bytes to show itself.
check_park_equality() {
    _out="$1"; _label="$2"
    _tr=$(printf '%s\n' "$_out" | sed -n 's/^ *tick returns *: *//p' | tail -1 | tr -d '\r')
    _ph=$(printf '%s\n' "$_out" | sed -n 's/^ *park hooks *: *//p' | tail -1 | tr -d '\r')
    if [ -z "$_tr" ] || [ -z "$_ph" ]; then
        fail "${_label}: the park counters were not reported"
        return 1
    fi
    if [ "$_tr" != "$_ph" ]; then
        fail "${_label}: park-hook counter equality broken (tick returns=${_tr}, park hooks=${_ph})"
        return 1
    fi
    return 0
}

# The performance report ends every line with CR-LF, so a counter read out of it carries a
# trailing carriage return. Strip it here rather than downstream. dash's `test` happens to
# treat that CR as trailing whitespace and compare the number correctly anyway, which is why
# this went unnoticed, but a comparison that works because of one shell's leniency is not a
# comparison, and an anchored grep against the same text matches nothing at all.
counter_of() { printf '%s\n' "$2" | sed -n "s/^ *$1 *: *//p" | tail -1 | tr -d '\r'; }

# ---- the stdin producers --------------------------------------------------------
# Each sleep is what puts the kernel on its idle path between bytes. Without them the guest
# never parks and none of these fixtures tests anything.

# The leading sleep is the whole fixture. Written at once, the two bytes are in the pipe
# before the guest has booted, so each child's own status probe finds CON_STAT_INPUT set and
# takes its byte without ever parking, and the trace under test never happens. Waiting until
# both children are genuinely parked is what makes one level change cover two parked readers.
feed_two_bytes() { sleep 2; printf 'AB'; sleep 1; }
feed_slow_200() {
    _i=0
    _alpha='abcdefghijklmnopqrstuvwxyz'
    while [ "$_i" -lt 200 ]; do
        _n=$(( (_i % 26) + 1 ))
        printf '%s' "$(printf '%s' "$_alpha" | cut -c "$_n")"
        sleep 0.02
        _i=$((_i + 1))
    done
    sleep 0.5
}
feed_late_eof()  { sleep 2; printf 'qq'; sleep 2; }  # two bytes, then close: EOF while parked
feed_mixed()     { sleep 0.5; printf 'x'; sleep 0.5; printf 'y'; sleep 0.5; printf 'z'; sleep 1; }
feed_silent()    { sleep 6; }                        # open, never readable
# Readable and never drained, for the AC-31 legs. The writer closes as soon as the bytes are
# in, and that is deliberate rather than lazy: a pipe holding unread bytes reads as data-pending
# whether or not the write end is still open, because EOF arrives only once the buffer drains
# and nothing here drains it. Holding it open with a trailing sleep would add nothing to the
# readiness answer and would put the writer's own lifetime inside the wall-clock the leg
# measures, which is a thing that has already happened once.
feed_200_unread() { head -c 200 /dev/zero | tr '\0' 'x'; }

# ---- AC-25: two readers, one level change ---------------------------------------
TOTAL=$((TOTAL + 1))
out=$(guest_run stdin_wake_readers two_bytes)
if printf '%s\n' "$out" | grep -qF 'stdin-wake-readers: PASS'; then
    check_park_equality "$out" 'stdin_wake_readers' && pass 'stdin_wake_readers (two parked readers, one level change)'
else
    fail 'stdin_wake_readers'; printf '%s\n' "$out" | sed 's/^/          | /'
fi

# ---- AC-25, the park hook in isolation ------------------------------------------
# The same trace with the instruction-tick pump silenced, so the park hook is the only caller
# of on_input_tick in the run. The pump and the hook cover overlapping intervals, and the
# pump is what makes master robust for a guest that keeps retiring instructions, so the leg
# above passes on a build with no park hook whenever the guest happens to retire 16384
# instructions between servicing the first reader and parking again. This leg cannot: with
# the pump gone, the second reader is delivered by the hook or by nothing.
TOTAL=$((TOTAL + 1))
out=$(guest_run stdin_wake_readers two_bytes no_pump)
if printf '%s\n' "$out" | grep -qF 'stdin-wake-readers: PASS'; then
    check_park_equality "$out" 'stdin_wake_readers_nopump' \
        && pass 'stdin_wake_readers_nopump (the park hook alone delivers the second byte)'
else
    fail 'stdin_wake_readers_nopump'; printf '%s\n' "$out" | sed 's/^/          | /'
fi

# ---- AC-6: the second and later bytes wake as reliably as the first --------------
TOTAL=$((TOTAL + 1))
out=$(guest_run stdin_wake_bytes slow_200 '' 90)
if printf '%s\n' "$out" | grep -qF 'stdin-wake-bytes: PASS 200'; then
    check_park_equality "$out" 'stdin_wake_bytes' && pass 'stdin_wake_bytes (200 bytes, idle path between each)'
else
    fail 'stdin_wake_bytes'; printf '%s\n' "$out" | sed 's/^/          | /'
fi

# ---- AC-7: end of input wakes a parked reader -----------------------------------
TOTAL=$((TOTAL + 1))
out=$(guest_run stdin_wake_eof late_eof)
if printf '%s\n' "$out" | grep -qF 'stdin-wake-eof: PASS 2'; then
    check_park_equality "$out" 'stdin_wake_eof' && pass 'stdin_wake_eof (pipe closed while the kernel is idle)'
else
    fail 'stdin_wake_eof'; printf '%s\n' "$out" | sed 's/^/          | /'
fi

# ---- AC-29: end of input raises even with the readiness edge already latched -----
# no_source rides alongside latch_ready deliberately, and it is what makes this criterion
# test what it says it tests. With a source running, the watcher's own POLLHUP raise wakes
# the parked reader whatever the device's edge logic did, so the fixture would go green on a
# build where the end-of-input branch raised nothing at all. Removing the source leaves
# on_input_tick as the only raiser, which is exactly the device behaviour under test, and the
# guest then spins rather than parking, so the pump keeps probing.
TOTAL=$((TOTAL + 1))
out=$(guest_run stdin_wake_eof late_eof latch_ready,no_source)
if printf '%s\n' "$out" | grep -qF 'stdin-wake-eof: PASS 2'; then
    pass 'stdin_wake_latched_eof (end of input taken with the edge already spent)'
else
    fail 'stdin_wake_latched_eof'; printf '%s\n' "$out" | sed 's/^/          | /'
fi

# ---- AC-27: the mixed set spins, and the pump still delivers ---------------------
TOTAL=$((TOTAL + 1))
out=$(guest_run stdin_wake_mixed mixed '' 60)
if printf '%s\n' "$out" | grep -qF 'stdin-wake-mixed: PASS 3 1'; then
    check_park_equality "$out" 'stdin_wake_mixed' && pass 'stdin_wake_mixed (console reader plus a finite poll deadline)'
else
    fail 'stdin_wake_mixed'; printf '%s\n' "$out" | sed 's/^/          | /'
fi

# ---- AC-11: a finite select timeout still fires when nothing else is runnable ----
TOTAL=$((TOTAL + 1))
out=$(guest_run stdin_wake_selecttimeout silent '' 40)
if printf '%s\n' "$out" | grep -qF 'stdin-wake-selecttimeout: PASS'; then
    check_park_equality "$out" 'stdin_wake_selecttimeout' && pass 'stdin_wake_selecttimeout (lone process, finite deadline)'
else
    fail 'stdin_wake_selecttimeout'; printf '%s\n' "$out" | sed 's/^/          | /'
fi

# ---- AC-28: the relatch, verified by the mechanism rather than by a payload ------
# A file-redirected stdin reaches the `latched` arm at every park after the first, because
# the readiness probe cannot answer 0 for a regular file. The guest never reads the data
# port, so nothing clears the latch and nothing about host input changes for the whole run:
# every raise after the first is therefore attributable to the relatch and to nothing else.
#
# This leg looks for a positive marker, so a missing image already goes red; the guard is here
# for the DIAGNOSIS. Without it an absent asm/test_relatch.mzb reports "no second raise
# arrived", which sends the next reader looking for a wake defect that is not there.
if have_evidence 'relatch_wake' "${REPO_ROOT}/asm/test_relatch.mzb"; then
    TOTAL=$((TOTAL + 1))
    head -c 64 /dev/urandom > "${WORK}/relatch-stdin.dat"
    t0=$(date +%s)
    out=$(timeout 30 "$MAIZE" --show-perf --bare "${REPO_ROOT}/asm/test_relatch.mzb" \
            < "${WORK}/relatch-stdin.dat" 2>&1 || true)
    t1=$(date +%s)
    rel=$(counter_of relatches "$out")
    lvl=$(counter_of 'level raises' "$out")
    if ! printf '%s\n' "$out" | grep -qF 'relatch: PASS'; then
        fail 'relatch_wake (no second raise arrived)'; printf '%s\n' "$out" | sed 's/^/          | /'
    elif [ -z "$rel" ] || [ "$rel" -lt 1 ]; then
        fail "relatch_wake (the wake was not attributable to the relatch: counter=${rel:-absent})"
    elif [ "${lvl:-0}" -ne 0 ]; then
        fail "relatch_wake (the host stdin source raised ${lvl} times, so host input was not static)"
    else
        if check_park_equality "$out" 'relatch_wake'; then
            pass "relatch_wake ($rel relatch expiries, no source raises, $((t1 - t0))s wall)"
        fi
    fi
fi

# ---- AC-31: a HALT executed with interrupts DISABLED still terminates the run ------
# The bound is the interrupt-enable state at the HALT, and it is deliberately not "no interrupt
# source exists". Nothing in the park block tests whether a source exists, so a guest that runs
# SETINT and then HALT on a VM with no source waits at int_event.wait() for the life of the
# process. These legs exercise the bound the code actually has: every one of them runs an image
# that never executes SETINT, and two of the four run WITH a source started.
#
# This is the card's highest-risk property, because getting it wrong ships a hang rather than
# the spin the card removes, and it is the one the park block's own comment states. Four legs,
# {source started, no source} x {a readable pipe nothing is consuming, /dev/null}, because the
# two conditions a wrong bound would rest on are a running source and a readable stdin.
#
# A 4096-byte zeroed image is the runaway-into-data case. Opcode $00 is HALT, and a guest that
# ran off into zeroed memory has not run SETINT, so the halt takes power_off() and the park
# block is skipped whole. The counters are the evidence rather than the exit code alone: one
# instruction retired, one tick return, one park hook (the hook runs before is_power_on is
# read, so its presence is expected), and zero relatches, which is what says the bounded-wait
# arm was never entered. A build that got this wrong would hang here and the timeout would
# turn that into a failure.
head -c 4096 /dev/zero > "${WORK}/zeroed.mzb"
for _fault in '' no_source; do
    for _shape in pipe devnull; do
        TOTAL=$((TOTAL + 1))
        _label="stdin_wake_zeroed_${_shape}_${_fault:-source}"
        _t0=$(date +%s%N)
        set +e
        case "$_shape" in
            pipe)    out=$(feed_200_unread \
                         | MAIZE_FAULT="$_fault" timeout 20 "$MAIZE" --show-perf --bare \
                             "${WORK}/zeroed.mzb" 2>&1) ;;
            devnull) out=$(MAIZE_FAULT="$_fault" timeout 20 "$MAIZE" --show-perf --bare \
                             "${WORK}/zeroed.mzb" < /dev/null 2>&1) ;;
        esac
        rc=$?
        set -e
        _t1=$(date +%s%N)
        _ms=$(( (_t1 - _t0) / 1000000 ))
        _ins=$(counter_of instructions "$out")
        _rel=$(counter_of relatches "$out")
        if [ "$rc" -ne 0 ]; then
            fail "${_label} (the run did not exit 0: status ${rc})"
            printf '%s\n' "$out" | sed 's/^/          | /'
        elif [ "${_ins:-0}" -ne 1 ]; then
            fail "${_label} (retired ${_ins:-absent} instructions, not the single HALT)"
        elif [ "${_rel:-1}" -ne 0 ]; then
            fail "${_label} (the relatch arm was entered ${_rel} times; it must be unreachable here)"
        elif ! check_park_equality "$out" "$_label"; then
            :
        elif [ "$_ms" -ge 2000 ]; then
            fail "${_label} (took ${_ms} ms; a clean power-off halt must not park)"
        else
            pass "${_label} (exit 0 in ${_ms} ms, 1 instruction, 0 relatches)"
        fi
    done
done

# ---- the degraded modes, which are what a failed source must not cost --------------
# H7 rule 1: a failed start degrades to master's behaviour exactly. The pump is wired
# unconditionally, so it keeps raising on the instruction tick; CON_STAT_WAKE reads clear, so
# the guest spins rather than parking; and a spinning guest retires instructions, so the pump
# runs. Nothing else in the VM changes.
TOTAL=$((TOTAL + 1))
out=$(guest_run stdin_wake_bytes slow_200 no_source 90)
if printf '%s\n' "$out" | grep -qF 'stdin-wake-bytes: PASS 200'; then
    _ins=$(counter_of instructions "$out")
    pass "stdin_wake_degraded_no_source (the guest ran on master's path, ${_ins} instructions retired)"
else
    fail 'stdin_wake_degraded_no_source'; printf '%s\n' "$out" | sed 's/^/          | /'
fi

# H7 rule 3: a source that dies mid-run is indistinguishable from end of input, on every exit
# path and not only on the clean drained-pipe one. The guest must see EOF and exit rather than
# park with no waker.
TOTAL=$((TOTAL + 1))
out=$(guest_run stdin_wake_eof silent source_die 25)
if printf '%s\n' "$out" | grep -qF 'stdin-wake-eof: PASS'; then
    pass 'stdin_wake_source_died (a dead source reads as end of input, not as a park)'
else
    fail 'stdin_wake_source_died'; printf '%s\n' "$out" | sed 's/^/          | /'
fi

# ---- AC-9: the VM exits promptly on every stdin shape ------------------------------
# stop() joins its thread unconditionally, with no grace period and no detach, which is
# available only because no source thread ever blocks in a read. The exit time is the evidence
# for that rather than a formality.
#
# A leg whose bar is an UPPER bound on wall-clock time passes trivially when the run never
# happened, so both ways of not happening are closed here. The image is checked readable
# before the loop, and each run's exit status is kept rather than swallowed by `|| true`:
# hello.mzb halts with interrupts disabled and exits 0, so any other status means the VM
# stopped for a reason of its own and the elapsed time measures that instead of a clean join.
HELLO="${REPO_ROOT}/asm/hello.mzb"
if have_evidence "stdin_wake_exit" "$HELLO"; then
    for shape in pipe file devnull; do
        TOTAL=$((TOTAL + 1))
        case "$shape" in
            pipe)   _redir_setup() { printf 'q'; sleep 0.2; } ;;
            file)   head -c 8 /dev/urandom > "${WORK}/exit-stdin.dat" ;;
            devnull) : ;;
        esac
        _t0=$(date +%s%N)
        set +e
        case "$shape" in
            pipe)    _redir_setup | timeout 20 "$MAIZE" --bare "$HELLO" >/dev/null 2>&1 ;;
            file)    timeout 20 "$MAIZE" --bare "$HELLO" < "${WORK}/exit-stdin.dat" >/dev/null 2>&1 ;;
            devnull) timeout 20 "$MAIZE" --bare "$HELLO" < /dev/null >/dev/null 2>&1 ;;
        esac
        _rc=$?
        set -e
        _t1=$(date +%s%N)
        _ms=$(( (_t1 - _t0) / 1000000 ))
        if [ "$_rc" -ne 0 ]; then
            fail "stdin_wake_exit_${shape} (the VM exited ${_rc}, so the ${_ms} ms is not a clean-join time)"
        elif [ "$_ms" -lt 1000 ]; then
            pass "stdin_wake_exit_${shape} (the VM exited ${_ms} ms after the guest halted)"
        else
            fail "stdin_wake_exit_${shape} (took ${_ms} ms, over the one-second bar)"
        fi
    done
fi

echo ""
echo "stdin-wake: $((TOTAL - FAILED)) passed, ${FAILED} failed (${TOTAL} total)"
[ "$FAILED" -eq 0 ]
