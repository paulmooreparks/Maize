#!/bin/sh
# build-quesos.sh: link quesOS (the maize-24 keystone, Piece 3) into quesos.mzx.
#
# quesOS is guest C (quesos.c) plus the metal it needs (quesos_boot.mazm), built
# through the SAME cproc -> qbe -t maize -> mazm -c pipeline as cc-maize.sh, but with
# two deliberate departures from the stock C link:
#
#   1. A NON-DEFAULT link base (mzld -b, decision D8). quesOS links at 0x00100000 so
#      it does not collide with the 0x2000 children it execs into the shared flat
#      address space. hello.mzx-class children are ~31 KiB (well under 1 MiB), so
#      [0x2000, 0x100000) is ample and non-overlapping; verified against the actual
#      demo child sizes at build time (see the overlap note printed below).
#   2. A MINIMAL object set: quesos_boot.mzo (its own entry _start + cause-7 handler),
#      the raw syscall stubs (syscall.mzo, for quesOS's own native file I/O), and the
#      quesos.c body. NO crt0 (quesOS supplies its own entry), no libc (quesos.c
#      defines its own memcpy/memset and writes output through the raw stubs).
#
# Usage: os/quesos/build-quesos.sh [--preset <name>] -o <out.mzx>
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "${SCRIPT_DIR}/../.." && pwd)
RT_DIR="${REPO_ROOT}/toolchain/rt"
QBE_DIR="${REPO_ROOT}/toolchain/qbe"
CPROC_DIR="${REPO_ROOT}/toolchain/cproc"

# quesOS's reserved non-default link base (decision D8).
QUESOS_BASE="0x00100000"

die() { echo "build-quesos.sh: $*" >&2; exit 2; }

UNAME=$(uname -s)
case "$UNAME" in
    Linux)  PRESET='linux-debug' ;;
    Darwin) PRESET='macos-debug' ;;
    MINGW*|MSYS*|CYGWIN*) PRESET='windows-llvm-mingw-debug' ;;
    *) die "unsupported platform: ${UNAME}" ;;
esac

OUT=""
while [ $# -gt 0 ]; do
    case "$1" in
        --preset) PRESET="${2:-}"; shift 2 ;;
        --preset=*) PRESET="${1#--preset=}"; shift ;;
        -o) OUT="${2:-}"; shift 2 ;;
        -o*) OUT="${1#-o}"; shift ;;
        *) die "unknown argument: $1" ;;
    esac
done
[ -n "$OUT" ] || die "an output path is required: -o <out.mzx>"

BUILD_DIR="${REPO_ROOT}/build/${PRESET}"

resolve_exe() {
    if [ -x "$1" ] || [ -f "$1" ]; then echo "$1"; return 0; fi
    if [ -x "$1.exe" ] || [ -f "$1.exe" ]; then echo "$1.exe"; return 0; fi
    return 1
}

CPROC_QBE=$(resolve_exe "${CPROC_DIR}/cproc-qbe") || die "cproc-qbe not found; run scripts/build-toolchain.sh"
QBE=$(resolve_exe "${QBE_DIR}/obj/qbe")           || die "qbe not found; run scripts/build-toolchain.sh"
MAZM=$(resolve_exe "${BUILD_DIR}/mazm")           || die "mazm not found in ${BUILD_DIR}; run scripts/run-tests.sh first."
MZLD=$(resolve_exe "${BUILD_DIR}/mzld")           || die "mzld not found in ${BUILD_DIR}; run scripts/run-tests.sh first."

CPP="${CC:-}"
if [ -z "$CPP" ]; then
    if command -v cc >/dev/null 2>&1; then CPP=cc; else CPP=gcc; fi
fi
command -v "$CPP" >/dev/null 2>&1 || die "no C preprocessor (cc/gcc) found."

WORK=$(mktemp -d)
cleanup() { _rc=$?; rm -rf "$WORK"; exit "$_rc"; }
trap cleanup EXIT

# --- Compile quesos.c to a .mzo through the exact cc-maize.sh C pipeline. ----------
SRC="${SCRIPT_DIR}/quesos.c"
tr -d '\r' < "$SRC" > "${WORK}/quesos.lf.c"
if ! "$CPP" -E -P -nostdinc -D '__attribute__(x)=' -I "$RT_DIR" -I "$SCRIPT_DIR" \
        "${WORK}/quesos.lf.c" > "${WORK}/quesos.pp.c" 2>"${WORK}/quesos.cpp.log"; then
    echo "cpp failed:" >&2; cat "${WORK}/quesos.cpp.log" >&2; exit 1
fi
if ! "$CPROC_QBE" < "${WORK}/quesos.pp.c" > "${WORK}/quesos.ssa" 2>"${WORK}/quesos.cproc.log"; then
    echo "cproc-qbe failed:" >&2; cat "${WORK}/quesos.cproc.log" >&2; exit 1
fi
# The ONE normalize sed (mirrors cc-maize.sh): strip `extern $` and lower `neg`.
sed -e 's/extern \$/$/g' \
    -e 's/\(=[wl]\) neg /\1 sub 0, /' "${WORK}/quesos.ssa" > "${WORK}/quesos.norm.ssa"
if ! "$QBE" -t maize "${WORK}/quesos.norm.ssa" > "${WORK}/quesos.body.mazm" 2>"${WORK}/quesos.qbe.log"; then
    echo "qbe -t maize failed:" >&2; cat "${WORK}/quesos.qbe.log" >&2; exit 1
fi
if ! "$MAZM" -c "${WORK}/quesos.body.mazm" >"${WORK}/quesos.mazm.log" 2>&1 \
   || [ ! -f "${WORK}/quesos.body.mzo" ]; then
    echo "mazm -c (quesos.c body) failed:" >&2; cat "${WORK}/quesos.mazm.log" >&2; exit 1
fi

# --- Assemble the metal + the raw syscall stubs to .mzo (copy in: mazm -c writes
#     beside its input). ----------------------------------------------------------
cp "${SCRIPT_DIR}/quesos_boot.mazm" "${WORK}/quesos_boot.mazm"
if ! "$MAZM" -c "${WORK}/quesos_boot.mazm" >"${WORK}/quesos_boot.mazm.log" 2>&1 \
   || [ ! -f "${WORK}/quesos_boot.mzo" ]; then
    echo "mazm -c (quesos_boot.mazm) failed:" >&2; cat "${WORK}/quesos_boot.mazm.log" >&2; exit 1
fi
cp "${RT_DIR}/syscall.mazm" "${WORK}/syscall.mazm"
if ! "$MAZM" -c "${WORK}/syscall.mazm" >"${WORK}/syscall.mazm.log" 2>&1 \
   || [ ! -f "${WORK}/syscall.mzo" ]; then
    echo "mazm -c (syscall.mazm) failed:" >&2; cat "${WORK}/syscall.mazm.log" >&2; exit 1
fi

# --- Link at the non-default base. Entry is _start (mzld default), defined by
#     quesos_boot.mzo. -------------------------------------------------------------
out_dir=$(dirname "$OUT"); [ -d "$out_dir" ] || mkdir -p "$out_dir"
if ! "$MZLD" -b "$QUESOS_BASE" -o "$OUT" \
        "${WORK}/quesos_boot.mzo" "${WORK}/syscall.mzo" "${WORK}/quesos.body.mzo" \
        >"${WORK}/quesos.mzld.log" 2>&1 || [ ! -f "$OUT" ]; then
    echo "mzld failed:" >&2; cat "${WORK}/quesos.mzld.log" >&2; exit 1
fi

echo "build-quesos.sh: linked $(wc -c <"$OUT" | tr -d ' ') bytes -> ${OUT} (base ${QUESOS_BASE})"
