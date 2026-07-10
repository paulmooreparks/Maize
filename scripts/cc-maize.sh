#!/bin/sh
# cc-maize.sh: the SINGLE canonical C compile-and-run driver for the Maize C
# toolchain (maize-96). This is the one place the whole C pipeline is defined:
#
#   tr -d '\r'                (defense-in-depth CRLF strip; no-op on a clean LF tree)
#     -> cpp -E -P -nostdinc -I toolchain/rt   (expand #include; cproc-qbe has none)
#     -> cproc-qbe            (C11 -> QBE IL)
#     -> normalize            (drop the `extern` linkage annotation; lower `neg`)
#     -> qbe -t maize         (QBE IL -> mazm body, with SECTION/GLOBAL/ALIGN/DREF)
#     -> mazm -c              (body.mazm -> body.mzo relocatable object)
#     -> mzld                 (crt0 syscall puts errno body -> <base>.mzx; entry _start;
#                              W^X, per-section alignment)
#     -> maize                (load_mzx sets RP=_start; execute; propagate guest exit)
#
# BOTH consumers of the C pipeline call this driver, so CI (scripts/run-ctest.sh,
# via --compile-only) exercises the EXACT pipeline the operator acceptance-tests
# with (the ~/bin/maize-cc forwarder execs this file). The normalize sed, the cpp
# flags, the RT object set, and the mzld link order therefore live in EXACTLY ONE
# place: here. Drift between CI and the operator's tool is structurally impossible.
#
# `extern` normalization: the pinned cproc (d1c53dd) tags EVERY external-symbol
# operand `extern` (calls AND loads/adds of file-scope globals), a linkage
# annotation the pinned qbe (4420727) predates and rejects. In the Maize linked-
# image model every `$sym` resolves through mzld regardless, so the annotation
# carries no meaning and is stripped deterministically. `neg X` is the unary op the
# pinned qbe's parser predates; `sub 0, X` is the identity lowering (same class /
# semantics).
#
# Modes:
#   cc-maize.sh [--preset <name>] <file.c>
#       compile + link + run, propagating the guest exit code.
#   cc-maize.sh [--preset <name>] --emit <file.c>
#       as default, plus drop <base>.mazm (the qbe body) and <base>.mzx (the linked
#       image) beside the source; all other intermediates stay in a scratch dir.
#   cc-maize.sh [--preset <name>] --compile-only -o <path> <file.c>
#       produce the linked <path>.mzx and exit WITHOUT running (run-ctest's mode).
#   cc-maize.sh --build
#       (re)build the vendored cproc/qbe toolchain, then exit.
#
# A source given as a Windows path (C:\... or C:/...) is translated with wslpath, so
# the tool works from a Windows shell forwarder as well as from a WSL/Linux shell.
#
# cproc is strict C11: declare any libc-style function you call (e.g.
# `int puts(const char *);`); the declaration only satisfies the front-end, the
# symbol still resolves to the runtime's definition in mazm's shared label table.
#
# Exit codes: the guest's exit code in run mode; 0 on success in --compile-only /
# --emit-without-run paths; 2 for a usage or environment/setup failure; a nonzero
# pipeline-stage failure is reported with context and propagated.
set -eu

# --- Paths resolved relative to THIS script, not the caller's CWD ----------------
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "${SCRIPT_DIR}/.." && pwd)
RT_DIR="${REPO_ROOT}/toolchain/rt"
QBE_DIR="${REPO_ROOT}/toolchain/qbe"
CPROC_DIR="${REPO_ROOT}/toolchain/cproc"

die() { echo "cc-maize.sh: $*" >&2; exit 2; }

# --- Default build preset, mirroring run-ctest.sh's platform switch ---------------
UNAME=$(uname -s)
case "$UNAME" in
    Linux)  DEFAULT_PRESET='linux-debug' ;;
    Darwin) DEFAULT_PRESET='macos-debug' ;;
    MINGW*|MSYS*|CYGWIN*) DEFAULT_PRESET='windows-llvm-mingw-debug' ;;
    *) die "unsupported platform: ${UNAME}" ;;
esac

# --- Parse arguments (flags accepted in any position) ----------------------------
PRESET="$DEFAULT_PRESET"
MODE="run"          # run | compile-only | build
EMIT=0
OUT=""
SRC=""
while [ $# -gt 0 ]; do
    case "$1" in
        --build) MODE="build"; shift ;;
        --emit) EMIT=1; shift ;;
        --compile-only) MODE="compile-only"; shift ;;
        -o) OUT="${2:-}"; shift 2 ;;
        -o*) OUT="${1#-o}"; shift ;;
        --preset) PRESET="${2:-}"; shift 2 ;;
        --preset=*) PRESET="${1#--preset=}"; shift ;;
        --) shift; [ $# -gt 0 ] && { SRC="$1"; shift; } ;;
        -*) die "unknown option: $1" ;;
        *) SRC="$1"; shift ;;
    esac
done

# --- Toolchain build passthrough --------------------------------------------------
if [ "$MODE" = "build" ]; then
    exec "${SCRIPT_DIR}/build-toolchain.sh"
fi

[ -n "$SRC" ] || die "usage: cc-maize.sh [--preset <name>] <file.c> [--emit] | --compile-only -o <path> <file.c> | --build"
[ "$MODE" = "compile-only" ] && [ -z "$OUT" ] && die "--compile-only requires -o <path>"

# Accept a Windows path (C:\... or C:/...) as well as a WSL/Linux path.
case "$SRC" in
    *:\\*|[A-Za-z]:/*) SRC=$(wslpath "$SRC") ;;
esac
[ -f "$SRC" ] || die "no such file: $SRC"

BUILD_DIR="${REPO_ROOT}/build/${PRESET}"

# Resolve an executable path, tolerating a .exe suffix on Windows.
resolve_exe() {
    if [ -x "$1" ] || [ -f "$1" ]; then echo "$1"; return 0; fi
    if [ -x "$1.exe" ] || [ -f "$1.exe" ]; then echo "$1.exe"; return 0; fi
    return 1
}

CPROC_QBE=$(resolve_exe "${CPROC_DIR}/cproc-qbe") \
    || die "cproc-qbe not found; run 'cc-maize.sh --build' (cproc/qbe)."
QBE=$(resolve_exe "${QBE_DIR}/obj/qbe") \
    || die "qbe not found; run 'cc-maize.sh --build' (cproc/qbe)."
MAZM=$(resolve_exe "${BUILD_DIR}/mazm") \
    || die "mazm not found in ${BUILD_DIR}; run scripts/install-mazm.sh (or run-tests.sh) first."
MAIZE=$(resolve_exe "${BUILD_DIR}/maize") \
    || die "maize not found in ${BUILD_DIR}; run scripts/install-mazm.sh (or run-tests.sh) first."
MZLD=$(resolve_exe "${BUILD_DIR}/mzld") \
    || die "mzld not found in ${BUILD_DIR}; run scripts/install-mazm.sh (or run-tests.sh) first."

# System preprocessor for #include expansion (maize-74). cproc-qbe's own front-end
# does not implement #include, so a source (or the errno.c runtime) that includes
# toolchain/rt/syscall.h is preprocessed by the system cc/gcc first. Mirror
# build-toolchain.sh's compiler pick.
CPP="${CC:-}"
if [ -z "$CPP" ]; then
    if command -v cc >/dev/null 2>&1; then CPP=cc; else CPP=gcc; fi
fi
command -v "$CPP" >/dev/null 2>&1 \
    || die "no C preprocessor (cc/gcc) found for #include expansion."

# Everything intermediate goes in a scratch dir so the source tree never sees .mzo
# clutter, even with --emit (only the body .mazm and the linked .mzx are copied out).
WORK=$(mktemp -d)
# Clean the scratch dir on exit WHILE PRESERVING the exit status. In dash (the usual
# /bin/sh), the EXIT trap's last command status becomes the script's exit status, so a
# plain `rm` would clobber a guest's nonzero exit (e.g. main returning 42) back to 0.
# Capture $? first and re-exit with it so the guest exit code always propagates.
cleanup() { _rc=$?; rm -rf "$WORK"; exit "$_rc"; }
trap cleanup EXIT

# Compile one C translation unit through the full segmented pipeline to a .mzo:
#   tr -d '\r' -> cpp -E -P -nostdinc -I toolchain/rt -> cproc-qbe -> normalize
#     -> qbe -t maize -> mazm -c
# $1 = source path, $2 = tag for intermediates. Echoes the emitted <tag>.body.mzo on
# success; on failure prints stage context to stderr and returns 1. Shared by the
# body compile and the errno.c runtime compile so both take the identical path.
compile_tu() {
    _src="$1"
    _tag="$2"
    _lf="${WORK}/${_tag}.lf.c"
    _pp="${WORK}/${_tag}.pp.c"
    _ssa="${WORK}/${_tag}.ssa"
    _norm="${WORK}/${_tag}.norm.ssa"
    _body="${WORK}/${_tag}.body.mazm"
    _mzo="${WORK}/${_tag}.body.mzo"

    # Defense in depth for CRLF checkouts (belt-and-suspenders with .gitattributes
    # eol=lf): cproc is strict C11 and treats a bare CR as a stray token. A clean LF
    # checkout makes this a no-op. (maize-62)
    tr -d '\r' < "$_src" > "$_lf"

    # Expand #include / include guards with the system preprocessor. -nostdinc keeps
    # the search to toolchain/rt (freestanding: no system headers); -P drops GNU line
    # markers (cproc-qbe tolerates them, so -P is just for cleanliness).
    if ! "$CPP" -E -P -nostdinc -I "$RT_DIR" "$_lf" > "$_pp" 2>"${WORK}/${_tag}.cpp.log"; then
        echo "cc-maize.sh: cpp failed for ${_tag}" >&2; cat "${WORK}/${_tag}.cpp.log" >&2; return 1
    fi
    if ! "$CPROC_QBE" < "$_pp" > "$_ssa" 2>"${WORK}/${_tag}.cproc.log"; then
        echo "cc-maize.sh: cproc-qbe failed for ${_tag}" >&2; cat "${WORK}/${_tag}.cproc.log" >&2; return 1
    fi
    # Normalize two IL-version-skew points between the pinned cproc and pinned qbe
    # (see the header comment): strip `extern $` and lower `neg` to `sub 0, `.
    # THIS IS THE ONE NORMALIZE SED IN THE TREE (maize-96).
    sed -e 's/extern \$/$/g' \
        -e 's/\(=[wl]\) neg /\1 sub 0, /' "$_ssa" > "$_norm"
    if ! "$QBE" -t maize "$_norm" > "$_body" 2>"${WORK}/${_tag}.qbe.log"; then
        echo "cc-maize.sh: qbe -t maize failed for ${_tag}" >&2; cat "${WORK}/${_tag}.qbe.log" >&2; return 1
    fi
    if ! "$MAZM" -c "$_body" >"${WORK}/${_tag}.mazm.log" 2>&1 || [ ! -f "$_mzo" ]; then
        echo "cc-maize.sh: mazm -c failed for ${_tag}" >&2; cat "${WORK}/${_tag}.mazm.log" >&2; return 1
    fi
    echo "$_mzo"
    return 0
}

# Assemble the freestanding asm runtime as relocatable objects (maize-77 decision
# 7168): crt0/syscall/puts each become a .mzo. mazm -c writes <input>.mzo beside its
# input, so the sources are copied into WORK first (keeping toolchain/rt clean).
RT_OBJS=""
for rt in crt0 syscall puts; do
    cp "${RT_DIR}/${rt}.mazm" "${WORK}/${rt}.mazm"
    if ! "$MAZM" -c "${WORK}/${rt}.mazm" >"${WORK}/${rt}.mazm.log" 2>&1 || [ ! -f "${WORK}/${rt}.mzo" ]; then
        echo "cc-maize.sh: failed to assemble runtime object ${rt}.mazm:" >&2
        cat "${WORK}/${rt}.mazm.log" >&2
        exit 2
    fi
    RT_OBJS="${RT_OBJS} ${WORK}/${rt}.mzo"
done

# maize-74: errno storage + the errno-translating read/write wrappers are a C runtime
# source, not asm. Compile it through the same segmented C pipeline and add its object
# to the RT set, so every C image links errno + the wrappers alongside crt0/syscall/puts.
ERRNO_MZO=$(compile_tu "${RT_DIR}/errno.c" "rt_errno") \
    || die "failed to compile C runtime object errno.c"
RT_OBJS="${RT_OBJS} ${ERRNO_MZO}"

# Compile the user body.
base=$(basename "${SRC%.c}")
BODY_MZO=$(compile_tu "$SRC" "$base") || exit 1

# Link crt0 syscall puts errno body -> <base>.mzx (default entry _start; W^X +
# per-section alignment via mzld). RT_OBJS already carries the crt0 syscall puts errno
# order; the body goes last.
MZX="${WORK}/${base}.mzx"
if ! "$MZLD" -o "$MZX" ${RT_OBJS} "$BODY_MZO" >"${WORK}/${base}.mzld.log" 2>&1 || [ ! -f "$MZX" ]; then
    echo "cc-maize.sh: mzld failed for ${base}" >&2; cat "${WORK}/${base}.mzld.log" >&2; exit 1
fi

# --- Emit intermediates beside the source, when asked -----------------------------
if [ "$EMIT" -eq 1 ]; then
    dst=$(dirname "$SRC")
    cp "${WORK}/${base}.body.mazm" "${dst}/${base}.mazm"
    cp "$MZX" "${dst}/${base}.mzx"
    echo "cc-maize.sh: emitted ${dst}/${base}.mazm (qbe body) and ${dst}/${base}.mzx (linked image)" >&2
fi

# --- Compile-only: copy the image to -o <path> and stop (no run) ------------------
if [ "$MODE" = "compile-only" ]; then
    out_dir=$(dirname "$OUT")
    [ -d "$out_dir" ] || mkdir -p "$out_dir"
    cp "$MZX" "$OUT"
    exit 0
fi

# --- Default: run, propagating the guest exit code --------------------------------
# `exec` cannot be used (the EXIT trap must still clean WORK). Capture the guest status
# and `exit` with it: that sets $? for the EXIT trap, and cleanup() re-exits with that
# captured status, so the guest exit code propagates unchanged.
set +e
"$MAIZE" "$MZX"
rc=$?
set -e
exit "$rc"
