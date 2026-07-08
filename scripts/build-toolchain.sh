#!/bin/sh
# Build the vendored C toolchain (cproc + qbe) from a clean checkout (maize-61).
#
# This is a BUILD gate: it overlays the Maize QBE target onto the pinned qbe
# submodule (maize-62), builds cproc and qbe with each tool's own build system, and
# checks that the expected executables were produced and that `qbe -t maize` is a
# recognized target. The end-to-end C-hello-world stdout diff lives in its own
# script (scripts/run-ctest.sh), kept separate from scripts/run-tests.{sh,ps1} (the
# asm/ PASS/FAIL harness) so a codegen regression reports distinctly from a toolchain
# build regression or an asm/ test regression.
#
# Runs on Linux/macOS directly (system compiler + POSIX make) and on Windows under
# MSYS2's POSIX environment (msys2/setup-msys2, `shell: msys2 {0}`). It is NOT run
# under native llvm-mingw: cproc's driver.c depends on POSIX process APIs
# (<spawn.h>/posix_spawn/<sys/wait.h>/<unistd.h>) and cproc's configure only
# recognizes POSIX target triples, so a native Windows PE build of cproc is not
# possible without patching vendored upstream. See toolchain/VENDORING.md.
#
# Exit codes:
#   0 - both tools built and their executables are present
#   1 - a build failed or an expected executable is missing
#   2 - environment/setup failure (submodules not initialized)
#
# Usage: scripts/build-toolchain.sh

set -eu

# --- Paths resolved relative to THIS script, not the caller's CWD ----------------
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "${SCRIPT_DIR}/.." && pwd)
QBE_DIR="${REPO_ROOT}/toolchain/qbe"
CPROC_DIR="${REPO_ROOT}/toolchain/cproc"

# --- Submodules must be initialized ----------------------------------------------
if [ ! -f "${QBE_DIR}/Makefile" ] || [ ! -f "${CPROC_DIR}/Makefile" ]; then
    echo "build-toolchain.sh: toolchain submodules not initialized." >&2
    echo "  run: git submodule update --init --recursive" >&2
    exit 2
fi

: "${MAKE:=make}"
if [ -z "${CC:-}" ]; then
    if command -v cc >/dev/null 2>&1; then CC=cc; else CC=gcc; fi
fi

# cproc's configure only recognizes POSIX target triples. Under MSYS2,
# `cc -dumpmachine` returns x86_64-pc-msys, which configure rejects with
# "unknown target". Pin an explicit recognized host so config.h is generated;
# this is a build-only gate, so the driver's runtime tool paths baked into
# config.h are never exercised here.
CONFIGURE_HOST=""
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) CONFIGURE_HOST="--host=x86_64-linux-gnu" ;;
esac

# Overlay the Maize QBE target onto the pinned qbe submodule (maize-62). This
# copies the target sources into qbe/maize/ and applies the registration patch
# before the build, keeping the submodule pinned at its upstream commit.
echo "=== overlaying Maize qbe target ==="
"${SCRIPT_DIR}/apply-maize-qbe-target.sh"

echo "=== building qbe (${QBE_DIR}) ==="
"${MAKE}" -C "${QBE_DIR}" CC="${CC}"

echo "=== building cproc (${CPROC_DIR}) ==="
(
    cd "${CPROC_DIR}"
    # shellcheck disable=SC2086
    ./configure CC="${CC}" ${CONFIGURE_HOST}
    "${MAKE}" CC="${CC}"
)

# --- Verify the expected executables exist (tolerating a .exe suffix) -------------
missing=0
check_exe() {
    if [ -f "$1" ] || [ -f "$1.exe" ]; then
        echo "  OK: $1"
    else
        echo "  MISSING: $1 (and $1.exe)" >&2
        missing=1
    fi
}

echo "=== verifying executables ==="
check_exe "${QBE_DIR}/obj/qbe"
check_exe "${CPROC_DIR}/cproc"
check_exe "${CPROC_DIR}/cproc-qbe"

if [ "$missing" -ne 0 ]; then
    echo "build-toolchain.sh: one or more executables are missing." >&2
    exit 1
fi

# --- Verify `qbe -t maize` is a recognized target (maize-62 AC 6626) --------------
QBE_EXE="${QBE_DIR}/obj/qbe"
[ -f "${QBE_EXE}" ] || QBE_EXE="${QBE_DIR}/obj/qbe.exe"
echo "=== verifying qbe -t maize target ==="
if printf 'export function w $t() {\n@s\n\tret 0\n}\n' | "${QBE_EXE}" -t maize - >/dev/null 2>&1; then
    echo "  OK: qbe -t maize"
else
    echo "build-toolchain.sh: qbe does not recognize the 'maize' target." >&2
    exit 1
fi

echo "toolchain build OK (cproc + qbe + Maize target)"
exit 0
