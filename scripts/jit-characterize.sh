#!/usr/bin/env bash
# Repeatable JIT characterization (maize-343's measurement, promoted to a tracked
# harness). Builds the VM, the deterministic DOOM bench (bare + quesOS-hosted),
# quesOS, and two micro-benches for one CMake preset, then runs the four-mode
# DOOM matrix (bare/paged x interp/JIT), the syscall-vs-compute micro legs, and
# an optional native doomgeneric reference, and emits one markdown report with
# medians and cross-mode ratios.
#
# Linux/WSL only (the presets and cc-maize.sh pipeline are POSIX here). Typical:
#   scripts/jit-characterize.sh --preset linux-release --out /tmp/jit-report.md
#
# The same harness run on a future VM (or on Maize v2) gives apples-to-apples
# numbers, which is the point of tracking it.

set -u

PRESET="linux-release"
FRAMES=240
TRIALS=3
NATIVE=1
OUT=""

while [ $# -gt 0 ]; do
    case "$1" in
        --preset)      PRESET="$2"; shift 2 ;;
        --frames)      FRAMES="$2"; shift 2 ;;
        --trials)      TRIALS="$2"; shift 2 ;;
        --skip-native) NATIVE=0; shift ;;
        --out)         OUT="$2"; shift 2 ;;
        *) echo "usage: $0 [--preset P] [--frames N] [--trials N] [--skip-native] [--out FILE]" >&2; exit 2 ;;
    esac
done

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT" || exit 1

M="build/$PRESET/maize"
WORK="$(mktemp -d /tmp/jitchar.XXXXXX)"
[ -n "$OUT" ] || OUT="$WORK/report.md"
LOG="$WORK/build.log"
echo "workdir: $WORK"
echo "report:  $OUT"

step() { echo "=== [$(date +%H:%M:%S)] $*"; }

# ---- Stage 1: the VM ---------------------------------------------------------
step "configure + build VM ($PRESET)"
cmake --preset "$PRESET" >>"$LOG" 2>&1 || { echo "configure failed; see $LOG" >&2; exit 1; }
cmake --build "build/$PRESET" --target maize >>"$LOG" 2>&1 || { echo "VM build failed; see $LOG" >&2; exit 1; }
[ -x "$M" ] || { echo "missing $M" >&2; exit 1; }

# ---- Stage 2: the synthetic IWAD (same generator run-ctest.sh uses) ----------
step "generate min.wad"
GEN_CC="${CC:-}"
if [ -z "$GEN_CC" ]; then
    if command -v cc >/dev/null 2>&1; then GEN_CC=cc; else GEN_CC=gcc; fi
fi
"$GEN_CC" -O2 -o "$WORK/make_min_iwad" demos/doom/tools/make_min_iwad.c >>"$LOG" 2>&1 \
    || { echo "IWAD generator build failed; see $LOG" >&2; exit 1; }
"$WORK/make_min_iwad" "$WORK/min.wad" >>"$LOG" 2>&1
[ -f "$WORK/min.wad" ] || { echo "min.wad generation failed" >&2; exit 1; }

# ---- Stage 3: guest images ---------------------------------------------------
step "build doom_bench.mzx (bare) + doom_bench_q.mzx (quesOS-hosted)"
scripts/cc-maize.sh --preset "$PRESET" --dev \
    -D DOOMGENERIC_RESX=320 -D DOOMGENERIC_RESY=200 \
    -D BENCH_DETERMINISTIC -D "BENCH_FRAMES=$FRAMES" \
    -o "$WORK/doom.mzx" \
    --sources demos/doom/doom.sources \
    demos/doom/doom_bench.c demos/doom/doomgeneric_maize.c >>"$LOG" 2>&1 \
    || { echo "doom_bench build failed; see $LOG" >&2; exit 1; }
scripts/cc-maize.sh --preset "$PRESET" --dev \
    -D DOOMGENERIC_RESX=320 -D DOOMGENERIC_RESY=200 \
    -D BENCH_DETERMINISTIC -D "BENCH_FRAMES=$FRAMES" \
    -o "$WORK/doomq.mzx" \
    --sources demos/doom/doom.sources \
    demos/doom/doom_bench_quesos.c demos/doom/doomgeneric_maize.c >>"$LOG" 2>&1 \
    || { echo "doom_bench_quesos build failed; see $LOG" >&2; exit 1; }

step "build quesos.mzx ($PRESET)"
os/quesos/build-quesos.sh --preset "$PRESET" -o "$WORK/quesos.mzx" >>"$LOG" 2>&1 \
    || { echo "quesos build failed; see $LOG" >&2; exit 1; }

step "build micro-benches (scloop, compute)"
scripts/cc-maize.sh --preset "$PRESET" --dev -o "$WORK/scloop.mzx"  "$SCRIPT_DIR/bench/scloop.c"  >>"$LOG" 2>&1 \
    || { echo "scloop build failed; see $LOG" >&2; exit 1; }
scripts/cc-maize.sh --preset "$PRESET" --dev -o "$WORK/compute.mzx" "$SCRIPT_DIR/bench/compute.c" >>"$LOG" 2>&1 \
    || { echo "compute build failed; see $LOG" >&2; exit 1; }

# Shared read-only mount dir for the guest runs.
MNT="$WORK/mnt"
mkdir -p "$MNT"
cp "$WORK/doom.mzx" "$WORK/doomq.mzx" "$WORK/min.wad" "$MNT/"

# ---- Stage 4: native reference (optional) ------------------------------------
NATIVE_US=""
if [ "$NATIVE" -eq 1 ] && command -v gcc >/dev/null 2>&1; then
    step "build + run native doomgeneric reference"
    DG=demos/doom/doomgeneric/doomgeneric
    SRCS=""
    while read -r line; do
        case "$line" in
            ''|\#*) continue ;;
            demos/doom/doom_r_draw.c)  SRCS="$SRCS $DG/r_draw.c" ;;
            demos/doom/doom_i_video.c) SRCS="$SRCS $DG/i_video.c" ;;
            *) SRCS="$SRCS $line" ;;
        esac
    done < demos/doom/doom.sources
    if gcc -O2 -w -std=gnu11 \
        -D DOOMGENERIC_RESX=320 -D DOOMGENERIC_RESY=200 -D "BENCH_FRAMES=$FRAMES" \
        -I "$DG" $SRCS "$SCRIPT_DIR/bench/dg_null.c" -lm -o "$WORK/doom_native" >>"$LOG" 2>&1; then
        best=""
        for r in 1 2 3; do
            us=$("$WORK/doom_native" -iwad "$MNT/min.wad" -warp 1 1 -nomonsters 2>/dev/null \
                 | grep -oE '[0-9]+(\.[0-9]+)? us/frame' | grep -oE '^[0-9]+(\.[0-9]+)?' | head -1)
            [ -n "$us" ] && { [ -z "$best" ] || awk "BEGIN{exit !($us < $best)}"; } && best="$us"
        done
        NATIVE_US="$best"
    else
        echo "note: native reference build failed (see $LOG); continuing without it" >&2
    fi
fi

# ---- Stage 5: the runs -------------------------------------------------------
# Each run leaves raw out/err in $WORK; the extractor pulls us/frame, MIPS,
# and JIT coverage. Medians over $TRIALS.
run_mode() {  # tag, then the full command
    local tag="$1"; shift
    local r
    for r in $(seq 1 "$TRIALS"); do
        local f="$WORK/${tag}_${r}"
        timeout 900 "$@" >"$f.out" 2>"$f.err"
        local us mips cov
        us=$(grep -h 'bench:' "$f.out" "$f.err" 2>/dev/null | grep -oE '[0-9]+(\.[0-9]+)? us/frame' | grep -oE '^[0-9]+(\.[0-9]+)?' | head -1)
        mips=$(grep 'MIPS' "$f.err" 2>/dev/null | head -1 | grep -oE '[0-9]+ avg' | grep -oE '[0-9]+')
        cov=$(grep 'covered fraction' "$f.err" 2>/dev/null | grep -oE '[0-9.]+%' | head -1)
        echo "$tag $r ${us:-na} ${mips:-na} ${cov:-na}" >>"$WORK/rows.txt"
        echo "    [$tag r$r] us/frame=${us:-na} mips=${mips:-na} covered=${cov:-na}"
    done
}

step "DOOM matrix ($TRIALS trials each)"
run_mode a_bare_interp "$M" --bare --no-jit --show-perf --no-root --mount "$MNT=/ro:ro" "$MNT/doom.mzx" -iwad /ro/min.wad -warp 1 1 -nomonsters
run_mode b_bare_jit    "$M" --bare --jit --jit-report --show-perf --no-root --mount "$MNT=/ro:ro" "$MNT/doom.mzx" -iwad /ro/min.wad -warp 1 1 -nomonsters
run_mode c_ques_interp "$M" --rom "$WORK/quesos.mzx" --no-jit --show-perf --no-root --fb-no-display --mount "$MNT=/ro:ro" /ro/doomq.mzx
run_mode d_ques_jit    "$M" --rom "$WORK/quesos.mzx" --jit --jit-report --show-perf --no-root --fb-no-display --mount "$MNT=/ro:ro" /ro/doomq.mzx

step "micro legs (syscall-bound vs compute-bound)"
run_mode scloop_interp  "$M" --bare --no-jit --show-perf "$WORK/scloop.mzx"
run_mode scloop_jit     "$M" --bare --jit --jit-report --show-perf "$WORK/scloop.mzx"
run_mode compute_interp "$M" --bare --no-jit --show-perf "$WORK/compute.mzx"
run_mode compute_jit    "$M" --bare --jit --jit-report --show-perf "$WORK/compute.mzx"

# ---- Stage 6: the report -----------------------------------------------------
step "write report"
median() {  # tag, column (3=us, 4=mips)
    awk -v t="$1" -v c="$2" '$1==t && $c!="na" {v[n++]=$c}
        END{ if(n==0){print "na"; exit}
             for(i=0;i<n;i++)for(j=i+1;j<n;j++)if(v[j]+0<v[i]+0){x=v[i];v[i]=v[j];v[j]=x}
             print v[int(n/2)] }' "$WORK/rows.txt"
}
cov_of() { awk -v t="$1" '$1==t && $5!="na"{print $5; exit}' "$WORK/rows.txt"; }
ratio() {  # a/b with 1 decimal, na-safe
    awk -v a="$1" -v b="$2" 'BEGIN{ if(a=="na"||b=="na"||b+0==0){print "na"} else {printf "%.1f", a/b} }'
}

A_US=$(median a_bare_interp 3);  A_MI=$(median a_bare_interp 4)
B_US=$(median b_bare_jit 3);     B_MI=$(median b_bare_jit 4);     B_CV=$(cov_of b_bare_jit)
C_US=$(median c_ques_interp 3);  C_MI=$(median c_ques_interp 4)
D_US=$(median d_ques_jit 3);     D_MI=$(median d_ques_jit 4);     D_CV=$(cov_of d_ques_jit)
SI=$(median scloop_interp 4);    SJ=$(median scloop_jit 4);       SJC=$(cov_of scloop_jit)
CI=$(median compute_interp 4);   CJ=$(median compute_jit 4);      CJC=$(cov_of compute_jit)

{
    echo "# JIT characterization: $PRESET"
    echo
    echo "Generated $(date -u +%Y-%m-%dT%H:%M:%SZ) at git $(git rev-parse --short HEAD) on $(uname -sm), $(grep -m1 'model name' /proc/cpuinfo 2>/dev/null | cut -d: -f2 | sed 's/^ //')."
    echo "Deterministic doom_bench, $FRAMES frames, min.wad -warp 1 1 -nomonsters, median of $TRIALS trials."
    echo
    echo "## DOOM matrix"
    echo
    echo "| Mode | us/frame | MIPS | JIT covered |"
    echo "|---|---|---|---|"
    echo "| bare-flat interp | $A_US | $A_MI | n/a |"
    echo "| bare-flat JIT | $B_US | $B_MI | ${B_CV:-na} |"
    echo "| quesOS-paged interp | $C_US | $C_MI | n/a |"
    echo "| quesOS-paged JIT | $D_US | $D_MI | ${D_CV:-na} |"
    echo
    echo "## Ratios"
    echo
    echo "- JIT speedup, bare: $(ratio "$A_US" "$B_US")x on us/frame"
    echo "- JIT speedup, paged: $(ratio "$C_US" "$D_US")x on us/frame"
    echo "- Paging tax under JIT (paged/bare us/frame): $(ratio "$D_US" "$B_US")x"
    echo "- Paging tax interpreted: $(ratio "$C_US" "$A_US")x"
    if [ -n "$NATIVE_US" ]; then
        echo "- Native doomgeneric reference: $NATIVE_US us/frame; bare JIT is $(ratio "$B_US" "$NATIVE_US")x native, paged JIT $(ratio "$D_US" "$NATIVE_US")x"
    else
        echo "- Native reference: skipped or unavailable"
    fi
    echo
    echo "## Micro legs (MIPS, bare-flat)"
    echo
    echo "| Program | interp | JIT | JIT covered |"
    echo "|---|---|---|---|"
    echo "| scloop (syscall-bound) | $SI | $SJ | ${SJC:-na} |"
    echo "| compute (no syscalls) | $CI | $CJ | ${CJC:-na} |"
    echo
    echo "Raw rows in $WORK/rows.txt; per-run logs beside them. The paged-vs-bare"
    echo "attribution (fast-page miss slow path plus the armed quesOS timer) is on"
    echo "the maize-354 record and applies to whatever ratio appears above."
} >"$OUT"

echo
cat "$OUT"
echo
echo "report written to $OUT"
