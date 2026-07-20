#!/bin/sh
# build-world.sh (maize-258): build the whole Maize world with one command.
#
# Composes the existing per-piece build scripts, in order: submodule init, native
# binaries + C toolchain (install-mazm.sh), quesOS (os/quesos/build-quesos.sh), the
# wave-1 userland (userland/build-userland.sh), and the demos (demos/build-demos.sh).
# No build logic is duplicated here; this script only sequences the existing entry
# points, checks each one's exit code, and prints a stage banner plus a final
# artifact/timing summary.
#
# One preset is pinned end to end and passed explicitly to every composed call, so a
# bare invocation is always internally coherent: every stage resolves tools from the
# SAME build directory install-mazm.sh just populated, even though the composed
# scripts' own standalone defaults differ by platform (build-quesos.sh /
# build-userland.sh / build-demos.sh each resolve their own per-platform default
# preset independently when --preset is omitted).
#
# This is the documented "I pulled, now what" answer; run it after a fresh clone or
# pull to build everything in one call.
#
# Usage: scripts/build-world.sh [--preset <name>] [--install-dir <dir>]
#                                [--userland-out <dir>] [--demos-out <dir>]
#                                [--quesos-out <path>]
#
# Exit codes: 0 all stages succeeded; nonzero the failing stage's own exit code
# (later stages do not run).
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "${SCRIPT_DIR}/.." && pwd)

die() { echo "build-world.sh: $*" >&2; exit 2; }

UNAME=$(uname -s)
case "$UNAME" in
    Linux)  PRESET='linux-release' ;;
    Darwin) PRESET='macos-release' ;;
    *) die "unsupported platform: ${UNAME}" ;;
esac

INSTALL_DIR="${HOME}/bin"
USERLAND_OUT="${HOME}/.maize/root/bin"
DEMOS_OUT="${HOME}/.maize/root/bin"
QUESOS_OUT="${REPO_ROOT}/os/quesos/quesos.mzx"

while [ $# -gt 0 ]; do
    case "$1" in
        --preset) PRESET="${2:-}"; shift 2 ;;
        --preset=*) PRESET="${1#--preset=}"; shift ;;
        --install-dir) INSTALL_DIR="${2:-}"; shift 2 ;;
        --install-dir=*) INSTALL_DIR="${1#--install-dir=}"; shift ;;
        --userland-out) USERLAND_OUT="${2:-}"; shift 2 ;;
        --userland-out=*) USERLAND_OUT="${1#--userland-out=}"; shift ;;
        --demos-out) DEMOS_OUT="${2:-}"; shift 2 ;;
        --demos-out=*) DEMOS_OUT="${1#--demos-out=}"; shift ;;
        --quesos-out) QUESOS_OUT="${2:-}"; shift 2 ;;
        --quesos-out=*) QUESOS_OUT="${1#--quesos-out=}"; shift ;;
        *) die "unknown argument: $1" ;;
    esac
done

START_TS=$(date +%s)

# A "clean pull" has no submodules checked out; demos/build-demos.sh (doom) otherwise
# die()s mid-run on the missing submodule, a confusing place to first discover it on a
# one-command promise (Decision 4). Mirrors the existing precedent in
# scripts/refresh-c-toolchain.sh, which already runs the same command as its own first
# step.
echo "=== [1/5] git submodule init ==="
if ! git -C "${REPO_ROOT}" submodule update --init --recursive; then
    echo "build-world.sh: stage [1/5] 'git submodule init' failed. Fix the submodule error above, then re-run; no later stage ran." >&2
    exit 1
fi

echo "=== [2/5] native binaries + C toolchain (install-mazm.sh) ==="
if ! "${SCRIPT_DIR}/install-mazm.sh" "${PRESET}" "${INSTALL_DIR}"; then
    echo "build-world.sh: stage [2/5] 'native binaries + C toolchain' failed. See install-mazm.sh's own error above; no later stage ran." >&2
    exit 1
fi

echo "=== [3/5] quesOS (build-quesos.sh) ==="
if ! "${REPO_ROOT}/os/quesos/build-quesos.sh" --preset "${PRESET}" -o "${QUESOS_OUT}"; then
    echo "build-world.sh: stage [3/5] 'quesOS' failed. See build-quesos.sh's own error above; no later stage ran." >&2
    exit 1
fi

echo "=== [4/5] wave-1 userland (build-userland.sh) ==="
if ! "${REPO_ROOT}/userland/build-userland.sh" --preset "${PRESET}" --out "${USERLAND_OUT}"; then
    echo "build-world.sh: stage [4/5] 'wave-1 userland' failed. See build-userland.sh's own error above; no later stage ran." >&2
    exit 1
fi

echo "=== [5/5] demos (build-demos.sh) ==="
if ! "${REPO_ROOT}/demos/build-demos.sh" --preset "${PRESET}" --out "${DEMOS_OUT}"; then
    echo "build-world.sh: stage [5/5] 'demos' failed. See build-demos.sh's own error above; no later stage ran." >&2
    exit 1
fi

END_TS=$(date +%s)
ELAPSED=$((END_TS - START_TS))

echo ""
echo "=== build-world.sh: all stages complete ==="

echo "Native tools:"
for tool in maize maizeg mazm mzld mzdis; do
    for cand in "${INSTALL_DIR}/${tool}" "${INSTALL_DIR}/${tool}.exe"; do
        [ -f "$cand" ] && echo "  ${cand}"
    done
done

echo "C cross-toolchain:"
for cand in \
    "${REPO_ROOT}/toolchain/qbe/obj/qbe" "${REPO_ROOT}/toolchain/qbe/obj/qbe.exe" \
    "${REPO_ROOT}/toolchain/cproc/cproc-qbe" "${REPO_ROOT}/toolchain/cproc/cproc-qbe.exe" \
    "${REPO_ROOT}/toolchain/cproc/cproc" "${REPO_ROOT}/toolchain/cproc/cproc.exe"; do
    [ -f "$cand" ] && echo "  ${cand}"
done

echo "quesOS image:"
[ -f "${QUESOS_OUT}" ] && echo "  ${QUESOS_OUT}"

echo "Userland (${USERLAND_OUT}):"
if [ -d "${USERLAND_OUT}" ]; then
    for f in "${USERLAND_OUT}"/*; do
        [ -f "$f" ] && echo "  ${f}"
    done
fi

echo "Demos (${DEMOS_OUT}):"
if [ -d "${DEMOS_OUT}" ]; then
    for f in "${DEMOS_OUT}"/*; do
        [ -f "$f" ] && echo "  ${f}"
    done
fi

echo "Total elapsed: ${ELAPSED}s"
exit 0
