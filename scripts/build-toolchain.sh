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
# Runs on Linux/macOS directly (system compiler + POSIX make), on Windows under
# MSYS2's POSIX environment when `make` is present (msys2/setup-msys2,
# `shell: msys2 {0}`), and natively on Windows under plain Git Bash (maize-257):
# when `make` is absent on a MINGW*/MSYS* uname, the two needed binaries are
# compiled directly with the vendored llvm-mingw clang instead of via each tool's
# Makefile. This works because the POSIX-only constraint is actually about cproc's
# DRIVER (driver.c: posix_spawn/<sys/wait.h>/<unistd.h>, and cproc's configure
# only recognizing POSIX target triples), not about qbe or cproc-qbe (the compiler
# frontend proper): cc-maize.sh never invokes the driver binary, so the native
# branch below builds qbe.exe and cproc-qbe.exe only, skipping driver.c and the
# configure/config.h step entirely (config.h is used only by driver.c). See
# toolchain/VENDORING.md.
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

# --- maize-257: native-Windows detection (Git Bash, no make on PATH) --------------
# MSYS2 (msys2/setup-msys2, `gcc`+`make` installed) still takes the existing
# make-based path below unchanged; only a bare Git-Bash environment (no `make`)
# takes the native clang-direct branch.
NATIVE_WINDOWS=0
case "$(uname -s)" in
    MINGW*|MSYS*)
        if ! command -v "${MAKE}" >/dev/null 2>&1; then
            NATIVE_WINDOWS=1
        fi
        ;;
esac

if [ "$NATIVE_WINDOWS" -eq 1 ]; then
    # Vendored llvm-mingw clang (bootstrap-toolchain.ps1), the same compiler the
    # windows-llvm-mingw-* CMake presets use for the VM itself.
    : "${CC:=${REPO_ROOT}/.toolchains/llvm-mingw/bin/x86_64-w64-mingw32-clang.exe}"
    if [ ! -f "$CC" ]; then
        echo "build-toolchain.sh: vendored llvm-mingw clang not found at ${CC}." >&2
        echo "  run: scripts/bootstrap-toolchain.ps1" >&2
        exit 2
    fi
elif [ -z "${CC:-}" ]; then
    if command -v cc >/dev/null 2>&1; then CC=cc; else CC=gcc; fi
fi

# cproc's configure only recognizes POSIX target triples. Under MSYS2,
# `cc -dumpmachine` returns x86_64-pc-msys, which configure rejects with
# "unknown target". Pin an explicit recognized host so config.h is generated;
# this is a build-only gate, so the driver's runtime tool paths baked into
# config.h are never exercised here. Not needed on the native-Windows branch: it
# never runs configure (see build_native_windows).
CONFIGURE_HOST=""
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) CONFIGURE_HOST="--host=x86_64-linux-gnu" ;;
esac

# --- maize-257: native-Windows build (no make, no configure) ----------------------
# Compiles qbe.exe and cproc-qbe.exe directly with $CC (the vendored llvm-mingw
# clang), mirroring the SRC/SRCALL lists each tool's own Makefile enumerates
# (toolchain/qbe/Makefile, toolchain/cproc/Makefile) so the object set stays in
# lockstep with upstream's own build description. driver.c is never compiled here
# (POSIX-only, unused by cc-maize.sh); config.h is skipped entirely for cproc since
# only driver.c includes it (verified: `grep -rl config.h toolchain/cproc/*.c`
# matches only driver.c). qbe's config.h IS needed (main.c includes it
# unconditionally for its no-`-t`-flag default), so it is generated with the exact
# case logic qbe's own Makefile `config.h:` target uses, just run directly instead
# of through make.
build_native_windows() {
    echo "=== building qbe natively (${QBE_DIR}, ${CC}) ==="
    mkdir -p "${QBE_DIR}/obj/amd64" "${QBE_DIR}/obj/arm64" "${QBE_DIR}/obj/maize"

    # qbe's own config.h generation logic (toolchain/qbe/Makefile's `config.h:`
    # target), run directly since there is no make here.
    (
        cd "${QBE_DIR}"
        case "$(uname)" in
            *Darwin*)
                {
                    echo "#define Defasm Gasmacho"
                    echo "#define Deftgt T_amd64_sysv"
                } > config.h
                ;;
            *)
                {
                    echo "#define Defasm Gaself"
                    case "$(uname -m)" in
                        *aarch64*) echo "#define Deftgt T_arm64" ;;
                        *)         echo "#define Deftgt T_amd64_sysv" ;;
                    esac
                } > config.h
                ;;
        esac
    )

    _qbe_src="main.c util.c parse.c cfg.c mem.c ssa.c alias.c load.c copy.c fold.c live.c spill.c rega.c gas.c"
    _qbe_amd64="amd64/targ.c amd64/sysv.c amd64/isel.c amd64/emit.c"
    _qbe_arm64="arm64/targ.c arm64/abi.c arm64/isel.c arm64/emit.c"
    _qbe_maize="maize/targ.c maize/abi.c maize/isel.c maize/emit.c maize/data.c"
    (
        cd "${QBE_DIR}"
        for _f in ${_qbe_src} ${_qbe_amd64} ${_qbe_arm64} ${_qbe_maize}; do
            "${CC}" -Wall -Wextra -std=c99 -g -pedantic -c "${_f}" -o "obj/${_f%.c}.o"
        done
        "${CC}" obj/*.o obj/amd64/*.o obj/arm64/*.o obj/maize/*.o -o obj/qbe.exe
    )

    echo "=== building cproc-qbe natively (${CPROC_DIR}, ${CC}) ==="
    # NO driver.c: cc-maize.sh pipes into cproc-qbe directly and never spawns the
    # POSIX-only driver. BACKEND=qbe, so qbe.c stands in for $(BACKEND).c.
    _cproc_src="attr.c decl.c eval.c expr.c init.c main.c map.c pp.c scan.c scope.c stmt.c targ.c token.c tree.c type.c utf.c util.c qbe.c"
    (
        cd "${CPROC_DIR}"
        mkdir -p obj
        for _f in ${_cproc_src}; do
            "${CC}" -Wall -Wpedantic -Wno-parentheses -Wno-switch -g -pipe -c "${_f}" -o "obj/${_f%.c}.o"
        done
        "${CC}" obj/*.o -o cproc-qbe.exe
    )
}

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

# maize-257: the native-Windows branch never builds the cproc DRIVER (cproc.exe,
# POSIX-only, unused by cc-maize.sh), only qbe + cproc-qbe, so the cache
# hit/populate/verify checks below skip that one binary on that branch.
TOOLCHAIN_FROM_CACHE=0
if [ "${MAIZE_NO_TOOLCHAIN_CACHE:-}" != "1" ] && [ -n "$CACHE_DIR" ] \
    && [ -f "${CACHE_DIR}/.provenance" ] \
    && [ -n "$(tc_resolve "${CACHE_DIR}/qbe/obj/qbe")" ] \
    && { [ "$NATIVE_WINDOWS" -eq 1 ] || [ -n "$(tc_resolve "${CACHE_DIR}/cproc/cproc")" ]; } \
    && [ -n "$(tc_resolve "${CACHE_DIR}/cproc/cproc-qbe")" ]; then
    echo "=== toolchain cache hit ($(printf '%.8s' "$CACHE_KEY")) ==="
    mkdir -p "${QBE_DIR}/obj" "${CPROC_DIR}"
    _c=$(tc_resolve "${CACHE_DIR}/qbe/obj/qbe");     cp -p "$_c" "${QBE_DIR}/obj/$(basename "$_c")"
    if [ "$NATIVE_WINDOWS" -ne 1 ]; then
        _c=$(tc_resolve "${CACHE_DIR}/cproc/cproc"); cp -p "$_c" "${CPROC_DIR}/$(basename "$_c")"
    fi
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

    if [ "$NATIVE_WINDOWS" -eq 1 ]; then
        build_native_windows
    else
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
if [ "$NATIVE_WINDOWS" -ne 1 ]; then
    check_exe "${CPROC_DIR}/cproc"
fi
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
    _c=""
    if [ "$NATIVE_WINDOWS" -ne 1 ]; then
        _c=$(tc_resolve "${CPROC_DIR}/cproc")
    fi
    _cq=$(tc_resolve "${CPROC_DIR}/cproc-qbe")
    if [ -n "$_q" ] && { [ "$NATIVE_WINDOWS" -eq 1 ] || [ -n "$_c" ]; } && [ -n "$_cq" ] \
        && mkdir -p "${CACHE_DIR}/qbe/obj" "${CACHE_DIR}/cproc" \
        && cp -p "$_q"  "${CACHE_DIR}/qbe/obj/$(basename "$_q")" \
        && { [ "$NATIVE_WINDOWS" -eq 1 ] || cp -p "$_c"  "${CACHE_DIR}/cproc/$(basename "$_c")"; } \
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
