#!/bin/sh
# demos/build-demos.sh (maize-248): compile the operator-facing demo guest programs
# (kilo, doom) to .mzx images through the SAME cproc -> qbe -> mazm -> mzld pipeline as
# scripts/cc-maize.sh, mirroring userland/build-userland.sh's batch-build shape.
#
# Each demo has its own documented standalone build command in its README (demos/kilo/
# README.md "Build", demos/doom/README.md "Build command"); this script generalizes those
# with --preset/--out so the whole demo set can be built into one destination in a single
# call. Output is one <name>.mzx per demo in the chosen --out dir.
#
# The demo IMAGES are all this script produces. It never supplies, downloads, generates,
# or references a DOOM WAD: that is the operator's own responsibility (see demos/doom/
# README.md). doom.mzx alone will not play until the operator provides a WAD at the
# documented location (~/.maize/root/home/user/doom/doom1.wad) or via --mount at run time.
#
# Usage: demos/build-demos.sh [--preset <name>] --out <dir> [demo ...]
#   With no demo names, builds the full v1 set (kilo doom). A demo name is one of
#   `kilo` or `doom`; any other name is an error.
#
# Exit codes: 0 all built; 1 a build failed; 2 usage / setup failure.
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "${SCRIPT_DIR}/.." && pwd)
CC_MAIZE="${REPO_ROOT}/scripts/cc-maize.sh"

die() { echo "build-demos.sh: $*" >&2; exit 2; }

# Fail LOUDLY if a build claims success but the produced image is missing, empty, or not
# a real .mzx (first 3 bytes must be the "MZX" magic). Mirrors build-userland.sh's own
# verify_mzx: cc-maize.sh already fails nonzero on a pipeline error, but this catches a
# silently-truncated or empty output before it reaches maize as an unloadable image.
verify_mzx() {
    _f="$1"
    if [ ! -f "$_f" ]; then echo "build-demos.sh: MISSING output ${_f}" >&2; return 1; fi
    _sz=$(wc -c < "$_f" 2>/dev/null | tr -d ' ')
    if [ -z "$_sz" ] || [ "$_sz" -lt 24 ]; then
        echo "build-demos.sh: output ${_f} too small (${_sz:-0} bytes, need >= 24-byte header)" >&2
        return 1
    fi
    _magic=$(dd if="$_f" bs=1 count=3 2>/dev/null)
    if [ "$_magic" != "MZX" ]; then
        echo "build-demos.sh: output ${_f} is not a .mzx image (bad magic)" >&2
        return 1
    fi
    return 0
}

# --- maize-263: WSL-native mirror + throttle, BEFORE arg parsing consumes "$@" so
#     --out and the demo list reach the mirrored child intact. --out is a caller-supplied
#     path resolved against the unchanged PWD (no cd), so the .mzx outputs still land where
#     the caller asked. This script computes no submodule-SHA cache key, so it needs no
#     maize_precompute_submodule_keys call (unlike build-userland.sh/build-toolchain.sh). -
. "${REPO_ROOT}/scripts/lib/harness-env.sh"
maize_apply_throttle
maize_native_mirror_run "$REPO_ROOT" "$SCRIPT_DIR" "$(basename "$0")" -- "$@"

UNAME=$(uname -s)
case "$UNAME" in
    Linux)  PRESET='linux-debug' ;;
    Darwin) PRESET='macos-debug' ;;
    MINGW*|MSYS*|CYGWIN*) PRESET='windows-llvm-mingw-debug' ;;
    *) die "unsupported platform: ${UNAME}" ;;
esac

OUT=""
DEMOS=""
while [ $# -gt 0 ]; do
    case "$1" in
        --preset) PRESET="${2:-}"; shift 2 ;;
        --preset=*) PRESET="${1#--preset=}"; shift ;;
        --out) OUT="${2:-}"; shift 2 ;;
        --out=*) OUT="${1#--out=}"; shift ;;
        -*) die "unknown option: $1" ;;
        *) DEMOS="${DEMOS} $1"; shift ;;
    esac
done
[ -n "$OUT" ] || die "an output dir is required: --out <dir>"
mkdir -p "$OUT"

# v1 demo set (decision 9482): kilo and doom, nothing else. No names given builds both.
if [ -z "$DEMOS" ]; then DEMOS="kilo doom"; fi

# Validate every requested demo up front (decision 9488), so an unknown name fails fast
# with a clear message before any build work happens.
for _demo in $DEMOS; do
    case "$_demo" in
        kilo|doom) ;;
        *) die "unknown demo: ${_demo} (known: kilo, doom)" ;;
    esac
done

# kilo: a single-source build straight through cc-maize.sh, per demos/kilo/README.md.
build_kilo() {
    if ! "$CC_MAIZE" --preset "$PRESET" -o "${OUT}/kilo.mzx" "${REPO_ROOT}/demos/kilo/kilo.c"; then
        echo "build-demos.sh: FAILED building kilo" >&2
        return 1
    fi
    verify_mzx "${OUT}/kilo.mzx" || return 1
    echo "built ${OUT}/kilo.mzx"
}

# doom: the whole doomgeneric + DOOM tree through cc-maize.sh --sources, per demos/doom/
# README.md's "Build command". Always built from source; never shortcuts by copying the
# committed demos/doom/doom.mzx. Checks the engine submodule is initialized first
# (decision 9488) and dies with the exact init command if not; never auto-inits it.
build_doom() {
    if [ ! -d "${REPO_ROOT}/demos/doom/doomgeneric/doomgeneric" ]; then
        die "submodule not initialized: demos/doom/doomgeneric (git submodule update --init demos/doom/doomgeneric)"
    fi
    if ! "$CC_MAIZE" --preset "$PRESET" --dev \
            -D DOOMGENERIC_RESX=320 -D DOOMGENERIC_RESY=200 \
            -o "${OUT}/doom.mzx" \
            --sources "${REPO_ROOT}/demos/doom/doom.sources" \
            "${REPO_ROOT}/demos/doom/doom_main.c" \
            "${REPO_ROOT}/demos/doom/doomgeneric_maize.c"; then
        echo "build-demos.sh: FAILED building doom" >&2
        return 1
    fi
    verify_mzx "${OUT}/doom.mzx" || return 1
    echo "built ${OUT}/doom.mzx"
}

rc=0
for demo in $DEMOS; do
    case "$demo" in
        kilo) build_kilo || rc=1 ;;
        doom) build_doom || rc=1 ;;
    esac
done
exit "$rc"
