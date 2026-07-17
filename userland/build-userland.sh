#!/bin/sh
# build-userland.sh (maize-94): compile the vendored wave-1 userland to .mzx binaries.
#
# The vendoring model (VENDORING.md): each project is a PRISTINE pinned submodule; every
# Maize-local change lives in a numbered userland/patches/<project>/ overlay. This script
# stages a scratch checkout of each submodule, applies its patch series in order, then
# compiles each program through scripts/cc-maize.sh (the single canonical C pipeline)
# using the program's userland/sources/<project>/<name>.list source list. Output is one
# <name>.mzx per program in the chosen --out dir (the wave-1 /bin set). Never edits the
# submodule in place, so a re-pin stays clean.
#
# Usage: userland/build-userland.sh [--preset <name>] --out <dir> [prog ...]
#   With no prog names, builds the full wave-1 set. `prog` is an sbase util name or
#   `oksh`. The sbase scratch also carries userland/include on the cpp path (the regex.h
#   shim util.h pulls in).
#
# Exit codes: 0 all built; 1 a build failed; 2 usage / setup failure.
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "${SCRIPT_DIR}/.." && pwd)
CC_MAIZE="${REPO_ROOT}/scripts/cc-maize.sh"
INCLUDE_DIR="${SCRIPT_DIR}/include"

die() { echo "build-userland.sh: $*" >&2; exit 2; }

UNAME=$(uname -s)
case "$UNAME" in
    Linux)  PRESET='linux-debug' ;;
    Darwin) PRESET='macos-debug' ;;
    MINGW*|MSYS*|CYGWIN*) PRESET='windows-llvm-mingw-debug' ;;
    *) die "unsupported platform: ${UNAME}" ;;
esac

OUT=""
PROGS=""
while [ $# -gt 0 ]; do
    case "$1" in
        --preset) PRESET="${2:-}"; shift 2 ;;
        --preset=*) PRESET="${1#--preset=}"; shift ;;
        --out) OUT="${2:-}"; shift 2 ;;
        --out=*) OUT="${1#--out=}"; shift ;;
        -*) die "unknown option: $1" ;;
        *) PROGS="${PROGS} $1"; shift ;;
    esac
done
[ -n "$OUT" ] || die "an output dir is required: --out <dir>"
mkdir -p "$OUT"

# Wave-1 default set (operator-confirmed, OQ 8949) when no programs are named.
SBASE_WAVE1="true false echo printf pwd cat cp mv rm ls ed"
if [ -z "$PROGS" ]; then PROGS="${SBASE_WAVE1} oksh"; fi

# Stage a pristine submodule into a scratch dir and apply its patch series in order.
# Idempotent per run (scratch is rebuilt each invocation).
stage_project() {
    _proj="$1"
    _sub="${SCRIPT_DIR}/${_proj}"
    _stage="${WORK}/${_proj}"
    [ -d "$_sub" ] || die "submodule not initialized: ${_sub} (git submodule update --init)"
    rm -rf "$_stage"
    cp -a "$_sub" "$_stage"
    if [ -d "${SCRIPT_DIR}/patches/${_proj}" ]; then
        for _p in "${SCRIPT_DIR}/patches/${_proj}"/*.patch; do
            [ -e "$_p" ] || continue
            if ! (cd "$_stage" && patch -p1 --forward --silent < "$_p"); then
                die "patch failed for ${_proj}: ${_p}"
            fi
        done
    fi
    # Copy the Maize-local shim headers (e.g. the regex.h util.h pulls in) INTO the
    # scratch checkout so cc-maize.sh's per-source `-I <source dir>` resolves the
    # angle-bracket includes without any RT-slice change or a -I passthrough.
    if [ -d "$INCLUDE_DIR" ]; then
        for _h in "$INCLUDE_DIR"/*.h; do
            [ -e "$_h" ] || continue
            cp "$_h" "$_stage/"
        done
    fi
    echo "$_stage"
}

WORK=$(mktemp -d)
cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT

SBASE_STAGE=$(stage_project sbase)
OKSH_STAGE=""

build_sbase_util() {
    _name="$1"
    _list="${SCRIPT_DIR}/sources/sbase/${_name}.list"
    [ -f "$_list" ] || die "no sources list for sbase/${_name}: ${_list}"
    # Prefix each list entry with the scratch sbase dir; skip blank/# lines.
    _srcs=""
    while IFS= read -r _line || [ -n "$_line" ]; do
        _line=$(printf '%s' "$_line" | tr -d '\r')
        case "$_line" in ''|\#*) continue ;; esac
        _srcs="${_srcs} ${SBASE_STAGE}/${_line}"
    done < "$_list"
    [ -n "$_srcs" ] || die "empty sources list: ${_list}"
    # cc-maize.sh -I's each source's own directory (the scratch sbase dir), which carries
    # sbase's own headers plus the copied shim headers, so angle-bracket includes resolve.
    # shellcheck disable=SC2086
    if ! "$CC_MAIZE" --preset "$PRESET" -o "${OUT}/${_name}.mzx" $_srcs; then
        echo "build-userland.sh: FAILED building sbase/${_name}" >&2
        return 1
    fi
    echo "built ${OUT}/${_name}.mzx"
}

rc=0
for prog in $PROGS; do
    if [ "$prog" = "oksh" ]; then
        [ -n "$OKSH_STAGE" ] || OKSH_STAGE=$(stage_project oksh)
        echo "build-userland.sh: oksh build not yet wired (Phase e)" >&2
        rc=1
        continue
    fi
    build_sbase_util "$prog" || rc=1
done
exit "$rc"
