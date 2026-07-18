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

# --- maize-263: WSL-native mirror + throttle. This script takes no arguments, so the
#     mirrored child runs argument-free; the mirror still moves the cold make/configure
#     churn (and the .git-dependent apply-maize-qbe-target.sh check, D4) onto native
#     storage. When invoked by an already-mirrored run-ctest.sh / cc-maize.sh the
#     inherited MAIZE_NATIVE_MIRROR_ACTIVE=1 makes this a no-op (runs in-place). ----
. "${SCRIPT_DIR}/lib/harness-env.sh"
maize_apply_throttle
# Precompute submodule SHAs on the SOURCE side (git works here) BEFORE re-exec, so the
# git-less mirror (D14) reads them from MAIZE_KEY_* env instead of running git against a
# broken in-mirror gitlink.
maize_precompute_submodule_keys "$REPO_ROOT"
maize_native_mirror_run "$REPO_ROOT" "$SCRIPT_DIR" "$(basename "$0")" -- "$@"

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

# --- maize-263 C1: content-addressed toolchain binary cache ------------------------
# cproc/qbe are pinned-submodule build products that every fresh worktree otherwise
# cold-builds. Cache them keyed on the two submodule SHAs + the Maize QBE-target
# overlay/patch content + compiler identity + platform (decision D7), so the key
# rolls automatically on any submodule re-pin or overlay/patch edit (the maize-110
# stale-patch footgun cannot recur: the registration patch is IN the key). A hit
# copies the three binaries in and skips make/configure entirely; it still falls
# through to the existing check_exe + `qbe -t maize` smoke check below, which
# doubles as cache-integrity verification. MAIZE_NO_TOOLCHAIN_CACHE=1 forces a build.
# A hit is additionally gated on the .provenance marker existing (review cycle-1
# [minor]; written last when populating, mirroring C2's .complete marker in
# build-userland.sh), so a torn cache from an interrupted populate (binaries copied,
# marker not yet written) is never mistaken for a hit.
QBE_MAIZE_DIR="${REPO_ROOT}/toolchain/qbe-maize"
TOOLCHAIN_CACHE_ROOT="${MAIZE_TOOLCHAIN_CACHE:-$HOME/.cache/maize/toolchain}"

# Resolve a cached/produced binary tolerating a .exe suffix; echo the real path or "".
tc_resolve() {
    if [ -f "$1" ]; then
        echo "$1"
    elif [ -f "$1.exe" ]; then
        echo "$1.exe"
    fi
}

toolchain_cache_key() {
    {
        # Submodule SHAs come from the host-side precompute (D14): git does not resolve
        # inside the git-less mirror. Fall back to the labels only if the env is unset
        # (e.g. a submodule was absent at precompute time).
        printf '%s\n' "${MAIZE_KEY_QBE:-no-qbe-head}"
        printf '%s\n' "${MAIZE_KEY_CPROC:-no-cproc-head}"
        for _f in all.h targ.c abi.c isel.c emit.c data.c qbe-registration.patch; do
            cat "${QBE_MAIZE_DIR}/${_f}" 2>/dev/null || true
        done
        command -v "${CC}" 2>/dev/null || printf '%s\n' "${CC}"
        "${CC}" --version 2>/dev/null | head -1 || true
        uname -s
        uname -m
    } | maize_sha256
}

CACHE_KEY=$(toolchain_cache_key) || CACHE_KEY=""
CACHE_DIR=""
if [ -n "$CACHE_KEY" ]; then
    CACHE_DIR="${TOOLCHAIN_CACHE_ROOT}/${CACHE_KEY}"
fi

TOOLCHAIN_FROM_CACHE=0
if [ "${MAIZE_NO_TOOLCHAIN_CACHE:-}" != "1" ] && [ -n "$CACHE_DIR" ] \
    && [ -f "${CACHE_DIR}/.provenance" ] \
    && [ -n "$(tc_resolve "${CACHE_DIR}/qbe/obj/qbe")" ] \
    && [ -n "$(tc_resolve "${CACHE_DIR}/cproc/cproc")" ] \
    && [ -n "$(tc_resolve "${CACHE_DIR}/cproc/cproc-qbe")" ]; then
    echo "=== toolchain cache hit ($(printf '%.8s' "$CACHE_KEY")) ==="
    mkdir -p "${QBE_DIR}/obj" "${CPROC_DIR}"
    _c=$(tc_resolve "${CACHE_DIR}/qbe/obj/qbe");     cp -p "$_c" "${QBE_DIR}/obj/$(basename "$_c")"
    _c=$(tc_resolve "${CACHE_DIR}/cproc/cproc");     cp -p "$_c" "${CPROC_DIR}/$(basename "$_c")"
    _c=$(tc_resolve "${CACHE_DIR}/cproc/cproc-qbe"); cp -p "$_c" "${CPROC_DIR}/$(basename "$_c")"
    TOOLCHAIN_FROM_CACHE=1
else
    # Cache miss: overlay + build exactly as before. Bound make parallelism off CI
    # (D6); GNU make defaults to serial with -j unset, so this is the explicit cap.
    MAKE_JOBS_FLAG=""
    if ! maize_is_ci; then
        _mj=$(maize_bounded_jobs)
        MAKE_JOBS_FLAG="-j ${_mj}"
        echo "build-toolchain.sh: using ${_mj} build jobs (nproc=$(maize_nproc))"
    fi

    # Overlay the Maize QBE target onto the pinned qbe submodule (maize-62). This
    # copies the target sources into qbe/maize/ and applies the registration patch
    # before the build, keeping the submodule pinned at its upstream commit.
    echo "=== overlaying Maize qbe target ==="
    "${SCRIPT_DIR}/apply-maize-qbe-target.sh"

    echo "=== building qbe (${QBE_DIR}) ==="
    # shellcheck disable=SC2086
    "${MAKE}" -C "${QBE_DIR}" CC="${CC}" ${MAKE_JOBS_FLAG}

    echo "=== building cproc (${CPROC_DIR}) ==="
    (
        cd "${CPROC_DIR}"
        # shellcheck disable=SC2086
        ./configure CC="${CC}" ${CONFIGURE_HOST}
        # shellcheck disable=SC2086
        "${MAKE}" CC="${CC}" ${MAKE_JOBS_FLAG}
    )
fi

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

# --- maize-263 C1: populate the toolchain cache on a fresh build (D7) --------------
# Only after check_exe + the `qbe -t maize` smoke check above have passed, so a
# cached entry is always a verified-good one. A hit skipped this block entirely.
if [ "$TOOLCHAIN_FROM_CACHE" -eq 0 ] && [ -n "$CACHE_DIR" ] \
    && [ "${MAIZE_NO_TOOLCHAIN_CACHE:-}" != "1" ]; then
    _q=$(tc_resolve "${QBE_DIR}/obj/qbe")
    _c=$(tc_resolve "${CPROC_DIR}/cproc")
    _cq=$(tc_resolve "${CPROC_DIR}/cproc-qbe")
    if [ -n "$_q" ] && [ -n "$_c" ] && [ -n "$_cq" ] \
        && mkdir -p "${CACHE_DIR}/qbe/obj" "${CACHE_DIR}/cproc" \
        && cp -p "$_q"  "${CACHE_DIR}/qbe/obj/$(basename "$_q")" \
        && cp -p "$_c"  "${CACHE_DIR}/cproc/$(basename "$_c")" \
        && cp -p "$_cq" "${CACHE_DIR}/cproc/$(basename "$_cq")"; then
        {
            echo "qbe:   ${MAIZE_KEY_QBE:-unknown}"
            echo "cproc: ${MAIZE_KEY_CPROC:-unknown}"
            "${CC}" --version 2>/dev/null | head -1 || true
        } > "${CACHE_DIR}/.provenance" 2>/dev/null || true
        echo "build-toolchain.sh: populated toolchain cache ($(printf '%.8s' "$CACHE_KEY"))."
    else
        echo "build-toolchain.sh: WARNING: could not populate toolchain cache; continuing." >&2
        rm -rf "$CACHE_DIR" 2>/dev/null || true
    fi
fi

echo "toolchain build OK (cproc + qbe + Maize target)"
exit 0
