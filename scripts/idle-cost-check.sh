#!/bin/sh
# maize-313: the idle-cost evidence, taken rather than argued.
#
# Two measurements, and neither is sufficient alone: a design that parked the guest and spun
# a host thread instead would pass the first and fail the second.
#
#   1. The retired-instruction proxy. A parked CPU retires no instructions, so the difference
#      between two runs that differ ONLY in how long they sit idle is a machine-checkable
#      proxy for "the guest is not spinning", and it needs no CPU-time API at all. Run against
#      a reference binary as well as this branch, because a fixture that has never failed is
#      not evidence.
#   2. The host CPU cost, in core-equivalents. CPU-seconds consumed per wall-clock second,
#      where 1.00 is one core saturated. Sampled from /proc/<pid>/stat rather than from a
#      per-core utilization graph, because a single busy thread migrates across cores and
#      never pegs one graph.
#
# The session is a real quesOS oksh at its prompt with nothing typed, which is the state the
# card exists to make free.
#
# Usage: scripts/idle-cost-check.sh --quesos <quesos.mzx> --bin <bindir> --rw <rwdir>
#                                   [--maize <binary>] [--label <name>] [--trials N]
#                                   [--extra '<flags>']
#
# --extra passes flags through to both halves, which is how the JIT tier is selected: pass
# nothing for the default tier and '--no-jit' for the interpreter.

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "${SCRIPT_DIR}/.." && pwd)
. "${SCRIPT_DIR}/lib/harness-env.sh"
MAIZE="${REPO_ROOT}/build/linux-debug/maize"
QUESOS=''
BINDIR=''
RWDIR=''
LABEL='branch'
TRIALS=3
EXTRA=''

while [ $# -gt 0 ]; do
    case "$1" in
        --maize)  MAIZE="$2"; shift 2 ;;
        --quesos) QUESOS="$2"; shift 2 ;;
        --bin)    BINDIR="$2"; shift 2 ;;
        --rw)     RWDIR="$2"; shift 2 ;;
        --label)  LABEL="$2"; shift 2 ;;
        --trials) TRIALS="$2"; shift 2 ;;
        --extra)  EXTRA="$2"; shift 2 ;;
        *) echo "idle-cost-check: unknown argument '$1'" >&2; exit 2 ;;
    esac
done

for v in "$QUESOS" "$BINDIR" "$RWDIR"; do
    if [ -z "$v" ]; then
        echo "idle-cost-check: --quesos, --bin and --rw are all required" >&2; exit 2
    fi
done

# maize-442: both mount directories are converted to the host-native form ONCE, here,
# rather than at each --mount construction below. The two sites that build a grant
# (run_with_gap and the core-equivalents trial loop) are the only remaining uses of
# either variable, and neither needs the POSIX form, so a single conversion is safe and
# leaves no second place for a wrong one to hide. Converting at each call site instead
# was verifiable at the first site and not the second: reconstructing run_with_gap is
# cheap, while driving the trial loop needs a fifo, a background feeder and a watchdog.
BINDIR=$(maize_host_to_native "$BINDIR")
RWDIR=$(maize_host_to_native "$RWDIR")

# The other half of the same argv contract, and it belongs next to the conversion above
# rather than at the two invocations, for the reason the conversion does. Converting the
# HOST side of a --mount grant is only half the job on MSYS: the GUEST paths this script
# passes, /bin/oksh.mzx below and the /bin and /rw grant targets, are plain colon-free
# arguments, so the argv heuristic DOES fire on them and rewrites /bin/oksh.mzx into
# C:/Program Files/Git/usr/bin/oksh.mzx. Fixing only the host half is worse than fixing
# neither: an unconverted mount is rejected loudly and this script's own capture swallows
# a mangled guest path, so the run reports a green idle-cost measurement taken from a VM
# that never loaded a shell. The value is SEMICOLON-separated (a colon makes it one
# literal prefix that matches nothing), and matches the start of the whole argument.
# scripts/run-ctest.sh:4334, :4412 and :4485 pair the same two halves at the same
# /bin/oksh.mzx invocation.
MSYS2_ARG_CONV_EXCL='/bin;/rw'; export MSYS2_ARG_CONV_EXCL
if [ ! -x "$MAIZE" ]; then echo "idle-cost-check: no VM at ${MAIZE}" >&2; exit 2; fi
# The kernel image is checked here rather than discovered mid-run. Both measurements below
# read counters out of a VM run, and a run that died on a missing --rom reports no counters
# at all, which the proxy would report as "--show-perf gave no instruction total" and the
# core-equivalents half as a VM that vanished before the window closed. Neither names the
# actual cause.
if ! _bad=$(maize_require_file "$QUESOS"); then
    echo "idle-cost-check: no readable quesOS image at ${_bad}" >&2; exit 2
fi

# Feed the shell a script with a gap of $1 seconds before `exit`, so two runs differ only in
# how long the kernel sits on its idle path.
run_with_gap() {
    _gap="$1"; _extra="${2:-}"
    { printf 'pwd\n'; sleep "$_gap"; printf 'exit\n'; sleep 0.3; } \
        | timeout 120 "$MAIZE" --no-root --show-perf $_extra \
            --mount "${BINDIR}=/bin:ro" --mount "${RWDIR}=/rw:rw" \
            --rom "$QUESOS" /bin/oksh.mzx 2>&1
}

# The strip is not optional. src/perf.cpp terminates every report line with CR-LF on every
# platform, deliberately, so the captured value carries a trailing carriage return and the
# arithmetic below is what breaks on it: `test` treats the CR as whitespace under dash, but
# $(( )) does not, and bash rejects it outright rather than comparing wrongly. This script
# runs today only because /bin/sh is dash on the hosts its /proc reads require.
insns_of() { printf '%s\n' "$1" | sed -n 's/^ *instructions *: *//p' | tail -1 | tr -d '\r'; }

echo "== retired-instruction proxy (${LABEL})"
short=$(run_with_gap 1 "$EXTRA")
long=$(run_with_gap 5 "$EXTRA")
si=$(insns_of "$short")
li=$(insns_of "$long")
if [ -z "$si" ] || [ -z "$li" ]; then
    echo "   FAIL: --show-perf reported no instruction total"
    exit 1
fi
delta=$((li - si))
echo "   1s idle : ${si}"
echo "   5s idle : ${li}"
echo "   delta   : ${delta}   (bar: under 1000000)"
if [ "$delta" -lt 1000000 ]; then
    echo "   [PASS] the guest is parked, not spinning"
    PROXY_OK=1
else
    echo "   [RED]  the guest spins while idle"
    PROXY_OK=0
fi

# Core-equivalents at an idle prompt. The shell is held at its prompt for the sampling window
# with nothing typed, and utime+stime are read from /proc before and after.
echo ""
echo "== idle-prompt host CPU cost, core-equivalents (${LABEL})"
CLK=$(getconf CLK_TCK 2>/dev/null || echo 100)
SAMPLE=10
t=1
while [ "$t" -le "$TRIALS" ]; do
    # The fifo goes in TMPDIR rather than under the build tree: a Windows drive mounted into
    # WSL cannot host a named pipe, and a repository checked out on one is the ordinary case
    # for this project.
    fifo="${TMPDIR:-/tmp}/maize-idle-cost-fifo.$$.${t}"
    rm -f "$fifo"; mkfifo "$fifo"
    ( printf 'pwd\n'; sleep $((SAMPLE + 6)); printf 'exit\n'; sleep 0.5 ) > "$fifo" &
    feeder=$!
    # No `timeout` wrapper here, deliberately. timeout forks the real process, so $! would be
    # timeout's own pid and /proc/$!/stat would report timeout's CPU time, which is zero on
    # every build and would make this measurement read 0.0000 for a saturated spin as readily
    # as for a park. The feeder's own `exit` line is what ends the run; a watchdog below
    # bounds it if the guest wedges.
    "$MAIZE" --no-root $EXTRA \
        --mount "${BINDIR}=/bin:ro" --mount "${RWDIR}=/rw:rw" \
        --rom "$QUESOS" /bin/oksh.mzx < "$fifo" > /dev/null 2>&1 &
    vm=$!
    ( sleep 120; kill -9 "$vm" 2>/dev/null || true ) &
    watchdog=$!
    sleep 3                     # let the shell reach its prompt and the kernel reach idle
    read_cpu() { awk '{print $14 + $15}' "/proc/$1/stat" 2>/dev/null || echo ''; }
    c0=$(read_cpu "$vm"); w0=$(date +%s.%N)
    sleep "$SAMPLE"
    c1=$(read_cpu "$vm"); w1=$(date +%s.%N)
    if [ -z "$c0" ] || [ -z "$c1" ]; then
        echo "   trial ${t}: the VM was gone before the window closed"
    else
        awk -v c0="$c0" -v c1="$c1" -v w0="$w0" -v w1="$w1" -v clk="$CLK" -v t="$t" \
            'BEGIN { printf "   trial %d: %.4f core-equivalents (%.2f CPU-s over %.2f s)\n", \
                     t, ((c1-c0)/clk)/(w1-w0), (c1-c0)/clk, w1-w0 }'
    fi
    wait "$vm" 2>/dev/null || true
    kill "$watchdog" 2>/dev/null || true
    wait "$feeder" 2>/dev/null || true
    rm -f "$fifo"
    t=$((t + 1))
done

echo ""
[ "$PROXY_OK" -eq 1 ]
