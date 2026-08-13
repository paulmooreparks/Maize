#!/bin/sh
# test-toolchain-resolution.sh (maize-439): holds the three toolchain resolvers to one
# answer.
#
# scripts/lib/ToolchainRoot.ps1, scripts/lib/toolchain-root.sh and
# cmake/ToolchainRoot.cmake implement the same resolution order three times, because
# PowerShell, POSIX sh and CMake cannot source each other. Three implementations of
# one order is a divergence waiting to happen, and a fixture that only bumps the pin
# and checks all three land on the same VERSION proves they agree on what version, not
# on precedence. So this one populates SEVERAL candidate locations at once, each
# holding a probe file with distinguishable bytes, and asserts which one each resolver
# picks per scenario.
#
# The scenarios, and what each is for:
#
#   a  env-root only                    the override is honoured at all
#   b  per-user default only            the default is honoured at all
#   c  repo fallback only               a checkout predating maize-439 still builds
#   d  env-root + repo fallback         env-root wins. A resolver that checks the
#                                       repo fallback first passes every other
#                                       scenario and fails this one, which is the
#                                       reason this fixture exists.
#   e  per-user default + repo fallback per-user default wins
#   f  all three                        env-root wins
#   g  nothing populated                all three report not-found, and CMake's
#                                       FATAL_ERROR names both paths it checked and
#                                       the bootstrap command
#
# Each candidate's probe file carries its own marker bytes, so the assertion is on
# WHICH DIRECTORY'S CONTENT came back rather than on a path string. Path spelling is
# checked too, but second: Git Bash answers /c/..., CMake answers C:/..., and a
# fixture that compared only spellings would fail on that difference while a genuinely
# divergent resolver went unnoticed.
#
# Runs nothing from the real toolchain and downloads nothing. It copies the three
# resolvers plus the pin files into a throwaway tree and drives them there, so the
# in-repo fallback candidate is a directory this fixture owns rather than the
# operator's real .toolchains/.
#
# WHERE THIS RUNS: the Windows CI job, .github/workflows/ci.yml, step "Toolchain
# resolver agreement". That is the only job carrying both prerequisites, and this
# script exits 2 rather than passing when either is absent, so a job that cannot run it
# reports a failure instead of a hollow pass. It is deliberately NOT registered with
# ctest: it needs no build, and registering it would make the ctest suite depend on
# PowerShell. Run it by hand whenever you touch one of the three resolvers.
#
# Usage: scripts/test-toolchain-resolution.sh
# Exit:  0 all scenarios passed, 1 an assertion failed, 2 a prerequisite is missing.

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "${SCRIPT_DIR}/.." && pwd)

PROBE_REL='bin/x86_64-w64-mingw32-clang++.exe'
FAILURES=0
CHECKS=0

say()  { printf '%s\n' "$*"; }
fail() { printf 'FAIL: %s\n' "$*" >&2; FAILURES=$((FAILURES + 1)); }

# --- Prerequisites ----------------------------------------------------------------
# All three resolvers, or none: a run that silently skips one of them would report a
# pass having checked two thirds of what this fixture is for.
PWSH=''
for cand in powershell.exe pwsh powershell; do
    if command -v "$cand" >/dev/null 2>&1; then PWSH="$cand"; break; fi
done
if [ -z "$PWSH" ]; then
    say "test-toolchain-resolution.sh: no PowerShell on PATH; cannot exercise ToolchainRoot.ps1."
    exit 2
fi
if ! command -v cmake >/dev/null 2>&1; then
    say "test-toolchain-resolution.sh: cmake not on PATH; cannot exercise ToolchainRoot.cmake."
    exit 2
fi

VERSION=$(sed -e 's/[[:space:]]*$//' "${REPO_ROOT}/scripts/toolchain-pins/llvm-mingw.pin" \
          | grep -v '^[[:space:]]*#' | grep -v '^[[:space:]]*$' | sed -n '1p')
[ -n "$VERSION" ] || { say "cannot read the pinned version"; exit 2; }

# --- The throwaway tree ------------------------------------------------------------
WORK=$(mktemp -d 2>/dev/null || mktemp -d -t maize-tcres)
cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT INT TERM

FAKE_REPO="${WORK}/repo"
mkdir -p "${FAKE_REPO}/scripts/lib" "${FAKE_REPO}/scripts/toolchain-pins" "${FAKE_REPO}/cmake"
cp "${REPO_ROOT}/scripts/lib/ToolchainRoot.ps1"          "${FAKE_REPO}/scripts/lib/"
cp "${REPO_ROOT}/scripts/lib/toolchain-root.sh"          "${FAKE_REPO}/scripts/lib/"
cp "${REPO_ROOT}/scripts/toolchain-pins/llvm-mingw.pin"  "${FAKE_REPO}/scripts/toolchain-pins/"
cp "${REPO_ROOT}/scripts/toolchain-pins/sdl2.pin"        "${FAKE_REPO}/scripts/toolchain-pins/"
cp "${REPO_ROOT}/cmake/ToolchainRoot.cmake"              "${FAKE_REPO}/cmake/"

ENV_ROOT="${WORK}/env-root"
USER_ROOT="${WORK}/localappdata"          # stands in for %LOCALAPPDATA%
REPO_FALLBACK="${FAKE_REPO}/.toolchains/llvm-mingw"

# Windows-shaped, forward-slash spellings for the values that cross into PowerShell
# and CMake. cygpath -am also collapses any ".." a resolver composes, which is what
# makes the path comparison below meaningful.
winpath() {
    if command -v cygpath >/dev/null 2>&1; then
        cygpath -am "$1"
    else
        printf '%s' "$1"
    fi
}
ENV_ROOT_W=$(winpath "$ENV_ROOT")
USER_ROOT_W=$(winpath "$USER_ROOT")

# Write a candidate's probe file with its own marker bytes. <dir> <marker>
populate() {
    mkdir -p "$1/bin"
    printf '%s' "$2" > "$1/${PROBE_REL#bin/}.tmp"
    mv "$1/${PROBE_REL#bin/}.tmp" "$1/$PROBE_REL"
    # The C compiler sits beside the C++ one in a real install; some callers probe on
    # it instead, so a fixture that created only one would not represent the layout.
    printf '%s' "$2" > "$1/bin/x86_64-w64-mingw32-clang.exe"
}

depopulate() { rm -rf "$1"; }

ENV_DIR="${ENV_ROOT}/llvm-mingw/${VERSION}"
USER_DIR="${USER_ROOT}/Maize/toolchains/llvm-mingw/${VERSION}"

# --- Driving one resolver ----------------------------------------------------------
# Each of the three runs in its own process with the scenario's environment, and
# echoes the directory it resolved (empty when it resolved nothing).

run_sh() {
    (
        set +e
        if [ -n "$SCEN_ENV_ROOT" ]; then
            MAIZE_TOOLCHAIN_ROOT="$SCEN_ENV_ROOT"; export MAIZE_TOOLCHAIN_ROOT
        else
            unset MAIZE_TOOLCHAIN_ROOT
        fi
        LOCALAPPDATA="$USER_ROOT_W"; export LOCALAPPDATA
        MAIZE_TOOLCHAIN_LIB_DIR="${FAKE_REPO}/scripts/lib"
        . "${FAKE_REPO}/scripts/lib/toolchain-root.sh"
        maize_resolve_toolchain_dir llvm-mingw "$PROBE_REL" 2>/dev/null || true
    )
}

run_ps() {
    cat > "${WORK}/probe.ps1" <<'PS1'
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $env:MAIZE_FIXTURE_REPO 'scripts/lib/ToolchainRoot.ps1')
$d = Resolve-MaizeToolchainDir -Tool 'llvm-mingw' -ProbeRelativePath $env:MAIZE_FIXTURE_PROBE
if ($d) { Write-Output $d }
PS1
    (
        set +e
        if [ -n "$SCEN_ENV_ROOT" ]; then
            MAIZE_TOOLCHAIN_ROOT="$SCEN_ENV_ROOT"; export MAIZE_TOOLCHAIN_ROOT
        else
            unset MAIZE_TOOLCHAIN_ROOT
        fi
        LOCALAPPDATA="$USER_ROOT_W"; export LOCALAPPDATA
        MAIZE_FIXTURE_REPO=$(winpath "$FAKE_REPO"); export MAIZE_FIXTURE_REPO
        MAIZE_FIXTURE_PROBE="$PROBE_REL"; export MAIZE_FIXTURE_PROBE
        "$PWSH" -NoProfile -NonInteractive -File "$(winpath "${WORK}/probe.ps1")" 2>/dev/null | tr -d '\r'
    )
}

run_cmake() {
    # The sentinel matters: ToolchainRoot.cmake's own unconditional
    # message(STATUS "Maize toolchain: using ...") also lands on stdout under -P, so a
    # driver that just printed the variable would hand back two lines and the fixture
    # would compare the wrong one.
    cat > "${WORK}/probe.cmake" <<CMAKE1
include("\${MAIZE_FIXTURE_REPO}/cmake/ToolchainRoot.cmake")
message(STATUS "MAIZE_FIXTURE_RESOLVED=\${MAIZE_RESOLVED_TOOLCHAIN_DIR}")
CMAKE1
    (
        set +e
        if [ -n "$SCEN_ENV_ROOT" ]; then
            MAIZE_TOOLCHAIN_ROOT="$SCEN_ENV_ROOT"; export MAIZE_TOOLCHAIN_ROOT
        else
            unset MAIZE_TOOLCHAIN_ROOT
        fi
        LOCALAPPDATA="$USER_ROOT_W"; export LOCALAPPDATA
        cmake "-DMAIZE_FIXTURE_REPO=$(winpath "$FAKE_REPO")" \
              -P "$(winpath "${WORK}/probe.cmake")" 2>"${WORK}/cmake.err" \
            | tr -d '\r' | sed -n 's/^-- MAIZE_FIXTURE_RESOLVED=//p'
    )
}

# Read a resolved directory's marker bytes, or the empty string.
marker_of() {
    [ -n "$1" ] || { printf ''; return 0; }
    _m_dir=$1
    case "$_m_dir" in
        [A-Za-z]:[/\\]*) if command -v cygpath >/dev/null 2>&1; then _m_dir=$(cygpath -u "$_m_dir"); fi ;;
    esac
    if [ -f "${_m_dir}/${PROBE_REL}" ]; then cat "${_m_dir}/${PROBE_REL}"; else printf ''; fi
}

canon() {
    [ -n "$1" ] || { printf ''; return 0; }
    winpath "$1" | tr 'A-Z' 'a-z' | sed -e 's#/*$##'
}

# --- One scenario ------------------------------------------------------------------
# scenario <name> <env-root-value-or-empty> <expected-marker-or-NONE> <description>
scenario() {
    _s_name=$1
    SCEN_ENV_ROOT=$2
    _s_expect=$3
    _s_desc=$4

    _sh=$(run_sh)
    _ps=$(run_ps)
    _cm=$(run_cmake)

    _sh_m=$(marker_of "$_sh")
    _ps_m=$(marker_of "$_ps")
    _cm_m=$(marker_of "$_cm")

    CHECKS=$((CHECKS + 1))
    say "-- scenario ${_s_name}: ${_s_desc}"
    say "     sh    -> ${_sh:-<none>}  [${_sh_m:-<none>}]"
    say "     ps    -> ${_ps:-<none>}  [${_ps_m:-<none>}]"
    say "     cmake -> ${_cm:-<none>}  [${_cm_m:-<none>}]"

    if [ "$_s_expect" = NONE ]; then
        [ -z "$_sh" ] || fail "${_s_name}: sh resolved ${_sh}, expected nothing"
        [ -z "$_ps" ] || fail "${_s_name}: ps resolved ${_ps}, expected nothing"
        [ -z "$_cm" ] || fail "${_s_name}: cmake resolved ${_cm}, expected nothing"
        # CMake is the one resolver that must FAIL rather than return empty, and its
        # message is AC-1's contract: both checked paths plus the bootstrap command.
        for _needle in 'Vendored llvm-mingw compiler not found' 'bootstrap-toolchain.ps1' "llvm-mingw/${VERSION}" '.toolchains/llvm-mingw'; do
            grep -qF "$_needle" "${WORK}/cmake.err" \
                || fail "${_s_name}: cmake's FATAL_ERROR does not mention '${_needle}'"
        done
        return 0
    fi

    for _pair in "sh ${_sh_m}" "ps ${_ps_m}" "cmake ${_cm_m}"; do
        _who=${_pair%% *}
        _got=${_pair#* }
        [ "$_got" = "$_s_expect" ] \
            || fail "${_s_name}: ${_who} resolved a directory marked '${_got}', expected '${_s_expect}'"
    done

    _sh_c=$(canon "$_sh"); _ps_c=$(canon "$_ps"); _cm_c=$(canon "$_cm")
    if [ "$_sh_c" != "$_ps_c" ] || [ "$_sh_c" != "$_cm_c" ]; then
        fail "${_s_name}: the three resolvers named different directories: sh=${_sh_c} ps=${_ps_c} cmake=${_cm_c}"
    fi
}

# --- The scenarios -----------------------------------------------------------------
say "toolchain resolution fixture: pinned version ${VERSION}"
say "  env-root      ${ENV_DIR}"
say "  per-user      ${USER_DIR}"
say "  repo fallback ${REPO_FALLBACK}"
say ""

populate "$ENV_DIR" ENV
scenario a "$ENV_ROOT_W" ENV "MAIZE_TOOLCHAIN_ROOT only"
depopulate "$ENV_ROOT"

populate "$USER_DIR" USER
scenario b "" USER "per-user default only"
depopulate "$USER_ROOT"

populate "$REPO_FALLBACK" REPO
scenario c "" REPO "in-repo fallback only"

# The scenario a wrong implementation gets wrong. Both are present; the override wins.
populate "$ENV_DIR" ENV
scenario d "$ENV_ROOT_W" ENV "MAIZE_TOOLCHAIN_ROOT beats the in-repo fallback"
depopulate "$ENV_ROOT"

populate "$USER_DIR" USER
scenario e "" USER "per-user default beats the in-repo fallback"

populate "$ENV_DIR" ENV
scenario f "$ENV_ROOT_W" ENV "all three present, MAIZE_TOOLCHAIN_ROOT wins"

depopulate "$ENV_ROOT"
depopulate "$USER_ROOT"
depopulate "$REPO_FALLBACK"
scenario g "" NONE "nothing populated: all three report not-found"

say ""
if [ "$FAILURES" -eq 0 ]; then
    say "PASS: ${CHECKS} scenarios, three resolvers each, all in agreement."
    exit 0
fi
say "FAILED: ${FAILURES} assertion(s) across ${CHECKS} scenarios."
exit 1
