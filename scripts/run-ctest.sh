#!/bin/sh
# Compile + run the C hello-world through the full Maize C toolchain and diff its
# stdout against the committed fixture (maize-62, maize-11 AC 6397 / 6399).
#
# Pipeline (maize-77 segmented .mzo -> mzld -> .mzx object model). The whole C
# compile pipeline itself lives in scripts/cc-maize.sh (maize-96): the SINGLE
# canonical driver both CI and the operator's ~/bin/mzcc call, so what CI runs
# is exactly what the operator acceptance-tests with. Per fixture:
#
#   ctest/<name>.c
#     -> cc-maize.sh -o <name>.mzx   (new no-run default; tr -> cpp -E -> cproc-qbe ->
#                              normalize -> qbe -t maize -> mazm -c -> mzld over the
#                              crt0/syscall + C runtime (errno/string/ctype/stdio/stdlib/dirent); entry _start; W^X)
#     -> maize                (load_mzx sets RP=_start; execute; capture stdout)
#     -> diff vs ctest/<name>.expected
#
# The normalize sed, the cpp flags, the RT object set, and the mzld link order are
# defined in cc-maize.sh and NOWHERE ELSE, so CI and mzcc cannot drift apart.
#
# This is kept DISTINCT from run-tests.{sh,ps1} (the asm/ corpus harness) so a
# codegen regression reports separately from an asm-suite regression (maize-61
# decision 6611 precedent). maize-63 adds its nontrivial program to this runner.
#
# maize-58 adds an exit-status check (run_exit_status_test): a fixture whose main
# returns a fixed nonzero constant is run and its process exit status ($?) is
# captured and asserted, a code path separate from the stdout compare so an
# exit-status regression (sys_exit / crt0 / maize.cpp) surfaces on its own.
#
# Exit codes:
#   0 - every C program produced its expected stdout
#   1 - a program mismatched, or a pipeline stage failed
#   2 - environment/setup failure (a required executable is missing)
#
# Usage: scripts/run-ctest.sh [--preset <name>] [--skip-build]
#
# maize-376: the same fixtures are also registered as CTest tests, so once the preset
# is configured they can be selected, parallelised and individually timed:
#
#   ctest --test-dir build/linux-debug                 every fixture, serially
#   ctest --test-dir build/linux-debug -j8             the same set in parallel
#   ctest --test-dir build/linux-debug -L hostfs       one subsystem
#   ctest --test-dir build/linux-debug -R kilo         one fixture (regex on the name)
#
# Running this script directly is unchanged and still the plain entry point; the ctest
# path drives the identical fixture bodies through --ctest-setup / --ctest-env below.

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "${SCRIPT_DIR}/.." && pwd)
CTEST_DIR="${REPO_ROOT}/ctest"
RT_DIR="${REPO_ROOT}/toolchain/rt"
QBE_DIR="${REPO_ROOT}/toolchain/qbe"
CPROC_DIR="${REPO_ROOT}/toolchain/cproc"

# --- maize-263: WSL-native mirror + throttle, BEFORE the arg loop consumes "$@" so
#     the original argument vector reaches the mirrored child intact. A nested
#     cc-maize.sh call inside the mirror inherits MAIZE_NATIVE_MIRROR_ACTIVE=1 and
#     runs in-place (no double sync/run). ----------------------------------------
. "${SCRIPT_DIR}/lib/harness-env.sh"

# --- maize-376: the CTest dispatch modes ------------------------------------------
# Two modes let `ctest` drive this harness one fixture per test, so selection (-R/-L),
# parallelism (-jN), per-test timeouts and per-test timing all come from the tool
# instead of from bespoke shell. A plain invocation (neither flag) is unchanged.
#
#   --ctest-setup <file>  run the whole preamble ONCE (mirror sync, toolchain
#                         build-if-absent, binary resolution, wrapper-script
#                         generation), write <file> as a sourceable env snapshot,
#                         and exit. Wired as the ctest_guest_env FIXTURES_SETUP test.
#   --ctest-env <file> --only <label>
#                         source <file>, skip the preamble entirely, and run exactly
#                         the one mz_timed dispatch site named <label>. Wired as every
#                         per-fixture add_test, FIXTURES_REQUIRED ctest_guest_env.
#
# The peek loop below must run BEFORE maize_apply_throttle / maize_native_mirror_run,
# because a --ctest-env invocation must skip both (no renice of a ctest child, and
# above all no rsync --delete into the shared mirror directory from N concurrent
# fixtures under -jN). It must ALSO not consume "$@": the non-ctest path still hands
# the pristine argument vector to maize_native_mirror_run for its re-exec. `for x in
# "$@"` iterates without shifting, so the vector survives intact for the branch below.
CTEST_SETUP_FILE=''
CTEST_ENV_FILE=''
ONLY=''
_peek_prev=''
for _peek_arg in "$@"; do
    case "$_peek_prev" in
        --ctest-setup) CTEST_SETUP_FILE="$_peek_arg" ;;
        --ctest-env)   CTEST_ENV_FILE="$_peek_arg" ;;
        --only)        ONLY="$_peek_arg" ;;
    esac
    case "$_peek_arg" in
        --ctest-setup=*) CTEST_SETUP_FILE="${_peek_arg#--ctest-setup=}" ;;
        --ctest-env=*)   CTEST_ENV_FILE="${_peek_arg#--ctest-env=}" ;;
        --only=*)        ONLY="${_peek_arg#--only=}" ;;
    esac
    _peek_prev="$_peek_arg"
done

if [ -z "$CTEST_ENV_FILE" ]; then
    maize_apply_throttle
    # Precompute submodule SHAs host-side before re-exec (D14): this script nests
    # build-toolchain.sh inside the git-less mirror, which reads MAIZE_KEY_* from env.
    maize_precompute_submodule_keys "$REPO_ROOT"
    maize_native_mirror_run "$REPO_ROOT" "$SCRIPT_DIR" "$(basename "$0")" -- "$@"
fi

UNAME=$(uname -s)
case "$UNAME" in
    Linux)  DEFAULT_PRESET='linux-debug' ;;
    Darwin) DEFAULT_PRESET='macos-debug' ;;
    MINGW*|MSYS*|CYGWIN*) DEFAULT_PRESET='windows-llvm-mingw-debug' ;;
    *) echo "unsupported platform for run-ctest.sh: ${UNAME}" >&2; exit 2 ;;
esac

PRESET="$DEFAULT_PRESET"
SKIP_BUILD=0
while [ $# -gt 0 ]; do
    case "$1" in
        --preset) PRESET="${2:-}"; shift 2 ;;
        --preset=*) PRESET="${1#--preset=}"; shift ;;
        --skip-build) SKIP_BUILD=1; shift ;;
        # maize-376: already captured by the peek loop above; re-parsed here only so
        # the vector drains normally and the unknown-argument arm does not fire.
        --ctest-setup) shift 2 ;;
        --ctest-setup=*) shift ;;
        --ctest-env) shift 2 ;;
        --ctest-env=*) shift ;;
        --only) shift 2 ;;
        --only=*) shift ;;
        *) echo "Unknown argument: $1" >&2; exit 2 ;;
    esac
done

# maize-376: --only requires a dispatch mode that resolved the environment, and
# --ctest-setup/--ctest-env are mutually exclusive. Fail fast rather than half-run.
if [ -n "$CTEST_SETUP_FILE" ] && [ -n "$CTEST_ENV_FILE" ]; then
    echo "run-ctest.sh: --ctest-setup and --ctest-env are mutually exclusive." >&2; exit 2
fi
if [ -n "$CTEST_ENV_FILE" ] && [ -z "$ONLY" ]; then
    echo "run-ctest.sh: --ctest-env requires --only <label>." >&2; exit 2
fi

BUILD_DIR="${REPO_ROOT}/build/${PRESET}"
WORK_DIR="${BUILD_DIR}/ctest-run"

# Resolve an executable path, tolerating a .exe suffix on Windows. Host-aware
# preference (maize-257 micro-fix): a tree can carry both a Linux ELF binary
# (WSL-built) and its native .exe twin (Git-Bash-built) side by side, e.g.
# toolchain/qbe/obj/qbe next to qbe.exe. Trying the wrong flavor first picks a
# binary this host cannot execute, so try the host-matching flavor first and
# fall back to the other only when the preferred one is absent; that fallback
# keeps a single-flavor tree resolving exactly as before on every platform
# (this is also what the cmake-built tools, mazm/maize/mzld, rely on when only
# a .exe was ever produced there).
resolve_exe() {
    case "$UNAME" in
        MINGW*|MSYS*|CYGWIN*)
            if [ -x "$1.exe" ] || [ -f "$1.exe" ]; then echo "$1.exe"; return 0; fi
            if [ -x "$1" ] || [ -f "$1" ]; then echo "$1"; return 0; fi
            ;;
        *)
            if [ -x "$1" ] || [ -f "$1" ]; then echo "$1"; return 0; fi
            if [ -x "$1.exe" ] || [ -f "$1.exe" ]; then echo "$1.exe"; return 0; fi
            ;;
    esac
    return 1
}

# maize-114: translate a host fixture path into the form native `maize` expects for a
# --mount grant. Under MSYS/MinGW the built maize is a native Windows exe, so a POSIX
# /tmp/... path must become a Windows C:\... path (cygpath -w); elsewhere the path is
# passed through unchanged. The guest side of the grant stays a *nix path.
host_to_native() {
    case "$UNAME" in
        MINGW*|MSYS*|CYGWIN*) cygpath -w "$1" ;;
        *) printf '%s' "$1" ;;
    esac
}

# maize-257 fix pass: the real-pty fixtures (userland94_oksh_keystrokes,
# userland94_kilo_edit, userland94_kilo_kill) need a python3 whose stdlib pty
# module actually works (pty.py imports tty, which imports termios). The old
# `command -v python3` guard was only a proxy for that under CI's former MSYS2
# bash step, where python3 resolved to MSYS2's own POSIX-layer build; under
# native Git Bash on windows-latest, python3 resolves to the runner's system
# CPython instead, which has no termios/pty at all, so the guard passed while
# the probe crashed with ModuleNotFoundError before writing a single byte
# (the kilo_edit saved="" symptom). Probe the actual capability instead of a
# host-name proxy so this keeps working correctly on any host (Linux, macOS,
# or a future POSIX-layer python) without hardcoding uname.
python3_has_pty() {
    command -v python3 >/dev/null 2>&1 && python3 -c 'import pty' >/dev/null 2>&1
}

# maize-376: two routes to a resolved toolchain environment. The `else` arm below is
# the original preamble, unchanged except for its indentation; the `if` arm is the
# per-fixture ctest dispatch, which reads a snapshot of that arm's result instead of
# recomputing it.
if [ -n "$CTEST_ENV_FILE" ]; then
    # The ctest_guest_env FIXTURES_SETUP test already ran the else-arm exactly once for
    # this ctest invocation and wrote its result to $CTEST_ENV_FILE. Source it and
    # regenerate NOTHING. That is load-bearing under -jN, not just a saving: the three
    # exec wrappers (maize-bare-wrap.sh, maize-jit-wrap.sh, maizeg-bare-wrap.sh) live at
    # FIXED shared paths under ${BUILD_DIR}/ctest-run and are written by truncate-then-
    # write redirection, not by the atomic temp-plus-rename the mzcc object cache uses,
    # so N concurrent fixtures each rewriting them can let a sibling exec a zero-byte or
    # half-written script. CTest's FIXTURES_SETUP happens-before guarantee gives exactly
    # one writer that completes before any reader starts; this arm is a reader only.
    [ -f "$CTEST_ENV_FILE" ] || {
        echo "run-ctest.sh: --ctest-env file ${CTEST_ENV_FILE} not found; the ctest_guest_env setup test must run first." >&2
        exit 2; }
    . "$CTEST_ENV_FILE"
    # A few fixtures reach files through paths that are relative to the repo root rather
    # than absolute: demos/doom/doom.sources lists its ~50 doomgeneric translation units
    # relatively, and cc-maize.sh resolves those against the CURRENT directory. The
    # harness has always been invoked from the repo root, so that dependency was invisible
    # until ctest started launching it from the build directory instead. Make it explicit
    # rather than leaving it implicit in the caller's habits: a --ctest-env invocation runs
    # from the same directory a plain run-ctest.sh invocation does, whichever tree
    # --ctest-setup resolved (the WSL mirror when one is active, the repo otherwise).
    cd "$REPO_ROOT" || { echo "run-ctest.sh: cannot cd to ${REPO_ROOT}" >&2; exit 2; }
else
    # Build the C toolchain if the compilers are absent (fresh-clone one-command).
    if [ "$SKIP_BUILD" -eq 0 ]; then
        if ! resolve_exe "${QBE_DIR}/obj/qbe" >/dev/null \
        || ! resolve_exe "${CPROC_DIR}/cproc-qbe" >/dev/null; then
            "${SCRIPT_DIR}/build-toolchain.sh"
        fi
    fi

    # The whole C compile pipeline (tr -> cpp -> cproc-qbe -> normalize -> qbe -> mazm -c
    # -> mzld) lives in scripts/cc-maize.sh (maize-96); this harness drives it via the
    # no-run default (`-o <path>`) so CI exercises the EXACT pipeline the operator uses.
    # run-ctest therefore no longer resolves cproc-qbe / qbe / the system cpp itself: the
    # driver owns those. It still needs mazm (to re-assemble the W^X probe's crt0.mzo),
    # maize (to run each linked image), and mzld (the W^X negative case).
    # maize-278: MAIZE_CC selects the guest-build driver. Unset keeps cc-maize.sh
    # (the shell driver, the default until the parity gate is green on both
    # platforms, then this flips to mzcc, still a one-line change). Set it to
    # <REPO_ROOT>/build/<preset>/mzcc to point the whole harness at the compiled
    # native driver: compile_c, run_default_produce_test and run_driver_run_mode_test
    # all invoke the driver through this ONE binding, so they exercise mzcc unchanged.
    # Additive; no script is retired here (that is maize-281).
    CC_MAIZE="${MAIZE_CC:-${SCRIPT_DIR}/cc-maize.sh}"
    [ -f "$CC_MAIZE" ] || [ -x "$CC_MAIZE" ] || { echo "run-ctest.sh: driver ${CC_MAIZE} not found." >&2; exit 2; }
    MAZM=$(resolve_exe "${BUILD_DIR}/mazm") || {
        echo "run-ctest.sh: mazm not found in ${BUILD_DIR}; run scripts/run-tests.sh first." >&2; exit 2; }
    MAIZE=$(resolve_exe "${BUILD_DIR}/maize") || {   # maize-225/230: SDL-free console build (no WSLg window)
        echo "run-ctest.sh: maize (console build) not found in ${BUILD_DIR}; run scripts/run-tests.sh first." >&2; exit 2; }
    # maize-360: bake --bare into every maize/maizeg invocation. quesOS is now the default
    # boot ROM, so a plain `maize <image>` boots quesOS and runs <image> on top of it; every
    # pre-360 fixture here launches a raw VM image directly, which --bare preserves. A single
    # exec-wrapper per binary (same technique as the MAIZE_JIT leg) bakes --bare in, so the ~89
    # call sites need no edits. The wrapper execs the REAL binary, so argv[0] (and the
    # maizeg-beside-argv0 presenter lookup) is unchanged. BARE_MAIZE keeps the bare, non-JIT
    # wrapper for the JIT-equivalence tests below, which deliberately bypass the JIT wrap but
    # still need to run bare. New maize-360 default-ROM fixtures use DEFAULT_MAIZE (the raw
    # binary, no --bare) instead. Keep in sync with run-tests.{sh,ps1}.
    mkdir -p "${BUILD_DIR}/ctest-run"
    DEFAULT_MAIZE="$MAIZE"                                  # raw binary, no --bare (default-ROM fixtures)
    BARE_MAIZE="${BUILD_DIR}/ctest-run/maize-bare-wrap.sh"
    {
        echo '#!/bin/sh'
        echo "exec \"${MAIZE}\" --bare \"\$@\""
    } > "$BARE_MAIZE"
    chmod +x "$BARE_MAIZE"
    MAIZE="$BARE_MAIZE"
    # maize-330: optional JIT leg, same env contract as run-tests.{sh,ps1}. MAIZE_JIT=1
    # runs every C-fixture execution under --jit; MAIZE_JIT=check under --jit-check. It layers
    # on the --bare wrapper above (maize-360), so a JIT run is bare + jit.
    if [ -n "${MAIZE_JIT:-}" ]; then
        _jit_flag="--jit"
        if [ "${MAIZE_JIT}" = "check" ]; then _jit_flag="--jit-check"; fi
        mkdir -p "${BUILD_DIR}/ctest-run"
        _jit_wrap="${BUILD_DIR}/ctest-run/maize-jit-wrap.sh"
        {
            echo '#!/bin/sh'
            echo "export MAIZE_JIT_QUIET=1"
            echo "exec \"${MAIZE}\" ${_jit_flag} --jit-threshold ${MAIZE_JIT_THRESHOLD:-50} \"\$@\""
        } > "$_jit_wrap"
        chmod +x "$_jit_wrap"
        MAIZE="$_jit_wrap"
        echo "run-ctest.sh: running maize under ${_jit_flag} (threshold ${MAIZE_JIT_THRESHOLD:-50})"
    fi
    # maize-249: the graphical build. add_executable(maizeg ...) is unconditional (CMakeLists.txt:26)
    # and, with MAIZE_DISPLAY off (every CI preset), links no SDL and runs headless, so invoking it
    # directly is CI-safe on BOTH legs (Linux and Windows/MSYS2). Used by run_launcher_per_binary to
    # prove maizeg reads ~/.maize/maizeg.config while the console maize reads ~/.maize/maize.config.
    MAIZEG=$(resolve_exe "${BUILD_DIR}/maizeg") || {
        echo "run-ctest.sh: maizeg (graphical build) not found in ${BUILD_DIR}; run scripts/run-tests.sh first." >&2; exit 2; }
    # maize-360: run_launcher_per_binary runs `$MAIZEG <image>` directly, so it needs --bare
    # too (same wrapper technique as MAIZE above).
    _maizeg_bare="${BUILD_DIR}/ctest-run/maizeg-bare-wrap.sh"
    {
        echo '#!/bin/sh'
        echo "exec \"${MAIZEG}\" --bare \"\$@\""
    } > "$_maizeg_bare"
    chmod +x "$_maizeg_bare"
    MAIZEG="$_maizeg_bare"
    MZLD=$(resolve_exe "${BUILD_DIR}/mzld") || {
        echo "run-ctest.sh: mzld not found in ${BUILD_DIR}; run scripts/run-tests.sh first." >&2; exit 2; }
    # maize-382 (operator ruling, Option B): the guest builders. The nine quesOS call
    # sites and the two userland call sites below drive `mzcc build-quesos` /
    # `mzcc build-userland` instead of os/quesos/build-quesos.sh and
    # userland/build-userland.sh, so every guest compile rides mzcc's per-translation-unit
    # object cache (maize-274, src/mzcc_cache.c) rather than recompiling quesOS nine times
    # and the 43-program userland set twice per suite run. This binding is deliberately
    # INDEPENDENT of MAIZE_CC/CC_MAIZE above: MAIZE_CC still governs only the single-file
    # driver compiles (demo_child*.c, argcheck.c, the wave2_launch_* drivers), and the
    # quesOS/userland migration is unconditional, in every environment. Resolved with the
    # same die-if-missing precedent MAZM/MAIZE/MZLD already set (OQ 10250).
    MZCC=$(resolve_exe "${BUILD_DIR}/mzcc") || {
        echo "run-ctest.sh: mzcc not found in ${BUILD_DIR}; run scripts/run-tests.sh first." >&2; exit 2; }

    # maize-376: --ctest-setup snapshots everything resolved above into a sourceable
    # file and exits. Under the WSL native mirror this runs INSIDE the mirror (the
    # re-exec above happened with the argument vector intact), so the paths written
    # here point at the mirror and every per-fixture invocation works on native storage
    # while ctest itself drives from the original build directory.
    if [ -n "$CTEST_SETUP_FILE" ]; then
        mkdir -p "$(dirname "$CTEST_SETUP_FILE")"
        # Single-quote every value and escape any embedded single quote, so a path with
        # spaces or punctuation (the Windows leg) round-trips through `.` intact.
        _ctest_env_kv() {
            printf "%s='%s'\n" "$1" "$(printf '%s' "$2" | sed "s/'/'\\\\''/g")"
        }
        {
            echo "# Generated by run-ctest.sh --ctest-setup (maize-376); do not edit."
            echo "# Rewritten once per ctest invocation by the ctest_guest_env fixture."
            _ctest_env_kv SCRIPT_DIR     "$SCRIPT_DIR"
            _ctest_env_kv REPO_ROOT      "$REPO_ROOT"
            _ctest_env_kv CTEST_DIR      "$CTEST_DIR"
            _ctest_env_kv RT_DIR         "$RT_DIR"
            _ctest_env_kv QBE_DIR        "$QBE_DIR"
            _ctest_env_kv CPROC_DIR      "$CPROC_DIR"
            _ctest_env_kv PRESET         "$PRESET"
            _ctest_env_kv BUILD_DIR      "$BUILD_DIR"
            _ctest_env_kv WORK_DIR       "$WORK_DIR"
            _ctest_env_kv CC_MAIZE       "$CC_MAIZE"
            _ctest_env_kv MAZM           "$MAZM"
            _ctest_env_kv MAIZE          "$MAIZE"
            _ctest_env_kv BARE_MAIZE     "$BARE_MAIZE"
            _ctest_env_kv DEFAULT_MAIZE  "$DEFAULT_MAIZE"
            _ctest_env_kv MAIZEG         "$MAIZEG"
            _ctest_env_kv MZLD           "$MZLD"
            _ctest_env_kv MZCC           "$MZCC"
            echo "SKIP_BUILD=1"
            # Every nested harness call inside a --ctest-env fixture (cc-maize.sh, the
            # guest builders) is already inside the resolved tree, so tell it not to
            # mirror again. Without this a nested cc-maize.sh under /mnt would attempt
            # its own rsync from inside a fixture, which is the concurrent-rsync hazard
            # the setup/env split exists to remove.
            echo "MAIZE_NATIVE_MIRROR_ACTIVE=1; export MAIZE_NATIVE_MIRROR_ACTIVE"
            # D14: the mirror is a git-less file tree, so nested builds must read the
            # pinned submodule SHAs from the environment rather than from git.
            for _k in MAIZE_KEY_QBE MAIZE_KEY_CPROC MAIZE_KEY_SBASE MAIZE_KEY_OKSH; do
                eval "_kv=\${${_k}:-}"
                _ctest_env_kv "$_k" "$_kv"
                echo "export ${_k}"
            done
        } > "$CTEST_SETUP_FILE"
        echo "run-ctest.sh: ctest environment written to ${CTEST_SETUP_FILE}"
        exit 0
    fi
fi

# maize-221: non-interactive stdin for every test child, so the console VM's
# framebuffer-takeover trap (interactive-tty only) never fires on the headless
# doom self-checks regardless of how this script is launched. See run-tests.sh.
exec 0</dev/null

mkdir -p "${WORK_DIR}"

# maize-382 (decision D10/D12): per-fixture elapsed time, so the next optimization
# targets a measured hot spot instead of an inferred one. Every top-level fixture
# invocation below runs through mz_timed, which appends "<label> <seconds>" to
# TIMING_LOG and the summary block at the end of this script prints them slowest
# first. Whole-second granularity via `date +%s` keeps this portable across Linux,
# macOS, and Git Bash/MSYS (GNU-only `date +%s.%N` is not).
# maize-376: under --only the timing log is per-label, not the one shared
# fixture-timings.log. The shared file is truncated at startup, so N concurrent
# --ctest-env invocations under -jN would each clobber the others' entries; the label
# is unique per test, so a per-label path is race-free by construction. ctest reports
# its own per-test durations anyway, which is what the ctest path actually reads.
TIMING_LOG="${WORK_DIR}/fixture-timings.log"
if [ -n "$ONLY" ]; then
    mkdir -p "${WORK_DIR}/ctest-timings"
    TIMING_LOG="${WORK_DIR}/ctest-timings/${ONLY}.log"
fi
: > "$TIMING_LOG"

# maize-376 (cycle 2 blocker): the fixture-compile scratch directory. compile_c keys
# its output image and its compile log on the SOURCE basename, and several fixtures
# that are now separate ctest tests compile the same source. run_ctest "hello",
# run_image_resolution, run_launcher_defaults and run_launcher_per_binary all compile
# ctest/hello.c; run_args_test, run_image_resolution and run_launcher_defaults all
# compile ctest/args.c; run_launcher_config_mount and run_launcher_per_binary both
# compile cat_hostfs.c and cat_home_hostfs.c; and run_ctest "kilo_xalloc_die" shares
# kilo_xalloc_die.c with the exit-status test that drives the same source. Serially
# inside one process those repeats are harmless rewrites. Under `ctest -jN` they are
# separate processes writing one fixed path by truncate-then-write while a sibling
# reads it, which is a torn read waiting to happen even though every writer produces
# byte-identical content.
#
# Redirecting compile_c's artifacts (and the .out/.exp compare scratch beside them)
# into a per-label directory removes the contention instead of serializing it, and it
# removes the whole class rather than the four instances that exist today: the label is
# unique per test (the CMake drift guard enforces that), so no future fixture can
# reintroduce the collision by picking a source basename another fixture already uses.
# A RESOURCE_LOCK would have cost the parallelism this card exists to buy. With ONLY
# unset (a plain human or CI run of the whole script) this is $WORK_DIR itself, so the
# serial path writes exactly the paths it wrote before.
CC_WORK_DIR="$WORK_DIR"
if [ -n "$ONLY" ]; then
    CC_WORK_DIR="${WORK_DIR}/ctest-scratch/${ONLY}"
    mkdir -p "$CC_WORK_DIR"
fi

# maize-376: the --only dispatch guard. All 80 top-level fixture dispatches below take
# the form `_mz_want "<label>" && mz_timed "<label>" ...`, so `ctest` can run exactly one
# of them per test process. That count is 80 rather than the 78 `mz_timed` statements
# this card's spec counted: kilo_hl_tab_comment and kilo_hl_space_comment were dispatched
# outside mz_timed entirely, so they were untimed, and decision 10312 records the ruling
# that both are routed through the same guard as the other 78 and registered as their own
# tests. cmake/MaizeCTest.cmake's drift guard reads these 80 labels and fails the
# configure if its own registration list disagrees with them.
#
# With ONLY unset (the plain human and CI entry point) this returns 0 on the first
# test and the statement runs exactly as it did before, so a bare run-ctest.sh is
# behavior-identical to the pre-conversion script.
#
# `cond && action` is deliberate rather than an `if`: under `set -e` the failure of a
# command that is not the LAST command of an AND-OR list is ignored (POSIX XCU 2.11),
# so a non-matching _mz_want neither aborts the script nor leaves a nonzero status
# behind, it just falls through to the next statement. Verified against both this
# project's `sh` targets (Git Bash sh and dash).
MZ_ONLY_MATCHED=0
_mz_want() {
    if [ -z "${ONLY:-}" ]; then
        return 0
    fi
    if [ "${ONLY}" = "$1" ]; then
        MZ_ONLY_MATCHED=1
        return 0
    fi
    return 1
}

# Exit-status transparent under `set -eu`: "$@" runs as a plain command in mz_timed's
# body, so a fixture whose nonzero return aborts the script today still aborts it at
# the same point, and a fixture that completes sees no control-flow change at all.
mz_timed() {
    _mzt_label="$1"; shift
    _mzt_t0=$(date +%s)
    "$@"
    _mzt_rc=$?
    _mzt_t1=$(date +%s)
    printf '%s %s\n' "$_mzt_label" "$((_mzt_t1 - _mzt_t0))" >> "$TIMING_LOG"
    return "$_mzt_rc"
}

FAIL_COUNT=0
TOTAL=0

# Compile a C fixture to a runnable .mzx by delegating to the shared driver via its
# no-run default with an explicit `-o <path>` (maize-96). cc-maize.sh owns the whole pipeline end to end
# (tr -> cpp -> cproc-qbe -> normalize -> qbe -t maize -> mazm -c -> mzld over the
# crt0/syscall + C runtime (errno/string/ctype/stdio/stdlib/dirent) set); this harness just asks for the linked image
# and runs it. On success sets BIN to the linked .mzx and returns 0; on failure prints
# a [FAIL] line, bumps FAIL_COUNT, and returns 1. Shared by the stdout runner
# (run_ctest), the exit-status runner (run_exit_status_test), and the argv runner
# (run_args_test) so all three exercise the identical toolchain path.
compile_c() {
    name="$1"
    src="${CTEST_DIR}/${name}.c"

    if [ ! -f "$src" ]; then
        echo "[FAIL] ${name}: missing source fixture" >&2
        FAIL_COUNT=$((FAIL_COUNT + 1))
        return 1
    fi

    # $CC_WORK_DIR is $WORK_DIR on the whole-script path and a per-label subdirectory
    # under --only, so two ctest tests that compile the same source cannot contend.
    mzx="${CC_WORK_DIR}/${name}.mzx"
    if ! "$CC_MAIZE" --preset "$PRESET" -o "$mzx" "$src" \
        >"${CC_WORK_DIR}/${name}.cc.log" 2>&1 || [ ! -f "$mzx" ]; then
        echo "[FAIL] ${name}: C compile failed"; cat "${CC_WORK_DIR}/${name}.cc.log" >&2
        FAIL_COUNT=$((FAIL_COUNT + 1)); return 1
    fi
    BIN="$mzx"
    return 0
}

# maize-304: compile one fixture through $CC_MAIZE, bounded by a fail-fast `timeout`
# (mirroring quesos_ac_case's existing `timeout 90` on the RUN step below; this is the
# same guard on the COMPILE step, which previously had none). Used by the three
# heaviest fixture-compile loops (run_quesos_ac_fixtures, run_quesos94_fixtures,
# run_userland94_fixtures), each of which shells out to $CC_MAIZE dozens of times back
# to back and was the mechanical hole that let a Windows/MSYS `dofork ... Resource
# temporarily unavailable` condition stall the harness 35+ minutes with no bound
# (maize-304 spec section 0).
#
# On success (rc 0) returns 0 silently, appending the compile's own output to
# ${_cmb_log} exactly as the unwrapped call did before. On failure, echoes ONE of three
# [FAIL] lines depending on cause, so an operator scanning CI output does not have to
# guess:
#   - exit 124 (timeout's own "I killed the child" code): "compile timed out ...
#     (possible fork-resource exhaustion)"
#   - the dofork/"Resource temporarily unavailable" signature found in the captured
#     log: "compile failed (fork-resource exhaustion: ...)"
#   - anything else: the original generic "compile failed"
# then cats the accumulated log to stderr (unchanged from the prior per-call-site
# behavior) and returns 1; callers bump TOTAL/FAIL_COUNT and return/continue exactly as
# they did around the old bare call.
#
# Code-review fix (maize-304 cycle 2): plain `timeout N cmd` sends ONE SIGTERM and then
# WAITS for the child to exit; a compile wedged in the exact `dofork ... Resource
# temporarily unavailable` condition this card targets may not respond to SIGTERM (the
# stuck fork can leave the process in a state where its normal signal handling never
# runs), so an unwrapped `timeout` can silently reproduce the original unbounded stall.
# `-k GRACE` arms a SIGKILL that fires GRACE seconds after the SIGTERM if the child is
# still alive, which is the actual backstop for a child that ignores TERM. Confirmed
# `-k` is supported by this host's `timeout` (GNU coreutils 8.32 via git-bash). Per
# `timeout`'s own documented exit-status contract, a plain TERM-caused timeout exits
# 124, but when the KILL escalation actually has to fire (the child ignored TERM) the
# exit status becomes 128+9=137 (the "died from SIGKILL" status) instead of 124;
# confirmed directly against a SIGTERM-ignoring stub on this host. Both codes are
# treated as the same "timed out" diagnostic below.
#
# Test-stage finding (comment #3142, cycle 4): a single-fixture compile through this
# path was observed at ~80-100s on this host, comfortably under the prior 120s default
# but with thin margin (as little as ~20s) for a slower host or a noisier moment.
# Bumped to 180s for real headroom; still a generous ceiling relative to observed cost,
# not a tight ratchet, and still env-overridable.
MAIZE_FIXTURE_COMPILE_TIMEOUT="${MAIZE_FIXTURE_COMPILE_TIMEOUT:-180}"
MAIZE_FIXTURE_COMPILE_KILL_GRACE="${MAIZE_FIXTURE_COMPILE_KILL_GRACE:-10}"
cc_maize_compile_bounded() {
    _cmb_label="$1"; _cmb_out="$2"; _cmb_src="$3"; _cmb_log="$4"
    set +e
    timeout -k "$MAIZE_FIXTURE_COMPILE_KILL_GRACE" "$MAIZE_FIXTURE_COMPILE_TIMEOUT" \
        "$CC_MAIZE" --preset "$PRESET" -o "$_cmb_out" "$_cmb_src" >>"$_cmb_log" 2>&1
    _cmb_rc=$?
    set -e
    if [ "$_cmb_rc" -eq 0 ]; then
        return 0
    fi
    if [ "$_cmb_rc" -eq 124 ] || [ "$_cmb_rc" -eq 137 ]; then
        echo "[FAIL] ${_cmb_label} compile timed out after ${MAIZE_FIXTURE_COMPILE_TIMEOUT}s (possible fork-resource exhaustion)"
    elif grep -qiE 'dofork.*Resource temporarily unavailable' "$_cmb_log" 2>/dev/null; then
        echo "[FAIL] ${_cmb_label} compile failed (fork-resource exhaustion: dofork Resource temporarily unavailable)"
    else
        echo "[FAIL] ${_cmb_label} compile failed"
    fi
    cat "$_cmb_log" >&2
    return 1
}

run_ctest() {
    name="$1"
    expfile="${CTEST_DIR}/${name}.expected"
    TOTAL=$((TOTAL + 1))

    if [ ! -f "$expfile" ]; then
        echo "[FAIL] ${name}: missing expected fixture" >&2
        FAIL_COUNT=$((FAIL_COUNT + 1))
        return
    fi

    compile_c "$name" || return
    bin="$BIN"

    # Compare stdout against the committed fixture. Exact byte match is required
    # (this verifies the greeting AND the trailing newline puts appends); the only
    # tolerance is maize appending ONE extra trailing newline on Linux (documented
    # in src/maize.cpp and handled the same way by run-tests). So: exact cmp, else
    # accept iff the two agree once trailing newlines are stripped.
    out="${CC_WORK_DIR}/${name}.out"
    exp="${CC_WORK_DIR}/${name}.exp"
    "$MAIZE" "$bin" > "$out" 2>/dev/null || true
    # Strip CR from the fixture too, so a CRLF checkout of *.expected can't
    # cause a spurious mismatch (defense in depth with .gitattributes). (maize-62)
    tr -d '\r' < "$expfile" > "$exp"
    if cmp -s "$out" "$exp" \
    || { [ "$(cat "$out")" = "$(cat "$exp")" ]; }; then
        echo "[PASS] ${name}"
    else
        echo "[FAIL] ${name}"
        echo "        expected: \"$(cat "$exp")\""
        echo "        actual:   \"$(cat "$out")\""
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi
}

# maize-58 exit-status observability. A code path DISTINCT from the stdout compare
# above: compile a C fixture whose main returns a fixed nonzero constant, run maize
# on it, and capture $? IMMEDIATELY on the very next line -- before any tr/cmp/cat
# can clobber it -- then assert it equals the expected status. set -eu is active and
# maize now exits nonzero ON PURPOSE, so the invocation is guarded with set +e / set
# -e (NOT `|| true`, which would erase the very status under test).
run_exit_status_test() {
    name="$1"
    expected_status="$2"
    TOTAL=$((TOTAL + 1))

    compile_c "$name" || return
    bin="$BIN"

    set +e
    "$MAIZE" "$bin" >/dev/null 2>&1
    status=$?
    set -e

    if [ "$status" -eq "$expected_status" ]; then
        echo "[PASS] ${name} (exit status ${status})"
    else
        echo "[FAIL] ${name} (exit status)"
        echo "        expected: ${expected_status}"
        echo "        actual:   ${status}"
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi
}

# maize-60 argc/argv/envp end-to-end. Compiles ctest/args.c, then runs it with the
# working directory set to WORK_DIR (which holds the compiled args.mzb) and the image
# named by the FIXED RELATIVE path `args.mzb`, so argv[0] is the deterministic string
# `args.mzb` rather than an absolute build path. Passes one --env-populated pair via
# -e/--env plus another env entry and two guest args, then diffs stdout against
# ctest/args.expected with the same exact-cmp-else-trailing-newline-tolerant compare
# run_ctest uses. This asserts the whole chain: launcher block construction, RS-points-
# at-argc, crt0 marshalling into R0/R1/R2, and the argc-bounded argv loop (a wrong argc
# would change the printed line count). The guest environment is exactly the two --env
# values -- the host's ambient environment is never inherited.
run_args_test() {
    name="args"
    expfile="${CTEST_DIR}/${name}.expected"
    TOTAL=$((TOTAL + 1))

    if [ ! -f "$expfile" ]; then
        echo "[FAIL] ${name}: missing expected fixture" >&2
        FAIL_COUNT=$((FAIL_COUNT + 1))
        return
    fi

    compile_c "$name" || return
    # compile_c wrote ${CC_WORK_DIR}/args.mzx; run from that directory so argv[0] is the
    # bare relative string `args.mzx` whichever scratch directory this invocation uses.

    out="${CC_WORK_DIR}/${name}.out"
    exp="${CC_WORK_DIR}/${name}.exp"
    ( cd "$CC_WORK_DIR" && "$MAIZE" --env GREETING=hi --env TARGET=maize args.mzx alpha beta ) \
        > "$out" 2>/dev/null || true
    tr -d '\r' < "$expfile" > "$exp"
    if cmp -s "$out" "$exp" \
    || { [ "$(cat "$out")" = "$(cat "$exp")" ]; }; then
        echo "[PASS] ${name}"
    else
        echo "[FAIL] ${name}"
        echo "        expected: \"$(cat "$exp")\""
        echo "        actual:   \"$(cat "$out")\""
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi
}

# maize-246 host-launcher bare-image-name resolution. The launcher resolves a bare
# (extension-less) <image> by trying the exact name, then name.mzx, then name.mzb
# (the guest-side exec_forms order, decision 9084); an already-extensioned name is
# tried exactly as given with no cascade. A compiled hello.mzx is the reusable
# payload: load_mzx keys off the image's magic bytes, not the filename, so a byte
# copy renamed .mzb loads and runs identically (no flat-binary encoder needed).
# Every bare/candidate name here is a SINGLE path component (no '/') so MSYS2's
# argument auto-conversion cannot rewrite it before the native maize.exe sees it.
run_image_resolution() {
    name="image_resolution"
    TOTAL=$((TOTAL + 1))

    compile_c "hello" || return
    hello_mzx="$BIN"
    compile_c "args" || return
    args_mzx="$BIN"
    # compile_c reassigns the global `name` (no `local` in POSIX sh); restore it.
    name="image_resolution"

    hello_exp=$(tr -d '\r' < "${CTEST_DIR}/hello.expected")
    ok=1

    # (1) bare .mzx hit: only imgres_mzx.mzx present; `maize imgres_mzx` runs it.
    cp "$hello_mzx" "${WORK_DIR}/imgres_mzx.mzx"
    set +e
    out1=$( cd "$WORK_DIR" && "$MAIZE" imgres_mzx 2>/dev/null )
    set -e
    [ "$out1" = "$hello_exp" ] || ok=0

    # (2) bare .mzb hit: only imgres_mzb.mzb present; `maize imgres_mzb` runs it.
    cp "$hello_mzx" "${WORK_DIR}/imgres_mzb.mzb"
    set +e
    out2=$( cd "$WORK_DIR" && "$MAIZE" imgres_mzb 2>/dev/null )
    set -e
    [ "$out2" = "$hello_exp" ] || ok=0

    # (3) both-exist precedence: .mzx (hello) and .mzb (args) share the base name;
    # the .mzx must win (tried first). args' first stdout line is its argv[0]
    # (`imgres_both`), which hello never prints, so the two are distinguishable.
    cp "$hello_mzx" "${WORK_DIR}/imgres_both.mzx"
    cp "$args_mzx"  "${WORK_DIR}/imgres_both.mzb"
    set +e
    out3=$( cd "$WORK_DIR" && "$MAIZE" imgres_both 2>/dev/null )
    set -e
    [ "$out3" = "$hello_exp" ] || ok=0

    # (4) exact-name-with-extension untouched: `maize imgres_exact.mzx` is
    # byte-identical to the normal run_ctest "hello" behavior; no cascade fires.
    cp "$hello_mzx" "${WORK_DIR}/imgres_exact.mzx"
    set +e
    out4=$( cd "$WORK_DIR" && "$MAIZE" imgres_exact.mzx 2>/dev/null )
    set -e
    [ "$out4" = "$hello_exp" ] || ok=0

    # (5) nonexistent bare name: exit 2, stderr names all three candidates.
    set +e
    err5=$( cd "$WORK_DIR" && "$MAIZE" nosuchimage_246 2>&1 >/dev/null )
    rc5=$?
    set -e
    [ "$rc5" -eq 2 ] || ok=0
    printf '%s' "$err5" | grep -qF "nosuchimage_246" || ok=0
    printf '%s' "$err5" | grep -qF "nosuchimage_246.mzx" || ok=0
    printf '%s' "$err5" | grep -qF "nosuchimage_246.mzb" || ok=0

    # (6) nonexistent extensioned name: exit 2, stderr names ONLY the given path;
    # no .mzx/.mzb-appended phantoms (the cascade must not fire for a .mzb name).
    set +e
    err6=$( cd "$WORK_DIR" && "$MAIZE" nosuchimage_246.mzb 2>&1 >/dev/null )
    rc6=$?
    set -e
    [ "$rc6" -eq 2 ] || ok=0
    printf '%s' "$err6" | grep -qF "nosuchimage_246.mzb" || ok=0
    if printf '%s' "$err6" | grep -qF "nosuchimage_246.mzb.mzx"; then ok=0; fi
    if printf '%s' "$err6" | grep -qF "nosuchimage_246.mzb.mzb"; then ok=0; fi

    # (7) argv[0] preservation: a bare resolved name runs the args fixture; the
    # first stdout line (args prints argv[0]) is the TYPED bare name, not the
    # resolved .mzx path.
    cp "$args_mzx" "${WORK_DIR}/imgres_argv0.mzx"
    set +e
    out7=$( cd "$WORK_DIR" && "$MAIZE" imgres_argv0 2>/dev/null | head -n 1 )
    set -e
    [ "$out7" = "imgres_argv0" ] || ok=0

    if [ "$ok" -eq 1 ]; then
        echo "[PASS] ${name}"
    else
        echo "[FAIL] ${name}"
        echo "        (1) bare .mzx:    \"${out1}\" (want \"${hello_exp}\")"
        echo "        (2) bare .mzb:    \"${out2}\""
        echo "        (3) precedence:   \"${out3}\""
        echo "        (4) exact ext:    \"${out4}\""
        echo "        (5) missing bare: rc=${rc5} stderr=\"${err5}\""
        echo "        (6) missing ext:  rc=${rc6} stderr=\"${err6}\""
        echo "        (7) argv0:        \"${out7}\" (want \"imgres_argv0\")"
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi
}

# maize-77 W^X negative case (AC 7147): mzld must reject an executable section that
# is also writable. mazm only ever emits canonical per-kind attrs (CODE = R+X), so a
# W+X object cannot be authored in source; instead we take the linked runtime's
# crt0.mzo (whose sole section is CODE) and flip its section-attrs byte to add
# ATTR_WRITE, turning R+X into W+X, then confirm mzld refuses it. The .mzo section
# header layout (src/maize_obj.h): 48-byte object header, then 40-byte section
# headers; the first section header's attrs byte is at offset 48 + 4 (name_off) + 1
# (kind) = 53. CODE's default attrs 0x0B (EXEC|READ|ALLOC) OR ATTR_WRITE (0x04) =
# 0x0F. A vacuous guard (never rejecting) fails this test.
run_wx_reject_test() {
    name="wx_reject"
    expected="writable and executable"
    TOTAL=$((TOTAL + 1))

    # Re-assemble crt0 to a .mzo inline. The shared RT-object loop moved into
    # cc-maize.sh (maize-96), so this probe now builds its own CODE-only object base
    # rather than reusing one the harness assembled.
    cp "${RT_DIR}/crt0.mazm" "${WORK_DIR}/wx_crt0.mazm"
    if ! "$MAZM" -c "${WORK_DIR}/wx_crt0.mazm" >"${WORK_DIR}/wx_crt0.mazm.log" 2>&1 \
    || [ ! -f "${WORK_DIR}/wx_crt0.mzo" ]; then
        echo "[FAIL] ${name}: could not assemble crt0.mzo probe base" >&2
        cat "${WORK_DIR}/wx_crt0.mazm.log" >&2
        FAIL_COUNT=$((FAIL_COUNT + 1)); return
    fi

    probe="${WORK_DIR}/wx_probe.mzo"
    cp "${WORK_DIR}/wx_crt0.mzo" "$probe"
    printf '\017' | dd of="$probe" bs=1 seek=53 count=1 conv=notrunc >/dev/null 2>&1

    log=$(mktemp)
    if "$MZLD" -o "${WORK_DIR}/wx_probe.mzx" "$probe" >"$log" 2>&1; then
        ec=0
    else
        ec=$?
    fi
    actual=$(cat "$log")
    rm -f "$log"
    if [ "$ec" -ne 0 ] && printf '%s' "$actual" | grep -qF "$expected"; then
        echo "[PASS] ${name} (mzld rejects W+X)"
    else
        echo "[FAIL] ${name}"
        echo "        expected mzld reject containing: \"${expected}\""
        echo "        actual (exit ${ec}):             \"${actual}\""
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi
}

# maize-111 default-produce self-check. Exercises the reworked no-run DEFAULT (a bare
# `cc-maize.sh <file.c>` with no -r and no -o): it must (a) exit 0, (b) leave
# <base>.mzx beside the source copy, and (c) NOT run the program (no guest stdout on the
# driver's stdout). A known-good fixture (hello.c) is copied into WORK_DIR so the
# beside-source produce lands in the scratch dir, not the tracked ctest/ tree. The
# produced image is then run through maize to confirm it is a valid, runnable .mzx.
run_default_produce_test() {
    name="default_produce"
    TOTAL=$((TOTAL + 1))

    copy="${WORK_DIR}/dp_hello.c"
    cp "${CTEST_DIR}/hello.c" "$copy"
    produced="${copy%.c}.mzx"
    rm -f "$produced"

    set +e
    drv_out=$("$CC_MAIZE" --preset "$PRESET" "$copy" 2>"${WORK_DIR}/dp.err")
    drv_rc=$?
    set -e

    if [ "$drv_rc" -ne 0 ]; then
        echo "[FAIL] ${name}: driver exited ${drv_rc}"; cat "${WORK_DIR}/dp.err" >&2
        FAIL_COUNT=$((FAIL_COUNT + 1)); return
    fi
    if [ ! -f "$produced" ]; then
        echo "[FAIL] ${name}: no ${produced} produced beside the source"
        FAIL_COUNT=$((FAIL_COUNT + 1)); return
    fi
    if [ -n "$drv_out" ]; then
        echo "[FAIL] ${name}: default produce ran the program (unexpected stdout)"
        echo "        stdout: \"${drv_out}\""
        FAIL_COUNT=$((FAIL_COUNT + 1)); return
    fi
    # Confirm the produced image is a valid, runnable .mzx.
    if ! "$MAIZE" "$produced" >/dev/null 2>&1; then
        echo "[FAIL] ${name}: produced image did not run under maize"
        FAIL_COUNT=$((FAIL_COUNT + 1)); return
    fi
    echo "[PASS] ${name} (produced ${produced##*/}, did not run)"
}

# maize-111 driver -r self-check. Exercises the reworked RUN axis THROUGH the driver
# (distinct from run_exit_status_test, which compiles then runs maize directly): a bare
# `cc-maize.sh -r exitcode.c` must run the linked image from scratch and propagate the
# guest exit code (42). Captured under set +e with $? on the very next line (same
# discipline as run_exit_status_test) so the status under test survives set -eu. Also
# asserts no .mzx is left beside ctest/exitcode.c (-r runs from scratch, no persist).
# maize-376: this fixture used to drive the driver against the COMMITTED source
# ctest/exitcode.c and assert that no exitcode.mzx was left beside it. ${CTEST_DIR} is
# not preset-scoped, so that made ctest/exitcode.mzx a fixed shared path two concurrent
# harness invocations (two presets, two agent worktrees, two operators) could race on,
# which is the same shape as the wrapper-script race this card closes. The driver writes
# its output beside its INPUT, so compiling a per-preset sandbox COPY of the source moves
# both the write and the stray-file assertion under ${WORK_DIR} without weakening either.
run_driver_run_mode_test() {
    name="driver_run"
    TOTAL=$((TOTAL + 1))

    src="${WORK_DIR}/driver_run_exitcode.c"
    cp "${CTEST_DIR}/exitcode.c" "$src"
    stray="${src%.c}.mzx"
    rm -f "$stray"

    set +e
    "$CC_MAIZE" --preset "$PRESET" -r "$src" >/dev/null 2>"${WORK_DIR}/dr.err"
    status=$?
    set -e

    if [ "$status" -ne 42 ]; then
        echo "[FAIL] ${name}: driver -r exit status"
        echo "        expected: 42"
        echo "        actual:   ${status}"; cat "${WORK_DIR}/dr.err" >&2
        FAIL_COUNT=$((FAIL_COUNT + 1)); return
    fi
    if [ -f "$stray" ]; then
        echo "[FAIL] ${name}: -r left a persistent ${stray} (should run from scratch)"
        rm -f "$stray"
        FAIL_COUNT=$((FAIL_COUNT + 1)); return
    fi
    echo "[PASS] ${name} (driver -r propagated exit ${status}, left no .mzx)"
}

# maize-114 hostfs acceptance (doc section 8). Each runner prepares a host fixture
# tree under WORK_DIR, invokes maize with the appropriate --mount grant (host path
# translated to native form for the Windows leg), and asserts stdout. Exit-code
# capture follows the same set +e / status-on-next-line discipline as
# run_exit_status_test (never `|| true` on the status under test). The cat and ls
# scenarios must pass on BOTH Linux and Windows (operator ruling OQ 7850: Linux in CI,
# Windows verified locally at Test; the Windows CI lane is follow-up maize-117).

# Strip maize's one-extra-trailing-newline-on-Linux artifact and any blank lines, so a
# sorted/one-line compare is not perturbed by it (same tolerance run_ctest applies).
CAT_PAYLOAD='hostfs cat payload line'

run_hostfs_cat() {
    name="cat_hostfs"
    TOTAL=$((TOTAL + 1))
    compile_c "$name" || return
    bin="$BIN"

    root="${WORK_DIR}/hostfs_cat"
    rm -rf "$root"; mkdir -p "$root/ro"
    printf '%s\n' "$CAT_PAYLOAD" > "$root/ro/payload.txt"
    nat=$(host_to_native "$root/ro")

    set +e
    actual=$("$MAIZE" --no-root --mount "${nat}=/ro:ro" "$bin" 2>/dev/null)
    set -e
    expected=$(printf '%s\n' "$CAT_PAYLOAD")

    if [ "$actual" = "$expected" ]; then
        echo "[PASS] ${name}"
    else
        echo "[FAIL] ${name}"
        echo "        expected: \"${expected}\""
        echo "        actual:   \"${actual}\""
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi
}

run_hostfs_ls() {
    name="ls_hostfs"
    TOTAL=$((TOTAL + 1))
    compile_c "$name" || return
    bin="$BIN"

    root="${WORK_DIR}/hostfs_ls"
    rm -rf "$root"; mkdir -p "$root/ro"
    printf 'x\n' > "$root/ro/payload.txt"
    printf 'x\n' > "$root/ro/alpha.txt"
    printf 'x\n' > "$root/ro/beta.txt"
    nat=$(host_to_native "$root/ro")

    set +e
    actual=$("$MAIZE" --no-root --mount "${nat}=/ro:ro" "$bin" 2>/dev/null | grep -v '^$' | sort)
    set -e
    expected=$(printf 'alpha.txt\nbeta.txt\npayload.txt\n' | sort)

    if [ "$actual" = "$expected" ]; then
        echo "[PASS] ${name}"
    else
        echo "[FAIL] ${name}"
        echo "        expected: \"${expected}\""
        echo "        actual:   \"${actual}\""
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi
}

run_hostfs_escape() {
    name="escape_hostfs"
    TOTAL=$((TOTAL + 1))
    compile_c "$name" || return
    bin="$BIN"

    root="${WORK_DIR}/hostfs_esc"
    rm -rf "$root"; mkdir -p "$root/esc"
    printf 'secret\n' > "$root/escape_target.txt"
    # A host symlink inside the mount pointing OUTSIDE it (Linux/macOS). On Windows,
    # MSYS's default `ln -s` writes a plain regular file carrying MSYS-only symlink
    # metadata (not an NTFS reparse point) unless MSYS=winsymlinks:nativestrict is
    # set; that pseudo-symlink is opaque to Win32 CreateFile, so the hostfs Win32
    # backend's reparse-point check never fires and the file opens as ordinary
    # content, silently defeating this test. Force a real NTFS reparse-point
    # symlink so the existing FILE_ATTRIBUTE_REPARSE_POINT rejection applies; on a
    # host without symlink privilege the create fails and no file is left behind,
    # which the fixture still treats as a denied (ENOENT) path either way.
    MSYS=winsymlinks:nativestrict ln -s "$root/escape_target.txt" "$root/esc/esclink" 2>/dev/null || true
    nat=$(host_to_native "$root/esc")

    set +e
    actual=$("$MAIZE" --no-root --mount "${nat}=/esc:ro" "$bin" 2>/dev/null | grep -v '^$')
    set -e

    if [ "$actual" = "escape: PASS" ]; then
        echo "[PASS] ${name}"
    else
        echo "[FAIL] ${name}"
        echo "        expected: \"escape: PASS\""
        echo "        actual:   \"${actual}\""
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi
}

run_hostfs_stat() {
    name="stat_hostfs"
    TOTAL=$((TOTAL + 1))
    compile_c "$name" || return
    bin="$BIN"

    root="${WORK_DIR}/hostfs_stat"
    rm -rf "$root"; mkdir -p "$root/ro"
    printf '0123456789\n' > "$root/ro/payload.txt"   # exactly 11 bytes
    nat=$(host_to_native "$root/ro")

    set +e
    actual=$("$MAIZE" --no-root --mount "${nat}=/ro:ro" "$bin" 2>/dev/null | grep -v '^$')
    set -e

    if [ "$actual" = "stat: PASS" ]; then
        echo "[PASS] ${name}"
    else
        echo "[FAIL] ${name}"
        echo "        expected: \"stat: PASS\""
        echo "        actual:   \"${actual}\""
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi
}

run_hostfs_rofs() {
    name="rofs_hostfs"
    TOTAL=$((TOTAL + 1))
    compile_c "$name" || return
    bin="$BIN"

    root="${WORK_DIR}/hostfs_rofs"
    rm -rf "$root"; mkdir -p "$root/ro"
    printf 'payload\n' > "$root/ro/payload.txt"
    nat=$(host_to_native "$root/ro")

    set +e
    actual=$("$MAIZE" --no-root --mount "${nat}=/ro:ro" "$bin" 2>/dev/null | grep -v '^$')
    set -e

    if [ "$actual" = "rofs: PASS" ]; then
        echo "[PASS] ${name}"
    else
        echo "[FAIL] ${name}"
        echo "        expected: \"rofs: PASS\""
        echo "        actual:   \"${actual}\""
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi
}

# maize-120 FILE* stdio + dirent acceptance. Exercises the file-backed FILE* layer
# (fopen/fread/fwrite/fseek/ftell/fclose), opendir/readdir/closedir, and sprintf over
# host mounts. A 4096-byte binary file cycling all values 0x00..0xFF is pre-written to
# a :ro mount (the DOOM/WAD read pattern, so any text-mode mangling of 0x0A/0x0D is
# caught); a :rw mount takes the write round-trip and the flush-on-exit proof. Two
# invocations: the normal run does the four checks + returns from main without fclose
# (the atexit-registered __stdio_flush_all must land unclosed.dat), and the `noflush`
# run fwrites then _Exit()s (bypasses atexit), so noflush.dat must stay empty. Follows
# the same Linux-in-CI / Windows-verified-at-Test precedent as the other hostfs runners.
run_hostfs_stdio() {
    name="stdio_hostfs"
    TOTAL=$((TOTAL + 1))
    compile_c "$name" || return
    bin="$BIN"

    root="${WORK_DIR}/hostfs_stdio"
    rm -rf "$root"; mkdir -p "$root/ro" "$root/rw"

    # Pre-write the DOOM-shaped binary: 4096 bytes cycling every value 0x00..0xFF.
    # LC_ALL=C forces awk's %c to emit raw bytes (not UTF-8 multibyte) on either host.
    LC_ALL=C awk 'BEGIN{for(i=0;i<4096;i++)printf "%c", i%256}' > "$root/ro/bin.dat"
    nat_ro=$(host_to_native "$root/ro")
    nat_rw=$(host_to_native "$root/rw")

    set +e
    actual=$("$MAIZE" --no-root --mount "${nat_ro}=/ro:ro" --mount "${nat_rw}=/rw:rw" "$bin" \
        2>/dev/null | grep -v '^$')
    set -e

    ok=1
    [ "$actual" = "stdio: PASS" ] || ok=0

    # AC 8276 positive: the un-fclosed buffered write stream's bytes landed on exit.
    exp_unclosed='flush-on-exit-proof'
    got_unclosed=$(cat "$root/rw/unclosed.dat" 2>/dev/null)
    [ "$got_unclosed" = "$exp_unclosed" ] || ok=0

    # AC 8276 negative: a _Exit() run must NOT flush, so noflush.dat exists but is empty.
    set +e
    "$MAIZE" --no-root --mount "${nat_rw}=/rw:rw" "$bin" noflush >/dev/null 2>&1
    set -e
    if [ ! -f "$root/rw/noflush.dat" ] || [ -s "$root/rw/noflush.dat" ]; then
        ok=0
    fi

    if [ "$ok" -eq 1 ]; then
        echo "[PASS] ${name}"
    else
        echo "[FAIL] ${name}"
        echo "        stdout:   \"${actual}\" (want \"stdio: PASS\")"
        echo "        unclosed: \"${got_unclosed}\" (want \"${exp_unclosed}\")"
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi
}

# maize-151 path-mutating positive: the DOOM save shape (mkdir a dir, create+write a
# file in it, rename it, stat/read it back, unlink it) end-to-end against a WRITABLE
# filesystem. Uses the DEFAULT sandbox root (redirected to a fresh scratch dir via
# --root, so the run is deterministic and the cwd is /home/user, NOT --no-root), and the
# fixture uses relative paths so they resolve against that cwd exactly as DOOM does.
run_hostfs_savefs() {
    name="savefs_hostfs"
    TOTAL=$((TOTAL + 1))
    compile_c "$name" || return
    bin="$BIN"

    # A fresh, empty sandbox root; maize creates the /home/user + /tmp skeleton in it.
    root="${WORK_DIR}/hostfs_savefs"
    rm -rf "$root"; mkdir -p "$root"
    nat=$(host_to_native "$root")

    set +e
    actual=$("$MAIZE" --root "${nat}" "$bin" 2>/dev/null | grep -v '^$')
    set -e

    if [ "$actual" = "savefs: PASS" ]; then
        echo "[PASS] ${name}"
    else
        echo "[FAIL] ${name}"
        echo "        expected: \"savefs: PASS\""
        echo "        actual:   \"${actual}\""
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi
}

# maize-151 path-mutating security: the write-gate + confinement guarantees for the
# mutating ops. A default sandbox root (mounted "/" rw via --root) plus a :ro overlay at
# /ro (NOT --no-root). The guest asserts mkdir/rename on the :ro mount are EROFS and a
# `..` open cannot reach a host file outside every mount; the harness additionally proves
# a `..`-laden mkdir created nothing OUTSIDE the sandbox on the host, and the :ro mkdir
# left no directory behind.
run_hostfs_savefs_neg() {
    name="savefs_neg_hostfs"
    TOTAL=$((TOTAL + 1))
    compile_c "$name" || return
    bin="$BIN"

    esc="${WORK_DIR}/hostfs_savefs_neg"
    rm -rf "$esc"; mkdir -p "$esc/sandbox" "$esc/ro"
    printf 'x\n' > "$esc/ro/a.txt"
    # A host file OUTSIDE every mount: the sandbox is $esc/sandbox and the :ro mount is
    # $esc/ro, so a file at $esc/ is unreachable by any guest path.
    printf 'secret\n' > "$esc/escape_target.txt"
    nat_root=$(host_to_native "$esc/sandbox")
    nat_ro=$(host_to_native "$esc/ro")

    set +e
    actual=$("$MAIZE" --root "${nat_root}" --mount "${nat_ro}=/ro:ro" "$bin" 2>/dev/null | grep -v '^$')
    set -e

    ok=1
    [ "$actual" = "savefsneg: PASS" ] || ok=0
    # The `..` mkdir must not have created anything OUTSIDE the sandbox on the host (a
    # contained landing INSIDE the sandbox is fine; the escape location is what matters).
    [ ! -e "$esc/pwned" ] || ok=0
    # The :ro mkdir must not have created its target under the :ro host dir.
    [ ! -e "$esc/ro/newdir" ] || ok=0

    if [ "$ok" -eq 1 ]; then
        echo "[PASS] ${name}"
    else
        echo "[FAIL] ${name}"
        echo "        expected: \"savefsneg: PASS\" + host escape locations empty"
        echo "        actual:   \"${actual}\""
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi
}

# maize-179 ftruncate acceptance: shrink drops the tail (kilo save-after-shrink is now
# byte-exact), extend zero-fills, a negative length is EINVAL, and ftruncate on a fd from
# a :ro mount is EROFS. Same --root sandbox + :ro overlay grant shape as savefs_neg.
run_hostfs_truncate() {
    name="truncate_hostfs"
    TOTAL=$((TOTAL + 1))
    compile_c "$name" || return
    bin="$BIN"

    root="${WORK_DIR}/hostfs_truncate"
    rm -rf "$root"; mkdir -p "$root/sandbox" "$root/ro"
    printf 'payload\n' > "$root/ro/payload.txt"
    nat_root=$(host_to_native "$root/sandbox")
    nat_ro=$(host_to_native "$root/ro")

    set +e
    actual=$("$MAIZE" --root "${nat_root}" --mount "${nat_ro}=/ro:ro" "$bin" 2>/dev/null | grep -v '^$')
    set -e

    if [ "$actual" = "truncate: PASS" ]; then
        echo "[PASS] ${name}"
    else
        echo "[FAIL] ${name}"
        echo "        expected: \"truncate: PASS\""
        echo "        actual:   \"${actual}\""
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi
}

# maize-255 merged root directory listing: with a real "/" mount present (--root),
# opening "/" merges the top-level names of every other granted mount into the
# physical listing, deduped against any physical collision (D1-D5, D8). Grants
# /bin (a new guest name, proves the merge) and /tmp (colliding with the
# sandbox's own auto-created /tmp, proves dedup with zero extra fixture setup:
# the physical /tmp already exists under every --root sandbox). root_merge_hostfs.c
# uses a small read buffer to force multiple getdents64 calls across the
# mount-name-phase-to-physical-phase boundary (AC 9292) and exercises the
# lseek rewind (AC 9293) and lseek-EINVAL (AC 9303) postures in the same run.
# stat_mountpoint_hostfs.c then runs against the same launch shape to confirm
# fstat on the /bin mount (no physical counterpart under the sandbox) is
# unaffected (D6, AC 9288).
run_hostfs_root_merge() {
    name="root_merge_hostfs"
    TOTAL=$((TOTAL + 1))
    compile_c "$name" || return
    bin="$BIN"

    root="${WORK_DIR}/hostfs_root_merge"
    rm -rf "$root"; mkdir -p "$root/sandbox" "$root/bin" "$root/tmp_other"
    nat_root=$(host_to_native "$root/sandbox")
    nat_bin=$(host_to_native "$root/bin")
    nat_tmp=$(host_to_native "$root/tmp_other")

    set +e
    # MSYS2_ARG_CONV_EXCL exempts the /bin, /tmp guest paths (embedded in the
    # combined "host=guest:mode" --mount value) from the Windows-leg MSYS2
    # POSIX->Windows argv rewrite (semicolon-separated per the run_doom_render /
    # quesos94 fs_forward precedent, scripts/run-ctest.sh:1396/2170); the host
    # side of each --mount value is already a native path via host_to_native and
    # is left untouched.
    out=$(MSYS2_ARG_CONV_EXCL='/bin;/tmp' "$MAIZE" --root "${nat_root}" \
        --mount "${nat_bin}=/bin:ro" --mount "${nat_tmp}=/tmp:ro" "$bin" 2>/dev/null)
    set -e

    n1=$(printf '%s\n' "$out" | grep '^N1:' | sed 's/^N1://' | sort)
    n2=$(printf '%s\n' "$out" | grep '^N2:' | sed 's/^N2://' | sort)
    lseekbad=$(printf '%s\n' "$out" | grep '^lseekbad:')
    # maize-360: the sandbox-root skeleton now also creates /etc (holding the shipped
    # /etc/profile), so the merged "/" listing gains an etc entry alongside bin/home/tmp.
    # maize-374: the skeleton also creates /root (HOME for the now-coherent root login),
    # so the merged listing gains a root entry too.
    expected=$(printf 'bin\netc\nhome\nroot\ntmp\n' | sort)

    ok=1
    [ "$n1" = "$expected" ] || ok=0
    [ "$n2" = "$expected" ] || ok=0
    [ "$lseekbad" = "lseekbad: PASS" ] || ok=0

    if [ "$ok" -eq 1 ]; then
        echo "[PASS] ${name}"
    else
        echo "[FAIL] ${name}"
        echo "        expected N1/N2: \"${expected}\""
        echo "        actual N1:      \"${n1}\""
        echo "        actual N2:      \"${n2}\""
        echo "        lseekbad:       \"${lseekbad}\""
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi

    name2="stat_mountpoint_hostfs"
    TOTAL=$((TOTAL + 1))
    compile_c "$name2" || return
    bin2="$BIN"

    set +e
    actual2=$(MSYS2_ARG_CONV_EXCL='/bin;/tmp' "$MAIZE" --root "${nat_root}" \
        --mount "${nat_bin}=/bin:ro" --mount "${nat_tmp}=/tmp:ro" "$bin2" 2>/dev/null | grep -v '^$')
    set -e

    if [ "$actual2" = "statmount: PASS" ]; then
        echo "[PASS] ${name2}"
    else
        echo "[FAIL] ${name2}"
        echo "        expected: \"statmount: PASS\""
        echo "        actual:   \"${actual2}\""
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi

    # maize-255 REOPEN negative cases: the merged mount-name listing must appear ONLY
    # in the root "/" listing, never in a deeper directory that merely resolves through
    # the "/" root mount. Reproduces the operator's exact "double doom" shape: a --root
    # sandbox with a PHYSICAL /home/user/doom subdir, plus /bin and /doom OVERLAY grants
    # (the two names that were bleeding into every listing). /tmp is left as the
    # sandbox's own physical dir (NO overlay) so its listing reproduces the operator's
    # "ls /tmp shows bin+doom" case. Distinct marker files pin each listing to an exact
    # set, so a stray synthetic name (or a duplicated physical one) is caught precisely.
    name3="root_merge_neg_hostfs"
    TOTAL=$((TOTAL + 1))
    compile_c "$name3" || return
    bin3="$BIN"

    nroot="${WORK_DIR}/hostfs_root_merge_neg"
    rm -rf "$nroot"
    mkdir -p "$nroot/sandbox/home/user/doom" "$nroot/sandbox/tmp" \
        "$nroot/binhost" "$nroot/doomhost"
    printf 'x\n' > "$nroot/sandbox/home/user/sub_marker.txt"
    printf 'x\n' > "$nroot/sandbox/tmp/tmp_marker.txt"
    printf 'x\n' > "$nroot/binhost/bin_marker.txt"
    printf 'x\n' > "$nroot/doomhost/doom_marker.txt"
    nat_nroot=$(host_to_native "$nroot/sandbox")
    nat_nbin=$(host_to_native "$nroot/binhost")
    nat_ndoom=$(host_to_native "$nroot/doomhost")

    set +e
    negout=$(MSYS2_ARG_CONV_EXCL='/bin;/doom' "$MAIZE" --root "${nat_nroot}" \
        --mount "${nat_nbin}=/bin:ro" --mount "${nat_ndoom}=/doom:ro" "$bin3" 2>/dev/null)
    set -e

    neg_root=$(printf '%s\n' "$negout" | grep '^ROOT:' | sed 's/^ROOT://' | sort)
    neg_sub=$(printf '%s\n' "$negout" | grep '^SUB:' | sed 's/^SUB://' | sort)
    neg_tmp=$(printf '%s\n' "$negout" | grep '^TMP:' | sed 's/^TMP://' | sort)
    neg_doom=$(printf '%s\n' "$negout" | grep '^DOOM:' | sed 's/^DOOM://' | sort)
    neg_bin=$(printf '%s\n' "$negout" | grep '^BIN:' | sed 's/^BIN://' | sort)

    exp_root=$(printf 'bin\ndoom\netc\nhome\nroot\ntmp\n' | sort)  # maize-360 adds /etc, maize-374 adds /root
    exp_sub=$(printf 'doom\nsub_marker.txt\n' | sort)     # doom exactly ONCE, no bin
    exp_tmp=$(printf 'tmp_marker.txt\n' | sort)           # no bin/doom
    exp_doom=$(printf 'doom_marker.txt\n' | sort)         # overlay root, no synthetic
    exp_bin=$(printf 'bin_marker.txt\n' | sort)           # overlay root, no synthetic

    negok=1
    [ "$neg_root" = "$exp_root" ] || negok=0
    [ "$neg_sub" = "$exp_sub" ]   || negok=0
    [ "$neg_tmp" = "$exp_tmp" ]   || negok=0
    [ "$neg_doom" = "$exp_doom" ] || negok=0
    [ "$neg_bin" = "$exp_bin" ]   || negok=0

    if [ "$negok" -eq 1 ]; then
        echo "[PASS] ${name3}"
    else
        echo "[FAIL] ${name3}"
        echo "        ROOT expected: \"${exp_root}\"  actual: \"${neg_root}\""
        echo "        SUB  expected: \"${exp_sub}\"  actual: \"${neg_sub}\""
        echo "        TMP  expected: \"${exp_tmp}\"  actual: \"${neg_tmp}\""
        echo "        DOOM expected: \"${exp_doom}\"  actual: \"${neg_doom}\""
        echo "        BIN  expected: \"${exp_bin}\"  actual: \"${neg_bin}\""
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi
}

# maize-138 multi-file compile/link. Builds N C sources into one .mzx through the
# extended driver's multi-source path (an explicit -o over several positional
# sources), runs the linked image, and diffs stdout against ctest/<name>.expected
# with the SAME exact-cmp-else-trailing-newline-tolerant compare run_ctest uses. The
# fixture (multifile_main.c + multifile_lib.c) forces a genuine cross-object link: a
# function call and a shared global whose definition sits in the OTHER object, so a
# link that only worked for one self-contained body would fail it. $2 is a
# space-separated list of bare fixture source names under ctest/.
run_multi_ctest() {
    name="$1"
    srcs="$2"
    expfile="${CTEST_DIR}/${name}.expected"
    TOTAL=$((TOTAL + 1))

    if [ ! -f "$expfile" ]; then
        echo "[FAIL] ${name}: missing expected fixture" >&2
        FAIL_COUNT=$((FAIL_COUNT + 1))
        return
    fi

    # Expand the bare fixture names into ctest/ source paths (safe positional args).
    set --
    for s in $srcs; do
        set -- "$@" "${CTEST_DIR}/${s}"
    done

    mzx="${WORK_DIR}/${name}.mzx"
    if ! "$CC_MAIZE" --preset "$PRESET" -o "$mzx" "$@" \
        >"${WORK_DIR}/${name}.cc.log" 2>&1 || [ ! -f "$mzx" ]; then
        echo "[FAIL] ${name}: multi-source C compile failed"; cat "${WORK_DIR}/${name}.cc.log" >&2
        FAIL_COUNT=$((FAIL_COUNT + 1)); return
    fi

    out="${WORK_DIR}/${name}.out"
    exp="${WORK_DIR}/${name}.exp"
    "$MAIZE" "$mzx" > "$out" 2>/dev/null || true
    tr -d '\r' < "$expfile" > "$exp"
    if cmp -s "$out" "$exp" \
    || { [ "$(cat "$out")" = "$(cat "$exp")" ]; }; then
        echo "[PASS] ${name}"
    else
        echo "[FAIL] ${name}"
        echo "        expected: \"$(cat "$exp")\""
        echo "        actual:   \"$(cat "$out")\""
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi
}

# maize-138 negative link case (AC 8231). An EXPECT-FAIL runner in the same spirit as
# run_wx_reject_test: force the multi-source path (via --sources) with ONLY the main
# TU, omitting the sibling object that defines add_and_tag / shared_counter. mzld must
# reject the unresolved cross-object references ("undefined symbol ...") with a nonzero
# exit and leave no image, proving the cross-object link genuinely resolves rather than
# a single body happening to self-contain everything. A vacuous linker (never failing)
# fails this test.
run_multi_link_reject_test() {
    name="multifile_undef"
    expected="undefined symbol"
    TOTAL=$((TOTAL + 1))

    listfile="${WORK_DIR}/${name}.sources"
    # maize-278 coexistence seam: a --sources listfile is FILE CONTENTS read
    # directly by the driver, not an argv the shell can convert. Under MSYS/Git
    # Bash, CTEST_DIR is a POSIX /c/... path; argv paths get auto-mangled to
    # native form for a native exe, but paths written into a file do not. The
    # native mzcc (design D6: no path-translation layer) fopen()s the line
    # verbatim and reports "no such file" on /c/..., the wrong failure mode for
    # this negative test. cc-maize.sh absorbed this via its native_path/cygpath
    # layer; mzcc deliberately deleted it. Write the native form here so BOTH
    # drivers resolve the entry: host_to_native is a no-op off MSYS (Linux keeps
    # the POSIX path unchanged) and cc-maize.sh's win_to_posix re-normalizes the
    # native form back, so this is safe for either MAIZE_CC selection.
    printf '%s\n' "$(host_to_native "${CTEST_DIR}/multifile_main.c")" > "$listfile"
    mzx="${WORK_DIR}/${name}.mzx"
    rm -f "$mzx"
    log="${WORK_DIR}/${name}.cc.log"

    set +e
    "$CC_MAIZE" --preset "$PRESET" -o "$mzx" --sources "$listfile" >"$log" 2>&1
    ec=$?
    set -e

    if [ "$ec" -ne 0 ] && [ ! -f "$mzx" ] && grep -qF "$expected" "$log"; then
        echo "[PASS] ${name} (mzld rejects the omitted cross-object definition)"
    else
        echo "[FAIL] ${name}"
        echo "        expected nonzero exit + no image + stderr containing: \"${expected}\""
        echo "        actual (exit ${ec}): \"$(cat "$log")\""
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi
}

# maize-138 usage-error cases (AC 8235). EXPECT-FAIL runner asserting a nonzero exit
# and an expected stderr substring for a multi-source invocation that violates the
# CLI contract. $1 = label, $2 = expected stderr substring, remaining args = the
# driver arguments under test.
run_multi_usage_test() {
    name="$1"
    expected="$2"
    shift 2
    TOTAL=$((TOTAL + 1))

    log="${WORK_DIR}/${name}.usage.log"
    set +e
    "$CC_MAIZE" --preset "$PRESET" "$@" >"$log" 2>&1
    ec=$?
    set -e

    if [ "$ec" -ne 0 ] && grep -qF "$expected" "$log"; then
        echo "[PASS] ${name} (usage error, exit ${ec})"
    else
        echo "[FAIL] ${name}"
        echo "        expected nonzero exit + stderr containing: \"${expected}\""
        echo "        actual (exit ${ec}): \"$(cat "$log")\""
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi
}

# maize-143 flag-safety regression, at the QBE-IR layer where the fix lives. The
# nonzero-offset CAddr lowering (CP <label> ; LEA $<off>) MUST be flag-neutral: the
# isel successor-phi-argument pass can land a $sym+K materialization at a block end,
# between a fused flag-only CMP and its Jcc, so a flag-clobbering ADD/SUB there would
# corrupt the branch. cproc cannot express a bare CAddr-con phi argument from C (it
# keeps locals in memory, so a C loop materializes the address eagerly in the body or
# in a jmp-terminated ternary arm, never between a fused CMP and its Jcc). So this
# hazard is exercised from hand-written QBE: ctest/caddroff_flag.qbe folds
# `add $garr, K` onto a loop back edge, and ctest/caddroff_flag_crt.mazm is a minimal
# self-contained entry stub that exits with flagmain's return (0 == the fused loop
# summed correctly under the flag-neutral LEA, nonzero == a flag-clobber miscompile).
# Linked with ONLY the stub + qbe body, so the C-runtime link (scripts/cc-maize.sh)
# is NOT duplicated here. Swapping the LEA for an ADD/SUB fails this fixture.
run_qbe_flag() {
    name="caddroff_flag"
    TOTAL=$((TOTAL + 1))

    QBE=$(resolve_exe "${QBE_DIR}/obj/qbe") || {
        echo "[FAIL] ${name}: qbe not built (${QBE_DIR}/obj/qbe)" >&2
        FAIL_COUNT=$((FAIL_COUNT + 1)); return; }

    src="${CTEST_DIR}/${name}.qbe"
    crt="${CTEST_DIR}/${name}_crt.mazm"
    body_mazm="${WORK_DIR}/${name}.body.mazm"
    body_mzo="${WORK_DIR}/${name}.body.mzo"
    crt_copy="${WORK_DIR}/${name}_crt.mazm"
    crt_mzo="${WORK_DIR}/${name}_crt.mzo"
    mzx="${WORK_DIR}/${name}.mzx"

    if ! "$QBE" -t maize "$src" > "$body_mazm" 2>"${WORK_DIR}/${name}.qbe.log"; then
        echo "[FAIL] ${name}: qbe -t maize failed"; cat "${WORK_DIR}/${name}.qbe.log" >&2
        FAIL_COUNT=$((FAIL_COUNT + 1)); return
    fi
    cp "$crt" "$crt_copy"
    if ! "$MAZM" -c "$crt_copy" >"${WORK_DIR}/${name}.crt.log" 2>&1 \
    || ! "$MAZM" -c "$body_mazm" >"${WORK_DIR}/${name}.body.log" 2>&1; then
        echo "[FAIL] ${name}: mazm -c failed"
        cat "${WORK_DIR}/${name}.crt.log" "${WORK_DIR}/${name}.body.log" >&2
        FAIL_COUNT=$((FAIL_COUNT + 1)); return
    fi
    if ! "$MZLD" -o "$mzx" "$crt_mzo" "$body_mzo" >"${WORK_DIR}/${name}.mzld.log" 2>&1 \
    || [ ! -f "$mzx" ]; then
        echo "[FAIL] ${name}: mzld failed"; cat "${WORK_DIR}/${name}.mzld.log" >&2
        FAIL_COUNT=$((FAIL_COUNT + 1)); return
    fi

    set +e
    "$MAIZE" "$mzx" >/dev/null 2>&1
    status=$?
    set -e

    if [ "$status" -eq 0 ]; then
        echo "[PASS] ${name} (flag-neutral LEA lowering; fused exit test intact)"
    else
        echo "[FAIL] ${name} (exit ${status}: the nonzero-offset CAddr lowering clobbered the fused-branch flags)"
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi
}

echo "=== C toolchain end-to-end (cproc -> qbe -t maize -> mazm -c -> mzld -> maize) ==="
_mz_want "hello" && mz_timed "hello" run_ctest "hello"
_mz_want "capstone" && mz_timed "capstone" run_ctest "capstone"
_mz_want "globals" && mz_timed "globals" run_ctest "globals"
_mz_want "ptrdata" && mz_timed "ptrdata" run_ctest "ptrdata"
_mz_want "ldzfold" && mz_timed "ldzfold" run_ctest "ldzfold"
# maize-101 codegen-gap regressions: bug #1 (void call with args -> spill.c dead
# reg) and bug #3 (&&/ternary phi cycle -> Oswap die), both overlay-only.
_mz_want "voidcall" && mz_timed "voidcall" run_ctest "voidcall"
_mz_want "freelist" && mz_timed "freelist" run_ctest "freelist"
# maize-103 codegen-gap regression: an &local carried DIRECTLY as a loop-carried
# phi argument (freelist's inverse, no opaque() barrier). Pre-fix maize_isel never
# ran fixarg over successor phi args, so the alloc temp reached rega and the phi
# edge became a plain slot MOVE of the local's contents instead of a LEA of its
# address: a silent wrong answer. Overlay-only fix in qbe-maize/isel.c.
_mz_want "addrlocalphi" && mz_timed "addrlocalphi" run_ctest "addrlocalphi"
# maize-136 spilled-operand regression: >11 simultaneously-live values force QBE to
# spill to frame slots, and a loop rotating sixteen loop-carried values drives the
# block-edge slot->slot Ocopy (the PUSH/POP register borrow). Pre-fix the emitter
# die()d on any spilled operand; post-fix it emits the reload / spill-store / slot
# copy paths. Overlay-only fix in qbe-maize/emit.c. Self-checks against 1541762618.
_mz_want "spill" && mz_timed "spill" run_ctest "spill"
# maize-143 CAddr nonzero-offset regression: forms and uses &global_array[K],
# &s.field, "lit"+K, and a &global[K] carried across a fused-branch loop, each with
# a checked result folded into "caddroff: PASS". Pre-fix the emitter die()d on any
# nonzero-offset CAddr con; post-fix isel routes it through a register and emitcopy
# lowers it as CP <label> ; LEA $<off> (flag-neutral). Overlay-only in qbe-maize.
_mz_want "caddroff" && mz_timed "caddroff" run_ctest "caddroff"
# maize-143 flag-safety gate (QBE-IR level; see run_qbe_flag above): the LEA offset
# lowering must be flag-neutral so a $sym+K materialization landing between a fused
# CMP and its Jcc cannot corrupt the branch. Fails if LEA is swapped for ADD/SUB.
_mz_want "run_qbe_flag" && mz_timed "run_qbe_flag" run_qbe_flag
# maize-137 float/double codegen: a self-checking fixture exercising float and
# double arithmetic (+ - * /), all six comparisons in both widths (ordered and
# NaN/unordered), signed int<->float and float<->double conversions (unsigned
# int<->float is out of scope), inline float/double constants, and
# passing/returning float and double across a call boundary. Each sub-result is
# checked (value or exact IEEE bits) so a wrong FP encoding fails the gate.
_mz_want "fp" && mz_timed "fp" run_ctest "fp"
# maize-74 syscall C binding: raw stub direct (AC 7290), wrapper success returns the
# byte count (AC 7291), and error-range translation sets errno + returns -1 (AC 7292).
_mz_want "syscall_raw" && mz_timed "syscall_raw" run_ctest "syscall_raw"
_mz_want "syscall_write" && mz_timed "syscall_write" run_ctest "syscall_write"
_mz_want "syscall_errno" && mz_timed "syscall_errno" run_ctest "syscall_errno"
# maize-327: close on the stdio reservations 0/1/2 returns 0 instead of the spurious
# -EBADF the hostfs fd table used to produce. Every reported line is written to fd 1
# after close(1), so the stdout diff also proves the recorded semantics (bare success:
# the reservation stays usable).
_mz_want "syscall_close" && mz_timed "syscall_close" run_ctest "syscall_close"
# maize-76 freestanding libc slice: string.h (str), ctype.h (ctype), the malloc
# family over the sbrk free-list allocator (malloc), and the sbrk wrapper itself
# (sbrk). Each is a self-checking fixture printing a single PASS line.
_mz_want "str" && mz_timed "str" run_ctest "str"
# maize-216 large-n bulk memory: memcpy/memmove/memset at/over BULK_SYSCALL_THRESHOLD
# route to the host via SYS $F4 (sys_bulk_copy, memmove-safe) / $F5 (sys_bulk_set).
# str.c only exercises the sub-threshold inline word loop; this drives the syscall
# path (aligned/unaligned, both overlap directions, threshold boundary, n==0) and
# self-checks byte-for-byte. One "bulkmem PASS".
_mz_want "bulkmem" && mz_timed "bulkmem" run_ctest "bulkmem"
_mz_want "ctype" && mz_timed "ctype" run_ctest "ctype"
_mz_want "sbrk" && mz_timed "sbrk" run_ctest "sbrk"
_mz_want "malloc" && mz_timed "malloc" run_ctest "malloc"
# maize-146 freestanding headers: fixed-width types + limit/constant macros + bool,
# and (precautionary) the inttypes PRI* format macros over the Maize printf.
_mz_want "stdint" && mz_timed "stdint" run_ctest "stdint"
# maize-297: cproc/qbe miscompiled the equal-width, lower-rank-unsigned usual-
# arithmetic-conversions arm (typecommonreal's TYPELLONG case), so
# MIN(LLONG_MAX, SIZE_MAX) evaluated to -1 instead of LLONG_MAX. Covers the
# constant-folded AND runtime forms of the repro, the full mixed long-long-vs-
# unsigned-long relational matrix, the == / != controls, and an over-fix guard
# (long long vs unsigned int must stay a SIGNED compare).
_mz_want "minmax_signedness" && mz_timed "minmax_signedness" run_ctest "minmax_signedness"
# maize-147 RT headers round 2 for DOOM: includes every new header (strings/math/
# assert/unistd/sys/types/sys/stat), asserts the SEEK_*/EISDIR/S_IF* macro values and
# the off_t/ssize_t/mode_t widths, proves the struct stat byte-ABI (sizeof 144;
# nlink@16/mode@24/size@48 via runtime pointer subtraction), and parses each new decl
# via sizeof(&fn) with NO link dependency (bodies are maize-148). One "rthdrs2: PASS".
_mz_want "rthdrs2" && mz_timed "rthdrs2" run_ctest "rthdrs2"
# maize-149 GNU-attribute strip: a DOOM mapsidedef_t-shaped struct using the
# TRAILING __attribute__((packed)) position (which the pinned cproc rejects)
# compiles through the driver's cpp-step strip, and its sizeof/offsetof asserts
# (sizeof==30, char[8] blocks at 4/12/20, trailing short at 28) prove the natural
# layout is byte-identical to the packed on-disk WAD layout, so the strip is
# run-safe. Prints a single "packed: PASS" line.
_mz_want "packed" && mz_timed "packed" run_ctest "packed"
# maize-100 atexit registry: two handlers registered A-then-B run at exit in LIFO
# order (B, then A) after "main done", proving both that exit() runs the registry
# and the ordering, plus the indirect-call-through-a-runtime-indexed-fnptr-array path.
_mz_want "atexit" && mz_timed "atexit" run_ctest "atexit"
# maize-142 stdlib numeric conversions: atoi/abs/labs/strtol in one self-checking
# fixture (base 10/16/0-autodetect, overflow clamp + ERANGE, endptr/no-conversion,
# invalid-base EINVAL, and the bare-"0x"/"0"-no-digit corners). One "strtol PASS".
_mz_want "strtol" && mz_timed "strtol" run_ctest "strtol"
# maize-141 monotonic ms clock (SYS $F0): a self-checking fixture asserting the
# clock is non-decreasing at fine grain, advances under a bounded busy-spin, and
# reports a plausible (nonzero, < 60 s) delta. Prints a single "clock: PASS" line.
_mz_want "clock" && mz_timed "clock" run_ctest "clock"

# maize-213 palette-blit syscall (SYS $F3): a self-checking fixture proving the
# blit is bit-identical (dst[i] == lut[src[i]], RV == npixels) AND deny-by-default
# secure (oversized npixels -> -EINVAL, a dst/src base+len wrap -> -EFAULT, each
# with no guest write and no crash). Prints a single "palette-blit: PASS" line.
_mz_want "palette_blit_selfcheck" && mz_timed "palette_blit_selfcheck" run_ctest "palette_blit_selfcheck"
# maize-98 varargs / stdarg ABI: a self-checking fixture exercising the register
# save area, va_arg over mixed scalar classes, the register->overflow boundary,
# and va_copy. Prints a single PASS line.
_mz_want "varargs" && mz_timed "varargs" run_ctest "varargs"
# maize-99 variadic printf over the stdarg ABI: direct-emit correctness for every
# conversion (%d %i %u %x %X %c %s %p %%, %ld/%lu/%lx, width + zero-pad, INT_MIN /
# LONG_MIN) matched byte-for-byte, plus an snprintf return/truncation self-check
# and a >256-byte line proving chunked flush. Ends in a single "selfcheck PASS".
_mz_want "printf" && mz_timed "printf" run_ctest "printf"
# maize-144 RT libc gaps for the DOOM boot: printf/sprintf PRECISION (%.Nd min-digits
# incl. the DOOM STCFN%.3d lump shape, %.Ns string truncation, %8.3d width+precision,
# %.0d-of-0 empty, and the untouched %05d path) plus strdup / getenv / qsort / atof,
# all checked silently with inline-computed expected values. One "libcgaps PASS".
_mz_want "libcgaps" && mz_timed "libcgaps" run_ctest "libcgaps"
# maize-148 RT libc round 3 for the DOOM Phase A link: strcasecmp/strncasecmp (tolower),
# fabs via a sign-bit mask (incl. -0.0 -> +0.0 by bit pattern), the sscanf scanf core
# (%d/%x/%f/%s/%c/width/suppress with checked counts + values, a partial match), system
# (-1/0), usleep (no-op), and the remove/mkdir link-only stubs (execute smoke, no value
# assertion; real filesystem ACs are on maize-151). One "libcgaps3 PASS".
_mz_want "libcgaps3" && mz_timed "libcgaps3" run_ctest "libcgaps3"
_mz_want "exitcode" && mz_timed "exitcode" run_exit_status_test "exitcode" 42
# maize-76: abort() terminates with status 134 (128 + SIGABRT(6); no signals).
_mz_want "abort" && mz_timed "abort" run_exit_status_test "abort" 134
# maize-102: an own-TU _Noreturn function (die) calls exit(57); its `hlt` end block
# (and main's tail block, which calls the _Noreturn-declared die) traverse cfg.c
# simpljmp before emit, so a regression in the hlt-guard hunk crashes this at
# compile time rather than passing silently. Proves qbe -t maize parses/lowers hlt.
_mz_want "noreturn" && mz_timed "noreturn" run_exit_status_test "noreturn" 57
# maize-350: kilo's shared geometric-growth arithmetic (kilo_next_cap, a pure
# function) and its checked allocation wrappers. kilo_next_cap prints its growth
# progression over a fixed input sequence covering the 64-row and 4096-byte floors.
# kilo_xalloc_die forces a NULL allocation under KILO_XALLOC_TESTING (no real
# memory exhaustion) and is driven two ways off the one SOURCE: the exact die()
# message on stdout, and the process exit status 1. The source is shared; the
# compiled image is not. These are two separately scheduled ctest tests, so each
# compiles into its own $CC_WORK_DIR (see the comment on that variable).
_mz_want "kilo_next_cap" && mz_timed "kilo_next_cap" run_ctest "kilo_next_cap"
_mz_want "kilo_xalloc_die" && mz_timed "kilo_xalloc_die" run_ctest "kilo_xalloc_die"
_mz_want "kilo_xalloc_die_exit" && mz_timed "kilo_xalloc_die_exit" run_exit_status_test "kilo_xalloc_die" 1
# maize-365: kilo's C highlighter overran row->hl when a tab preceded a //
# comment. The memset fill count at kilo.c:485 used row->size (the raw line
# length) instead of row->rsize (the tab-expanded render length that i and hl
# are both scaled to), so on a line like "A\t//" the count size-i went negative
# and, as a size_t, ran the memset off the 9-byte hl allocation into unmapped
# guest memory (fatal page fault under quesOS, an unbounded runaway write on the
# bare VM's larger flat guest RAM). Each fixture #includes the real
# demos/kilo/kilo.c (kilo's own main renamed out of the way and never called)
# and drives the identical editorSelectSyntaxHighlight/editorInsertRow/
# editorUpdateRow/editorUpdateSyntax on one input line. kilo_hl_tab_comment is
# the pathological tab case: post-fix it prints the exact hl[] bytes then OK;
# pre-fix editorUpdateSyntax never reaches the printf calls (fault under quesOS,
# runaway memset under the bare VM), so stdout never matches the fixture (the
# fail-before/pass-after negative control). kilo_hl_space_comment is the
# space-indented control ("A //", no tab, so rsize == size and the fill count is
# identical before and after the fix), proving the fix leaves already-working
# input unchanged. Run under an explicit `timeout` (not the bare run_ctest
# helper) because a size_t-underflow memset is exactly the runaway-write input a
# bound protects against on a host that does not fault as fast as the production
# incident did.
kilo_hl_case() {
    name="$1"
    expfile="${CTEST_DIR}/${name}.expected"
    TOTAL=$((TOTAL + 1))
    if [ ! -f "$expfile" ]; then
        echo "[FAIL] ${name}: missing expected fixture" >&2
        FAIL_COUNT=$((FAIL_COUNT + 1)); return
    fi
    compile_c "$name" || return
    bin="$BIN"
    out="${CC_WORK_DIR}/${name}.out"
    exp="${CC_WORK_DIR}/${name}.exp"
    set +e
    timeout -k 5 30 "$MAIZE" "$bin" > "$out" 2>/dev/null
    set -e
    tr -d '\r' < "$expfile" > "$exp"
    if cmp -s "$out" "$exp" || { [ "$(cat "$out")" = "$(cat "$exp")" ]; }; then
        echo "[PASS] ${name}"
    else
        echo "[FAIL] ${name}"
        echo "        expected: \"$(cat "$exp")\""
        echo "        actual:   \"$(cat "$out")\""
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi
}
# maize-376: these two were the only top-level fixture dispatches maize-382 left
# outside mz_timed, so they were both untimed and (once --only landed) would have run
# on EVERY per-fixture invocation instead of on their own. Routed through the same
# guard as the other 78, which also gives them their own add_test entries and timings.
_mz_want "kilo_hl_tab_comment" && mz_timed "kilo_hl_tab_comment" kilo_hl_case "kilo_hl_tab_comment"
_mz_want "kilo_hl_space_comment" && mz_timed "kilo_hl_space_comment" kilo_hl_case "kilo_hl_space_comment"
_mz_want "run_args_test" && mz_timed "run_args_test" run_args_test
# maize-246 host-launcher bare-image-name resolution (exact / .mzx / .mzb).
_mz_want "run_image_resolution" && mz_timed "run_image_resolution" run_image_resolution
_mz_want "run_wx_reject_test" && mz_timed "run_wx_reject_test" run_wx_reject_test
# maize-111 CLI-rework self-checks: the new no-run default (produce beside source) and
# the driver -r run-and-propagate path.
_mz_want "run_default_produce_test" && mz_timed "run_default_produce_test" run_default_produce_test
_mz_want "run_driver_run_mode_test" && mz_timed "run_driver_run_mode_test" run_driver_run_mode_test

# maize-114 hostfs acceptance scenarios (cat + ls on both hosts, ..-escape and
# symlink-escape EACCES/ENOENT, :ro write EROFS).
_mz_want "run_hostfs_cat" && mz_timed "run_hostfs_cat" run_hostfs_cat
_mz_want "run_hostfs_ls" && mz_timed "run_hostfs_ls" run_hostfs_ls
_mz_want "run_hostfs_stat" && mz_timed "run_hostfs_stat" run_hostfs_stat
_mz_want "run_hostfs_escape" && mz_timed "run_hostfs_escape" run_hostfs_escape
_mz_want "run_hostfs_rofs" && mz_timed "run_hostfs_rofs" run_hostfs_rofs
# maize-120 FILE* stdio + dirent layer over the hostfs stubs.
_mz_want "run_hostfs_stdio" && mz_timed "run_hostfs_stdio" run_hostfs_stdio
# maize-151 path-mutating syscalls (mkdir/unlink/rename) over the confined hostfs.
_mz_want "run_hostfs_savefs" && mz_timed "run_hostfs_savefs" run_hostfs_savefs
_mz_want "run_hostfs_savefs_neg" && mz_timed "run_hostfs_savefs_neg" run_hostfs_savefs_neg
# maize-179 ftruncate over the confined hostfs (shrink/extend/EINVAL/EROFS).
_mz_want "run_hostfs_truncate" && mz_timed "run_hostfs_truncate" run_hostfs_truncate
# maize-255 merged root listing when a real "/" mount coexists with other grants.
_mz_want "run_hostfs_root_merge" && mz_timed "run_hostfs_root_merge" run_hostfs_root_merge

# maize-121 self-hosted framebuffer terminal headless self-check. The fixture is a
# guest-C program under demos/terminal/ that additionally links the mzdev device-access
# shim (mzdev.mzo), so it is compiled via the driver's opt-in `--dev` flag rather than
# compile_c's fixed RT set. Phase A drives term_write with a fixed ASCII+escape script and
# reads back the guest-RAM framebuffer; phase B injects a known Set-1 scancode sequence via
# `maize --input=keyboard` (mirroring run-tests.sh's run_keyboard_test) and checks the
# echoed glyphs. One "terminal: PASS" line gates both phases, on Linux CI and Windows.
run_terminal_selfcheck() {
    name="terminal"
    TOTAL=$((TOTAL + 1))
    src="${REPO_ROOT}/demos/terminal/terminal_selfcheck.c"

    if [ ! -f "$src" ]; then
        echo "[FAIL] ${name}: missing source fixture" >&2
        FAIL_COUNT=$((FAIL_COUNT + 1))
        return
    fi

    mzx="${WORK_DIR}/terminal_selfcheck.mzx"
    if ! "$CC_MAIZE" --preset "$PRESET" --dev -o "$mzx" "$src" \
        >"${WORK_DIR}/terminal.cc.log" 2>&1 || [ ! -f "$mzx" ]; then
        echo "[FAIL] ${name}: C compile failed"; cat "${WORK_DIR}/terminal.cc.log" >&2
        FAIL_COUNT=$((FAIL_COUNT + 1)); return
    fi

    # Set-1 scancodes: 1E('a'), 2A/1E/AA(shifted 'A'), 02('1'), 2A/02/AA(shifted '!'),
    # 39(space). Octal for printf: 1E=036 2A=052 AA=252 02=002 39=071.
    set +e
    actual=$(printf '\036\052\036\252\002\052\002\252\071' \
        | "$MAIZE" --no-root --input=keyboard "$mzx" 2>/dev/null | grep -v '^$')
    set -e

    if [ "$actual" = "terminal: PASS" ]; then
        echo "[PASS] ${name}"
    else
        echo "[FAIL] ${name}"
        echo "        expected: \"terminal: PASS\""
        echo "        actual:   \"${actual}\""
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi
}

_mz_want "run_terminal_selfcheck" && mz_timed "run_terminal_selfcheck" run_terminal_selfcheck

# maize-140 first-class graphical console headless self-check. Unlike the maize-121
# terminal (a self-hosted guest engine verified by reading guest-RAM pixels), the console
# is host C++ bound to fd 0/1/2, so this fixture drives it through ordinary stdio and the
# harness verifies the RESULT via the grid text dump (--console-dump) plus an injected
# Set-1 scancode stream on stdin (the same channel run_terminal_selfcheck uses). It checks
# the VT-output subset (CUP/EL/ED, LF/CR/BS/HT, right-margin wrap), that ED ESC[2J actually
# cleared (the PRECLEAR token is absent), and that the cooked line read and the raw
# byte-at-a-time read both delivered correctly ("console: PASS"). No device shim (--dev):
# it is a plain stdio + termios program on the default RT set.
run_console_selfcheck() {
    name="console"
    TOTAL=$((TOTAL + 1))
    src="${REPO_ROOT}/demos/console/console_selfcheck.c"

    if [ ! -f "$src" ]; then
        echo "[FAIL] ${name}: missing source fixture" >&2
        FAIL_COUNT=$((FAIL_COUNT + 1))
        return
    fi

    mzx="${WORK_DIR}/console_selfcheck.mzx"
    if ! "$CC_MAIZE" --preset "$PRESET" -o "$mzx" "$src" \
        >"${WORK_DIR}/console.cc.log" 2>&1 || [ ! -f "$mzx" ]; then
        echo "[FAIL] ${name}: C compile failed"; cat "${WORK_DIR}/console.cc.log" >&2
        FAIL_COUNT=$((FAIL_COUNT + 1)); return
    fi

    # Scancodes: 0x23 'h', 0x2C 'Z', 0x0E Backspace, 0x17 'i', 0x1C Enter, 0x2D 'x',
    # 0x3A CapsLock, 0x1E 'a', 0x02 '1' (octal 043 054 016 027 034 055 072 036 002). The
    # cooked read consumes h/Z/BS/i/Enter and the Backspace edits the pending line, so it
    # delivers "hi\n" (the erroneous Z erased); raw mode then returns the single 'x'. The
    # CapsLock make latches Caps Lock (no byte), so the following 'a' raw-reads as 'A'
    # (letters obey Caps Lock) and '1' as '1' (digits do not). This exercises cooked line
    # editing (Backspace) plus echo + deliver-on-Enter plus the alphabetic-only Caps Lock rule.
    set +e
    dump=$(printf '\043\054\016\027\034\055\072\036\002' \
        | "$MAIZE" --no-root --console-dump "$mzx" 2>/dev/null)
    set -e

    ok=1
    for want in "HELLO" "CD" "XQZ" "A       B" "ZZZ" "SGR" "ERA" "console: PASS"; do
        printf '%s\n' "$dump" | grep -qx "$want" || ok=0
    done
    # ED ESC[2J must have wiped the pre-clear token.
    if printf '%s\n' "$dump" | grep -q "PRECLEAR"; then ok=0; fi

    if [ "$ok" -eq 1 ]; then
        echo "[PASS] ${name}"
    else
        echo "[FAIL] ${name}"
        echo "        expected: VT-output markers + \"console: PASS\", no PRECLEAR"
        echo "        actual grid dump:"
        printf '%s\n' "$dump" | sed 's/^/          | /'
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi
}

_mz_want "run_console_selfcheck" && mz_timed "run_console_selfcheck" run_console_selfcheck

# maize-145 DOOM Phase A "it links" gate. Builds the ~50k-line doomgeneric + DOOM tree
# (the doom.sources core set plus the Maize stub platform doomgeneric_maize.c and the
# doom_main.c entry TU) to a .mzx through the real cc-maize.sh multi-source pipeline and
# asserts the image is produced. It does NOT run maize: DOOM's zone / WAD / device /
# render path is Phase B/C, so Phase A's gate is purely "the whole object set resolves
# and links." Like run_terminal_selfcheck it links the mzdev device shim via --dev (the
# DG_* platform seam references fb/kbd ports). doom.sources is entry-free (Phase C reuses
# it verbatim), so the entry TU and the stub platform are passed positionally alongside
# it, exactly as demos/doom/README.md documents.
#
# GRACEFUL SKIP: demos/doom/doomgeneric is a git submodule. A checkout (CI leg or local)
# that did not `git submodule update --init` the demo leaves the source tree absent;
# rather than hard-fail the whole ctest suite on a missing optional demo, this gate
# prints a skip notice and returns without counting a test. Every ci.yml run-ctest leg
# checks out with submodules: recursive, so CI exercises the real link; the skip is only
# a safety net for a partial checkout.
run_doom_link() {
    name="doom-link"
    doom_dir="${REPO_ROOT}/demos/doom"
    sources="${doom_dir}/doom.sources"
    entry="${doom_dir}/doom_main.c"
    platform="${doom_dir}/doomgeneric_maize.c"
    # Submodule presence probe: the doomgeneric core-loop TU. Absent => uninitialized.
    probe="${doom_dir}/doomgeneric/doomgeneric/doomgeneric.c"

    if [ ! -f "$probe" ]; then
        echo "[SKIP] ${name}: demos/doom/doomgeneric submodule not initialized" \
             "(run 'git submodule update --init demos/doom/doomgeneric'); skipping DOOM link gate"
        return
    fi

    TOTAL=$((TOTAL + 1))

    mzx="${WORK_DIR}/doom.mzx"
    log="${WORK_DIR}/doom-link.cc.log"
    rm -f "$mzx"

    # maize-153: carry the 320x200 geometry override the platform layer is written against
    # (DEC-5) so the full tree links at the geometry it will boot at in Phase C. Geometry
    # does not affect linkage, so run_doom_link still PASSES; this only keeps the two DOOM
    # builds (link gate + self-check) consistent. The -D flags require the cc-maize.sh
    # passthrough (DEC-6).
    set +e
    "$CC_MAIZE" --preset "$PRESET" --dev \
        -D DOOMGENERIC_RESX=320 -D DOOMGENERIC_RESY=200 -o "$mzx" \
        --sources "$sources" "$entry" "$platform" >"$log" 2>&1
    ec=$?
    set -e

    if [ "$ec" -eq 0 ] && [ -f "$mzx" ]; then
        echo "[PASS] ${name} ($(wc -c <"$mzx" | tr -d ' ') bytes .mzx)"
    else
        echo "[FAIL] ${name}: DOOM tree failed to link (exit ${ec})"
        cat "$log" >&2
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi
}

_mz_want "run_doom_link" && mz_timed "run_doom_link" run_doom_link

# maize-153 DOOM Phase B headless DG-platform self-check. Links ONLY the platform TU
# doomgeneric_maize.c with the standalone doom_selfcheck.c (a minimal link: no doom.sources,
# no doomgeneric.c, so no double DG_ScreenBuffer/main), plus the mzdev device shim via --dev
# and the RT libc set, all at the 320x200 geometry override (DEC-5, via the cc-maize.sh -D
# passthrough DEC-6). It exercises every DG_* primitive in isolation WITHOUT booting DOOM
# (full boot needs an IWAD, Phase C): framebuffer present + readback, the Set-1 -> DOOM
# keymap over an injected make/break sequence, the ms clock, the libc FILE* WAD-read path on
# a committed binary fixture, and a zone-sized malloc smoke. One "doom: PASS" line gates it.
#
# The committed fixture demos/doom/testdata/doomread.bin is mounted read-only at /ro (the
# same DOOM/WAD :ro read pattern run_hostfs_stdio proves), and the scancode stream is piped
# via `maize --input=keyboard` exactly as run_terminal_selfcheck does.
#
# GRACEFUL SKIP: like run_doom_link, doom_selfcheck.c includes doomgeneric.h from the
# submodule, so a checkout that did not init demos/doom/doomgeneric skips rather than fails.
run_doom_selfcheck() {
    name="doom"
    doom_dir="${REPO_ROOT}/demos/doom"
    selfcheck="${doom_dir}/doom_selfcheck.c"
    platform="${doom_dir}/doomgeneric_maize.c"
    fixture_dir="${doom_dir}/testdata"
    # Submodule presence probe: the doomgeneric core-loop TU. Absent => uninitialized.
    probe="${doom_dir}/doomgeneric/doomgeneric/doomgeneric.c"

    if [ ! -f "$probe" ]; then
        echo "[SKIP] ${name}: demos/doom/doomgeneric submodule not initialized" \
             "(run 'git submodule update --init demos/doom/doomgeneric'); skipping DOOM self-check"
        return
    fi

    TOTAL=$((TOTAL + 1))

    mzx="${WORK_DIR}/doom_selfcheck.mzx"
    log="${WORK_DIR}/doom-selfcheck.cc.log"
    rm -f "$mzx"

    if ! "$CC_MAIZE" --preset "$PRESET" --dev \
        -D DOOMGENERIC_RESX=320 -D DOOMGENERIC_RESY=200 \
        -o "$mzx" "$selfcheck" "$platform" >"$log" 2>&1 || [ ! -f "$mzx" ]; then
        echo "[FAIL] ${name}: self-check C compile/link failed"; cat "$log" >&2
        FAIL_COUNT=$((FAIL_COUNT + 1)); return
    fi

    nat=$(host_to_native "$fixture_dir")

    # Set-1 make/break stream (octal for printf): 1E('a')/9E('a' rel)/48(up)/4B(left)/
    # 4D(right)/50(down)/1D(ctrl->fire)/39(space->use)/1C(enter)/01(esc)/0F(tab).
    set +e
    actual=$(printf '\036\236\110\113\115\120\035\071\034\001\017' \
        | "$MAIZE" --no-root --input=keyboard --mount "${nat}=/ro:ro" "$mzx" 2>/dev/null | grep -v '^$')
    set -e

    if [ "$actual" = "doom: PASS" ]; then
        echo "[PASS] ${name}"
    else
        echo "[FAIL] ${name}"
        echo "        expected: \"doom: PASS\""
        echo "        actual:   \"${actual}\""
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi
}

_mz_want "run_doom_selfcheck" && mz_timed "run_doom_selfcheck" run_doom_selfcheck

# maize-154 DOOM Phase C headless RENDER gate. Distinct from run_doom_selfcheck
# (Phase B: DG_* platform in isolation, no engine boot): this boots the WHOLE
# DOOM engine against a minimal, license-clean SYNTHETIC IWAD and asserts a real
# 3D level actually rendered. The IWAD is produced AT TEST TIME by compiling the
# committed generator demos/doom/tools/make_min_iwad.c with the system cc (D7: the
# auditable artifact is the generator source, never a committed binary; every
# lump byte is generator-computed, zero copied DOOM assets). The generator is C,
# not Python, so this runs on both CI hosts including the Windows MSYS2 lane
# (gcc, no python3).
#
# The render TU doom_render_selfcheck.c links the entry-free doom.sources core +
# the Phase B platform + mzdev (--dev) at the 320x200 geometry override, boots
# via `-iwad /ro/min.wad -warp 1 1 -nomonsters` (DOOM args pass through maize's
# guest-argv), ticks until the level renders, and asserts the 3D VIEWPORT (ABOVE
# the status bar, per OQ3) has >= 2 distinct colors, printing "doom: PASS". Same
# submodule graceful-skip probe as run_doom_link / run_doom_selfcheck.
run_doom_render() {
    name="doom-render"
    doom_dir="${REPO_ROOT}/demos/doom"
    render="${doom_dir}/doom_render_selfcheck.c"
    platform="${doom_dir}/doomgeneric_maize.c"
    sources="${doom_dir}/doom.sources"
    generator="${doom_dir}/tools/make_min_iwad.c"
    # Submodule presence probe: the doomgeneric core-loop TU. Absent => uninitialized.
    probe="${doom_dir}/doomgeneric/doomgeneric/doomgeneric.c"

    if [ ! -f "$probe" ]; then
        echo "[SKIP] ${name}: demos/doom/doomgeneric submodule not initialized" \
             "(run 'git submodule update --init demos/doom/doomgeneric'); skipping DOOM render gate"
        return
    fi

    TOTAL=$((TOTAL + 1))

    # System C compiler (mirrors build-toolchain.sh's pick); present on both CI
    # hosts. Compile the committed generator and run it into WORK_DIR to produce
    # the synthetic IWAD in a directory we then mount read-only at /ro.
    gen_cc="${CC:-}"
    if [ -z "$gen_cc" ]; then
        if command -v cc >/dev/null 2>&1; then gen_cc=cc; else gen_cc=gcc; fi
    fi
    gen_exe="${WORK_DIR}/make_min_iwad"
    if ! "$gen_cc" -O2 -o "$gen_exe" "$generator" >"${WORK_DIR}/doom-render.gen.log" 2>&1; then
        echo "[FAIL] ${name}: min-IWAD generator failed to compile"
        cat "${WORK_DIR}/doom-render.gen.log" >&2
        FAIL_COUNT=$((FAIL_COUNT + 1)); return
    fi

    waddir="${WORK_DIR}/doom-render-wad"
    rm -rf "$waddir"; mkdir -p "$waddir"
    if ! "$gen_exe" "${waddir}/min.wad" >>"${WORK_DIR}/doom-render.gen.log" 2>&1 \
    || [ ! -f "${waddir}/min.wad" ]; then
        echo "[FAIL] ${name}: min-IWAD generation failed"
        cat "${WORK_DIR}/doom-render.gen.log" >&2
        FAIL_COUNT=$((FAIL_COUNT + 1)); return
    fi

    mzx="${WORK_DIR}/doom_render.mzx"
    log="${WORK_DIR}/doom-render.cc.log"
    rm -f "$mzx"
    if ! "$CC_MAIZE" --preset "$PRESET" --dev \
        -D DOOMGENERIC_RESX=320 -D DOOMGENERIC_RESY=200 \
        -o "$mzx" --sources "$sources" "$render" "$platform" >"$log" 2>&1 \
    || [ ! -f "$mzx" ]; then
        echo "[FAIL] ${name}: render-gate C compile/link failed"; cat "$log" >&2
        FAIL_COUNT=$((FAIL_COUNT + 1)); return
    fi

    nat=$(host_to_native "$waddir")
    # maize-154: the guest-side arg `-iwad /ro/min.wad` is a GUEST path, not a host
    # path: it names the WAD at its guest mount point, and maize's hostfs resolves it.
    # Under MSYS2/MinGW (the windows-llvm-mingw CI lane), the runtime rewrites POSIX
    # absolute argv elements into Windows paths before a NATIVE exe sees them, so a
    # bare `/ro/min.wad` reaches maize.exe as e.g. `C:/Program Files/Git/ro/min.wad`;
    # DOOM's D_FindWADByName then fails, I_Errors to stderr (discarded below), and the
    # render never runs. Every OTHER mount test hardcodes its guest path inside the
    # guest C, so this is the one leg that hands maize a guest path on the command line
    # and the one that hit the rewrite. MSYS2_ARG_CONV_EXCL exempts the `/ro` prefix so
    # the guest path passes through verbatim; the --mount host side is already a native
    # `C:\...` path (host_to_native/cygpath -w) and is left untouched, and `$mzx` (a
    # non-/ro host path) is still converted normally. Harmless on non-MSYS shells.
    set +e
    actual=$(MSYS2_ARG_CONV_EXCL='/ro' "$MAIZE" --no-root --mount "${nat}=/ro:ro" "$mzx" \
        -iwad /ro/min.wad -warp 1 1 -nomonsters 2>/dev/null | grep -v '^$')
    set -e

    # The engine prints a banner + per-tick viewport diagnostics; the gate is the
    # exact "doom: PASS" line (a real 3D render), like doom_selfcheck.c.
    if printf '%s\n' "$actual" | grep -qx "doom: PASS"; then
        echo "[PASS] ${name}"
    else
        echo "[FAIL] ${name}"
        echo "        expected a \"doom: PASS\" line (a non-blank 3D viewport render)"
        echo "        actual:   \"$(printf '%s' "$actual" | tail -3 | tr '\n' '|')\""
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi
}

_mz_want "run_doom_render" && mz_timed "run_doom_render" run_doom_render

# maize-193 DOOM LEVEL-TRANSITION gate. Distinct from run_doom_render (Phase C:
# boots ONE level and asserts a single rendered frame): this boots MAP01 of a
# two-map COMMERCIAL synthetic IWAD, drives the level transition (G_ExitLevel ->
# intermission -> G_WorldDone -> next-map load), and asserts MAP02 loads
# (gamemap == 2) and renders. It reproduces (and now guards against) the "maize
# exits at level completion" defect (a qbe-maize register-name-collision
# miscompile of the wi_stuff.c `bp[]` global that corrupted the intermission's
# return address). The IWAD is produced AT TEST TIME by compiling the committed
# generator demos/doom/tools/make_min_iwad.c with the system cc, run with
# --commercial (D7: the auditable artifact is the generator source, never a
# committed binary; every lump byte is generator-computed, zero copied DOOM
# assets). Same submodule graceful-skip probe and CC_MAIZE compile/mount/guest-
# argv shape as run_doom_render. Bounded tick budget (in the harness TU): a
# transition that never completes FAILs rather than hangs.
run_doom_transition() {
    name="doom-transition"
    doom_dir="${REPO_ROOT}/demos/doom"
    transition="${doom_dir}/doom_transition_selfcheck.c"
    platform="${doom_dir}/doomgeneric_maize.c"
    sources="${doom_dir}/doom.sources"
    generator="${doom_dir}/tools/make_min_iwad.c"
    # Submodule presence probe: the doomgeneric core-loop TU. Absent => uninitialized.
    probe="${doom_dir}/doomgeneric/doomgeneric/doomgeneric.c"

    if [ ! -f "$probe" ]; then
        echo "[SKIP] ${name}: demos/doom/doomgeneric submodule not initialized" \
             "(run 'git submodule update --init demos/doom/doomgeneric'); skipping DOOM transition gate"
        return
    fi

    TOTAL=$((TOTAL + 1))

    # System C compiler (mirrors run_doom_render): compile the committed generator
    # and run it with --commercial to produce the two-map DOOM 2 IWAD in a dir we
    # mount read-only at /ro.
    gen_cc="${CC:-}"
    if [ -z "$gen_cc" ]; then
        if command -v cc >/dev/null 2>&1; then gen_cc=cc; else gen_cc=gcc; fi
    fi
    gen_exe="${WORK_DIR}/make_min_iwad_c"
    if ! "$gen_cc" -O2 -o "$gen_exe" "$generator" >"${WORK_DIR}/doom-transition.gen.log" 2>&1; then
        echo "[FAIL] ${name}: min-IWAD generator failed to compile"
        cat "${WORK_DIR}/doom-transition.gen.log" >&2
        FAIL_COUNT=$((FAIL_COUNT + 1)); return
    fi

    waddir="${WORK_DIR}/doom-transition-wad"
    rm -rf "$waddir"; mkdir -p "$waddir"
    if ! "$gen_exe" --commercial "${waddir}/min2.wad" >>"${WORK_DIR}/doom-transition.gen.log" 2>&1 \
    || [ ! -f "${waddir}/min2.wad" ]; then
        echo "[FAIL] ${name}: commercial min-IWAD generation failed"
        cat "${WORK_DIR}/doom-transition.gen.log" >&2
        FAIL_COUNT=$((FAIL_COUNT + 1)); return
    fi

    mzx="${WORK_DIR}/doom_transition.mzx"
    log="${WORK_DIR}/doom-transition.cc.log"
    rm -f "$mzx"
    if ! "$CC_MAIZE" --preset "$PRESET" --dev \
        -D DOOMGENERIC_RESX=320 -D DOOMGENERIC_RESY=200 \
        -o "$mzx" --sources "$sources" "$transition" "$platform" >"$log" 2>&1 \
    || [ ! -f "$mzx" ]; then
        echo "[FAIL] ${name}: transition-gate C compile/link failed"; cat "$log" >&2
        FAIL_COUNT=$((FAIL_COUNT + 1)); return
    fi

    nat=$(host_to_native "$waddir")
    # -iwad /ro/min2.wad is a GUEST path; MSYS2_ARG_CONV_EXCL exempts /ro from the
    # POSIX->Windows argv rewrite (see run_doom_render for the full rationale).
    set +e
    actual=$(MSYS2_ARG_CONV_EXCL='/ro' "$MAIZE" --no-root --mount "${nat}=/ro:ro" "$mzx" \
        -iwad /ro/min2.wad -warp 1 1 -nomonsters 2>/dev/null | grep -v '^$')
    set -e

    # The gate is the exact "doom-transition: PASS" line (gamemap advanced to 2 AND
    # a real 3D render of MAP02's viewport).
    if printf '%s\n' "$actual" | grep -qx "doom-transition: PASS"; then
        echo "[PASS] ${name}"
    else
        echo "[FAIL] ${name}"
        echo "        expected a \"doom-transition: PASS\" line (MAP01 -> MAP02 advance + render)"
        echo "        actual:   \"$(printf '%s' "$actual" | tail -3 | tr '\n' '|')\""
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi
}

_mz_want "run_doom_transition" && mz_timed "run_doom_transition" run_doom_transition

# maize-156 DOOM ENGINE-LEVEL INPUT gate. Distinct from run_doom_render/run_doom_transition
# (which boot the engine but inject ZERO keyboard input) and from run_doom_selfcheck (which
# exercises DG_GetKey in isolation, no engine boot): this boots the WHOLE engine against the
# synthetic min.wad AND injects real Set-1 scancodes through the SAME doomgeneric_maize.c
# DG_GetKey path production input uses, then asserts the injected keys drove an in-SIM state
# change. It closes the maize-155 gap (Ctrl mapped to a keycode no in-game binding matched
# passed the render gate because nothing exercised in-game input end to end). Two MAKE-only
# scancode bytes are piped on stdin via `maize --input=keyboard` (as run_doom_selfcheck does):
# 0x48 (octal 110, KEY_UPARROW -> key_up) and 0x1D (octal 035, Ctrl -> KEY_FIRE, the exact
# maize-155 physical key). A make with no break holds each binding down for the whole run, so
# holding up moves the player (mo->x rises) and holding Ctrl fires the pistol (ammo[am_clip]
# strictly decreases); the harness asserts BOTH within a bounded tick budget. Same non-
# commercial min.wad, submodule-presence [SKIP] guard, --dev compile, mount, guest-argv and
# MSYS2_ARG_CONV_EXCL='/ro' handling as run_doom_render. Gates on the exact "doom-input: PASS".
run_doom_input() {
    name="doom-input"
    doom_dir="${REPO_ROOT}/demos/doom"
    input_harness="${doom_dir}/doom_input_selfcheck.c"
    platform="${doom_dir}/doomgeneric_maize.c"
    sources="${doom_dir}/doom.sources"
    generator="${doom_dir}/tools/make_min_iwad.c"
    # Submodule presence probe: the doomgeneric core-loop TU. Absent => uninitialized.
    probe="${doom_dir}/doomgeneric/doomgeneric/doomgeneric.c"

    if [ ! -f "$probe" ]; then
        echo "[SKIP] ${name}: demos/doom/doomgeneric submodule not initialized" \
             "(run 'git submodule update --init demos/doom/doomgeneric'); skipping DOOM input gate"
        return
    fi

    TOTAL=$((TOTAL + 1))

    # System C compiler (mirrors run_doom_render): compile the committed generator and
    # run it (non-commercial, single-room E1M1) into a dir we mount read-only at /ro.
    gen_cc="${CC:-}"
    if [ -z "$gen_cc" ]; then
        if command -v cc >/dev/null 2>&1; then gen_cc=cc; else gen_cc=gcc; fi
    fi
    gen_exe="${WORK_DIR}/make_min_iwad_input"
    if ! "$gen_cc" -O2 -o "$gen_exe" "$generator" >"${WORK_DIR}/doom-input.gen.log" 2>&1; then
        echo "[FAIL] ${name}: min-IWAD generator failed to compile"
        cat "${WORK_DIR}/doom-input.gen.log" >&2
        FAIL_COUNT=$((FAIL_COUNT + 1)); return
    fi

    waddir="${WORK_DIR}/doom-input-wad"
    rm -rf "$waddir"; mkdir -p "$waddir"
    if ! "$gen_exe" "${waddir}/min.wad" >>"${WORK_DIR}/doom-input.gen.log" 2>&1 \
    || [ ! -f "${waddir}/min.wad" ]; then
        echo "[FAIL] ${name}: min-IWAD generation failed"
        cat "${WORK_DIR}/doom-input.gen.log" >&2
        FAIL_COUNT=$((FAIL_COUNT + 1)); return
    fi

    mzx="${WORK_DIR}/doom_input.mzx"
    log="${WORK_DIR}/doom-input.cc.log"
    rm -f "$mzx"
    if ! "$CC_MAIZE" --preset "$PRESET" --dev \
        -D DOOMGENERIC_RESX=320 -D DOOMGENERIC_RESY=200 \
        -o "$mzx" --sources "$sources" "$input_harness" "$platform" >"$log" 2>&1 \
    || [ ! -f "$mzx" ]; then
        echo "[FAIL] ${name}: input-gate C compile/link failed"; cat "$log" >&2
        FAIL_COUNT=$((FAIL_COUNT + 1)); return
    fi

    nat=$(host_to_native "$waddir")
    # Combine run_doom_selfcheck's scancode-on-stdin injection (`--input=keyboard`) with
    # run_doom_render's mount + guest-argv shape. The make-only bytes 0x48 (up) and 0x1D
    # (Ctrl/fire) hold both bindings down for the whole run. `-iwad /ro/min.wad` is a GUEST
    # path; MSYS2_ARG_CONV_EXCL exempts /ro from the POSIX->Windows argv rewrite (see
    # run_doom_render for the full rationale).
    set +e
    actual=$(printf '\110\035' \
        | MSYS2_ARG_CONV_EXCL='/ro' "$MAIZE" --no-root --input=keyboard \
            --mount "${nat}=/ro:ro" "$mzx" \
            -iwad /ro/min.wad -warp 1 1 -nomonsters 2>/dev/null | grep -v '^$')
    set -e

    # The gate is the exact "doom-input: PASS" line (injected up moved the player AND
    # injected Ctrl decremented clip ammo, both through the real DG_GetKey path).
    if printf '%s\n' "$actual" | grep -qx "doom-input: PASS"; then
        echo "[PASS] ${name}"
    else
        echo "[FAIL] ${name}"
        echo "        expected a \"doom-input: PASS\" line (injected key drove an in-sim change)"
        echo "        actual:   \"$(printf '%s' "$actual" | tail -3 | tr '\n' '|')\""
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi
}

_mz_want "run_doom_input" && mz_timed "run_doom_input" run_doom_input

# maize-138 multi-file compile/link: the primary-gate cross-object fixture, the
# negative link-rejection case, and the two multi-source usage-error paths.
_mz_want "multifile" && mz_timed "multifile" run_multi_ctest "multifile" "multifile_main.c multifile_lib.c"
_mz_want "run_multi_link_reject_test" && mz_timed "run_multi_link_reject_test" run_multi_link_reject_test
_mz_want "multifile_no_out" && mz_timed "multifile_no_out" run_multi_usage_test "multifile_no_out" "needs an output path" \
    "${CTEST_DIR}/multifile_main.c" "${CTEST_DIR}/multifile_lib.c"
_mz_want "multifile_emit_reject" && mz_timed "multifile_emit_reject" run_multi_usage_test "multifile_emit_reject" "only when compiling a single" \
    --emit -o "${WORK_DIR}/multifile_emit_reject.mzx" \
    "${CTEST_DIR}/multifile_main.c" "${CTEST_DIR}/multifile_lib.c"

# maize-169 / maize-170 launcher defaults (~/.maize/config + ~/.maize/env). Redirects
# HOME to a FRESH temp dir so the operator's REAL ~/.maize is never read or written,
# writes a config + env there, and proves both features plus their CLI-override
# precedence. It reuses two already-built fixtures: args (dumps argv then envp, so a
# guest env var is directly observable) and hello (any runnable image). Config
# observability uses the `root` key: maize itself creates the sandbox skeleton
# (home/user, tmp) under the configured root host dir, so the default landing and its
# CLI --root override are visible on the host filesystem with no extra fixture.
run_launcher_defaults() {
    name="launcher_defaults"
    TOTAL=$((TOTAL + 1))

    compile_c "args" || return
    args_bin="$BIN"
    compile_c "hello" || return
    hello_bin="$BIN"
    # compile_c assigns the global `name` (no `local` in POSIX sh), so restore our label
    # after the two fixture compiles before it feeds the PASS/FAIL lines below.
    name="launcher_defaults"

    # A throwaway HOME so we touch neither the operator's real ~/.maize nor global state.
    fake_home="${WORK_DIR}/launcher_home"
    rm -rf "$fake_home"
    mkdir -p "$fake_home/.maize"
    printf 'GREETING=fromdefault\n' > "$fake_home/.maize/env"

    dirA="${WORK_DIR}/launcher_rootA"      # the config default sandbox root
    dirC="${WORK_DIR}/launcher_rootC"      # the CLI-override sandbox root
    rm -rf "$dirA" "$dirC"
    nat_home=$(host_to_native "$fake_home")
    nat_dirA=$(host_to_native "$dirA")
    nat_dirC=$(host_to_native "$dirC")
    # display-scale is a harmless headless-invisible filler; root is the observable key.
    printf 'display-scale=7\nroot=%s\n' "$nat_dirA" > "$fake_home/.maize/config"

    ok=1

    # (a1) ~/.maize/env reaches the guest: args dumps envp, GREETING=fromdefault present.
    set +e
    out_def=$(HOME="$nat_home" "$MAIZE" --no-root "$args_bin" 2>/dev/null)
    set -e
    printf '%s\n' "$out_def" | grep -qx 'GREETING=fromdefault' || ok=0

    # (a2) a CLI -e overrides the default (last-wins): only the override reaches the guest.
    set +e
    out_ovr=$(HOME="$nat_home" "$MAIZE" --no-root -e GREETING=override "$args_bin" 2>/dev/null)
    set -e
    printf '%s\n' "$out_ovr" | grep -qx 'GREETING=override' || ok=0
    if printf '%s\n' "$out_ovr" | grep -qx 'GREETING=fromdefault'; then ok=0; fi

    # (b1) the config default is applied: root=dirA, so maize builds the dirA skeleton.
    rm -rf "$dirA"
    set +e
    HOME="$nat_home" "$MAIZE" "$hello_bin" >/dev/null 2>&1
    set -e
    [ -d "$dirA/home/user" ] || ok=0

    # (b2) a CLI --root overrides the config default: the skeleton lands in dirC, and the
    # config's dirA is NOT recreated for this run.
    rm -rf "$dirA" "$dirC"
    set +e
    HOME="$nat_home" "$MAIZE" --root "$nat_dirC" "$hello_bin" >/dev/null 2>&1
    set -e
    [ -d "$dirC/home/user" ] || ok=0
    [ ! -e "$dirA" ] || ok=0

    if [ "$ok" -eq 1 ]; then
        echo "[PASS] ${name}"
    else
        echo "[FAIL] ${name}"
        echo "        default-env dump:  \"${out_def}\""
        echo "        override-env dump: \"${out_ovr}\""
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi
}

_mz_want "run_launcher_defaults" && mz_timed "run_launcher_defaults" run_launcher_defaults

# =============================================================================
# maize-252: config-file mount= / mount-home= grants. Kept in its OWN block,
# separate from run_launcher_defaults above (which this is additive to, not a
# replacement of): maize-250 is concurrently adding run-ctest fixtures on its
# own worktree, so this boundary is a deliberate rebase-friendly seam.
#
# Covers, all via a throwaway HOME (never the operator's real ~/.maize),
# reusing the cat_hostfs fixture (reads the fixed guest path /ro/payload.txt)
# and a new maize-252 twin, cat_home_hostfs (reads /home/user/payload.txt, the
# --mount-home / mount-home= grant's fixed guest path):
#   1. a config-only mount= line reaches the guest with NO --mount on the CLI.
#   2. precedence: a CLI --mount for the SAME guest path overrides the config
#      grant (config < CLI, same as every other launcher default).
#   3. fail-closed: a malformed/unreachable config mount= line exits nonzero
#      with a diagnostic naming the config path and the raw offending line.
#   4. a Windows drive-letter host value (host_to_native's native form on the
#      MSYS2 CI leg) parses correctly -- the maize-252 review-note (b) lock:
#      load_config_file's OWN first-'=' split and parse_mount_spec's rfind
#      (last-'=') split must compose safely even though the host string itself
#      carries a ':' (drive letter, never confused with '=').
#   5. mount-home=true/empty and mount-home=<path> both mount a read-write
#      /home/user grant (equivalent to the CLI forms; RW-ness is the same
#      hardcoded HOSTFS_RW assignment both paths share in src/maize.cpp, so
#      this proves reachability, the part unique to the config path); mount-
#      home=false/0/no is a no-op (the guest sees /home/user unmounted).
# =============================================================================
run_launcher_config_mount() {
    name="launcher_config_mount"
    TOTAL=$((TOTAL + 1))

    compile_c "cat_hostfs" || return
    cat_bin="$BIN"
    compile_c "cat_home_hostfs" || return
    cat_home_bin="$BIN"
    # compile_c assigns the global `name` (no `local` in POSIX sh); restore our
    # label before the PASS/FAIL line below.
    name="launcher_config_mount"

    fake_home="${WORK_DIR}/launcher_cfg_mount_home"
    rm -rf "$fake_home"
    mkdir -p "$fake_home/.maize"
    nat_home=$(host_to_native "$fake_home")
    cfg_path="$fake_home/.maize/config"
    # maize (a native Windows exe under MSYS2) reports paths in ITS OWN native
    # form (backslashes), built from the HOME we hand it (nat_home, already
    # native); this script's own $cfg_path stays POSIX-style (MSYS bash's pwd).
    # Use the native form for substring checks against maize's own stderr.
    nat_cfg_path=$(host_to_native "$cfg_path")

    ok=1

    # (1) config-only: mount=<dir>=/ro:ro with NO --mount flag on the CLI.
    cfg_root="${WORK_DIR}/launcher_cfg_mount_a"
    rm -rf "$cfg_root"; mkdir -p "$cfg_root"
    printf 'config payload\n' > "$cfg_root/payload.txt"
    nat_cfg_root=$(host_to_native "$cfg_root")
    printf 'mount=%s=/ro:ro\n' "$nat_cfg_root" > "$cfg_path"

    set +e
    out1=$(HOME="$nat_home" "$MAIZE" --no-root "$cat_bin" 2>/dev/null)
    set -e
    [ "$out1" = "config payload" ] || ok=0

    # (2) precedence: the same config mount= line for /ro, PLUS a CLI --mount for
    # the SAME guest path with DIFFERENT content; the CLI grant must win, no error.
    cli_root="${WORK_DIR}/launcher_cfg_mount_b"
    rm -rf "$cli_root"; mkdir -p "$cli_root"
    printf 'cli payload\n' > "$cli_root/payload.txt"
    nat_cli_root=$(host_to_native "$cli_root")

    set +e
    out2=$(HOME="$nat_home" "$MAIZE" --no-root --mount "${nat_cli_root}=/ro:rw" "$cat_bin" 2>/dev/null)
    set -e
    [ "$out2" = "cli payload" ] || ok=0

    # (3) fail-closed: a config mount= line whose host directory does not exist
    # must exit maize nonzero before the guest starts, with a diagnostic naming
    # both the config file path and the raw offending line text.
    bad_root="${WORK_DIR}/launcher_cfg_mount_missing"
    rm -rf "$bad_root"
    nat_bad_root=$(host_to_native "$bad_root")
    bad_line="mount=${nat_bad_root}=/bad:ro"
    printf '%s\n' "$bad_line" > "$cfg_path"

    set +e
    err3=$(HOME="$nat_home" "$MAIZE" --no-root "$cat_bin" 2>&1 >/dev/null)
    rc3=$?
    set -e
    [ "$rc3" -ne 0 ] || ok=0
    printf '%s' "$err3" | grep -qF "$nat_cfg_path" || ok=0
    printf '%s' "$err3" | grep -qF "$bad_line" || ok=0

    # (4) Windows drive-letter host value: mechanically identical to (1), but the
    # host_to_native translation is the point under test (native `C:\...` form on
    # the MSYS2 CI leg), locking in review-note (b)'s split-composition invariant.
    drv_root="${WORK_DIR}/launcher_cfg_mount_drv"
    rm -rf "$drv_root"; mkdir -p "$drv_root"
    printf 'drive payload\n' > "$drv_root/payload.txt"
    nat_drv_root=$(host_to_native "$drv_root")
    printf 'mount=%s=/ro:ro\n' "$nat_drv_root" > "$cfg_path"

    set +e
    out4=$(HOME="$nat_home" "$MAIZE" --no-root "$cat_bin" 2>/dev/null)
    set -e
    [ "$out4" = "drive payload" ] || ok=0

    # (5a)/(5b) mount-home=true and mount-home= (empty) are the SAME boolean-true
    # form, resolving the host home via resolve_home("", home) -- i.e. HOME itself
    # (USERPROFILE on Windows), the identical resolver that also locates ~/.maize.
    # So for THIS sub-case, HOME must point at a directory that is both the
    # ~/.maize/config location AND the fixture payload directory.
    mh_home="${WORK_DIR}/launcher_cfg_mount_home_bool"
    rm -rf "$mh_home"; mkdir -p "$mh_home/.maize"
    printf 'home payload bool\n' > "$mh_home/payload.txt"
    nat_mh_home=$(host_to_native "$mh_home")

    printf 'mount-home=true\n' > "$mh_home/.maize/config"
    set +e
    out5a=$(HOME="$nat_mh_home" "$MAIZE" --no-root "$cat_home_bin" 2>/dev/null)
    set -e
    [ "$out5a" = "home payload bool" ] || ok=0

    printf 'mount-home=\n' > "$mh_home/.maize/config"
    set +e
    out5b=$(HOME="$nat_mh_home" "$MAIZE" --no-root "$cat_home_bin" 2>/dev/null)
    set -e
    [ "$out5b" = "home payload bool" ] || ok=0

    # (5c) mount-home=<path>: an explicit host-path override, independent of HOME,
    # using the ORIGINAL fake_home (whose ~/.maize/config we control directly).
    mh_root2="${WORK_DIR}/launcher_cfg_mount_home_c"
    rm -rf "$mh_root2"; mkdir -p "$mh_root2"
    printf 'home payload path\n' > "$mh_root2/payload.txt"
    nat_mh_root2=$(host_to_native "$mh_root2")
    printf 'mount-home=%s\n' "$nat_mh_root2" > "$cfg_path"

    set +e
    out5c=$(HOME="$nat_home" "$MAIZE" --no-root "$cat_home_bin" 2>/dev/null)
    set -e
    [ "$out5c" = "home payload path" ] || ok=0

    # (5d) mount-home=false is a no-op: /home/user stays unmounted (--no-root, no
    # other grant names it), so the guest's open must fail.
    printf 'mount-home=false\n' > "$cfg_path"
    set +e
    out5d=$(HOME="$nat_home" "$MAIZE" --no-root "$cat_home_bin" 2>/dev/null)
    rc5d=$?
    set -e
    [ "$rc5d" -ne 0 ] || ok=0
    [ "$out5d" != "home payload path" ] || ok=0

    if [ "$ok" -eq 1 ]; then
        echo "[PASS] ${name}"
    else
        echo "[FAIL] ${name}"
        echo "        (1) config-only:  \"${out1}\""
        echo "        (2) precedence:   \"${out2}\""
        echo "        (3) fail-closed:  rc=${rc3} stderr=\"${err3}\""
        echo "        (4) drive-letter: \"${out4}\""
        echo "        (5a) home=true:   \"${out5a}\""
        echo "        (5b) home=empty:  \"${out5b}\""
        echo "        (5c) home=<path>: \"${out5c}\""
        echo "        (5d) home=false:  rc=${rc5d} out=\"${out5d}\""
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi
}

_mz_want "run_launcher_config_mount" && mz_timed "run_launcher_config_mount" run_launcher_config_mount
# =============================================================================

# =============================================================================
# maize-249: per-binary launcher config overrides (~/.maize/maize.config for the
# console `maize`, ~/.maize/maizeg.config for the graphical `maizeg`) layered on
# top of the shared ~/.maize/config. Structural precedent: run_launcher_defaults
# and run_launcher_config_mount above. Every sub-case redirects HOME to a fresh
# throwaway dir so the operator's REAL ~/.maize is never read or written. Runs
# UNCONDITIONALLY on both CI legs (Linux and Windows/MSYS2), with no OS-specific
# skip, matching those precedents (AC 9269); $MAIZEG is resolved up front (headless,
# SDL-free with MAIZE_DISPLAY off) so invoking it directly is CI-safe.
#
# Covers (see the card's acceptance criteria for the exact leg list):
#   - shared-only regression, per-binary-only, and both-with-override root= landing
#     (AC 9258/9259/9260): built-in < shared < per-binary precedence for a scalar key
#   - cross-binary isolation (AC 9261): maize reads maize.config, maizeg reads
#     maizeg.config, neither touches the other's root
#   - mount-grant three-tier layering: distinct guest paths reachable simultaneously
#     (AC 9262), same guest path per-binary-wins (AC 9263), CLI wins over both (AC 9264)
#   - every load_config_file warning names its own file (AC 9265)
#   - the maize-237 discard fires for a per-binary input=keyboard AND a bare-VM guest's
#     own fd-0 read stays intact under the layered config, on the maize-238-landed
#     default path (AC 9266, via the asm/test_sysread.mazm / "hello|EOF" pairing)
#   - the OQ 9257 ruling: the console build WARNS when maize.config sets a graphical-only
#     input default (its own file) and stays SILENT when the shared config sets it
# =============================================================================
run_launcher_per_binary() {
    name="launcher_per_binary"
    TOTAL=$((TOTAL + 1))

    compile_c "hello" || return
    hello_bin="$BIN"
    compile_c "cat_hostfs" || return
    cat_bin="$BIN"
    compile_c "cat_home_hostfs" || return
    cat_home_bin="$BIN"
    # compile_c assigns the global `name` (no `local` in POSIX sh); restore our label.
    name="launcher_per_binary"

    # A bare-VM stdin fixture (AC 9266): assemble asm/test_sysread.mazm via $MAZM, the
    # same guest run-tests.sh's run_sysread_test drives. Piped "hello" must round-trip
    # to "hello|EOF" (the guest's own SYS $00 read of fd 0), proving no config-armed
    # device stole the first byte under the layered load, on the maize-238 default path.
    sysread_src="${REPO_ROOT}/asm/test_sysread.mazm"
    sysread_asm="${WORK_DIR}/pb_test_sysread.mazm"
    cp "$sysread_src" "$sysread_asm"
    if ! "$MAZM" "$sysread_asm" >/dev/null 2>&1 || [ ! -f "${sysread_asm%.mazm}.mzb" ]; then
        echo "[FAIL] ${name} (mazm failed to assemble test_sysread.mazm)"
        FAIL_COUNT=$((FAIL_COUNT + 1)); return
    fi
    sysread_bin="${sysread_asm%.mazm}.mzb"

    fake_home="${WORK_DIR}/launcher_pb_home"
    rm -rf "$fake_home"; mkdir -p "$fake_home/.maize"
    nat_home=$(host_to_native "$fake_home")
    cfg="$fake_home/.maize/config"
    mcfg="$fake_home/.maize/maize.config"
    mgcfg="$fake_home/.maize/maizeg.config"
    # maize (a native exe under MSYS2) reports paths in its OWN native form; build the
    # native views for substring checks against maize's own stderr.
    nat_cfg=$(host_to_native "$cfg")
    nat_mcfg=$(host_to_native "$mcfg")

    dirA="${WORK_DIR}/pb_rootA"; nat_dirA=$(host_to_native "$dirA")
    dirB="${WORK_DIR}/pb_rootB"; nat_dirB=$(host_to_native "$dirB")
    dirX="${WORK_DIR}/pb_rootX"; nat_dirX=$(host_to_native "$dirX")
    dirY="${WORK_DIR}/pb_rootY"; nat_dirY=$(host_to_native "$dirY")

    ok=1

    # (1) AC 9258 shared-only regression: shared config root=dirA, no per-binary file;
    # the console maize builds the dirA skeleton (existing shared-only behavior).
    rm -f "$cfg" "$mcfg" "$mgcfg"; rm -rf "$dirA"
    printf 'root=%s\n' "$nat_dirA" > "$cfg"
    set +e; HOME="$nat_home" "$MAIZE" "$hello_bin" >/dev/null 2>&1; set -e
    [ -d "$dirA/home/user" ] || ok=0

    # (2) AC 9259 per-binary-only: NO shared config, maize.config root=dirB; maize builds dirB.
    rm -f "$cfg" "$mcfg" "$mgcfg"; rm -rf "$dirB"
    printf 'root=%s\n' "$nat_dirB" > "$mcfg"
    set +e; HOME="$nat_home" "$MAIZE" "$hello_bin" >/dev/null 2>&1; set -e
    [ -d "$dirB/home/user" ] || ok=0

    # (3) AC 9260 both-with-override: shared root=dirA, per-binary root=dirB; per-binary
    # wins (skeleton lands in dirB) and dirA is NOT created for this run.
    rm -f "$cfg" "$mcfg" "$mgcfg"; rm -rf "$dirA" "$dirB"
    printf 'root=%s\n' "$nat_dirA" > "$cfg"
    printf 'root=%s\n' "$nat_dirB" > "$mcfg"
    set +e; HOME="$nat_home" "$MAIZE" "$hello_bin" >/dev/null 2>&1; set -e
    [ -d "$dirB/home/user" ] || ok=0
    [ ! -e "$dirA" ] || ok=0

    # (4) AC 9261 cross-binary isolation: maize.config root=dirX, maizeg.config root=dirY.
    # The console maize creates dirX only; the graphical maizeg creates dirY only.
    rm -f "$cfg" "$mcfg" "$mgcfg"
    printf 'root=%s\n' "$nat_dirX" > "$mcfg"
    printf 'root=%s\n' "$nat_dirY" > "$mgcfg"
    rm -rf "$dirX" "$dirY"
    set +e; HOME="$nat_home" "$MAIZE" "$hello_bin" >/dev/null 2>&1; set -e
    [ -d "$dirX/home/user" ] || ok=0
    [ ! -e "$dirY" ] || ok=0
    rm -rf "$dirX" "$dirY"
    set +e; HOME="$nat_home" "$MAIZEG" "$hello_bin" >/dev/null 2>&1; set -e
    [ -d "$dirY/home/user" ] || ok=0
    [ ! -e "$dirX" ] || ok=0

    # (5) AC 9262 mount layering, DISTINCT guest paths reachable at once: shared config
    # grants /ro (from roA), maize.config grants /home/user (mount-home=true -> HOME).
    rm -f "$cfg" "$mcfg" "$mgcfg"
    roA="${WORK_DIR}/pb_roA"; rm -rf "$roA"; mkdir -p "$roA"
    printf 'ro payload\n' > "$roA/payload.txt"; nat_roA=$(host_to_native "$roA")
    printf 'home payload\n' > "$fake_home/payload.txt"   # mount-home=true maps /home/user -> HOME
    printf 'mount=%s=/ro:ro\n' "$nat_roA" > "$cfg"
    printf 'mount-home=true\n' > "$mcfg"
    set +e; out5ro=$(HOME="$nat_home" "$MAIZE" --no-root "$cat_bin" 2>/dev/null); set -e
    [ "$out5ro" = "ro payload" ] || ok=0
    set +e; out5hm=$(HOME="$nat_home" "$MAIZE" --no-root "$cat_home_bin" 2>/dev/null); set -e
    [ "$out5hm" = "home payload" ] || ok=0

    # (6) AC 9263 mount layering, SAME guest path: shared grants roA at /ro, maize.config
    # grants roB at /ro; the per-binary grant wins, exits 0, with no overlap diagnostic.
    roB="${WORK_DIR}/pb_roB"; rm -rf "$roB"; mkdir -p "$roB"
    printf 'per-binary payload\n' > "$roB/payload.txt"; nat_roB=$(host_to_native "$roB")
    printf 'mount=%s=/ro:ro\n' "$nat_roA" > "$cfg"
    printf 'mount=%s=/ro:ro\n' "$nat_roB" > "$mcfg"
    set +e
    out6=$(HOME="$nat_home" "$MAIZE" --no-root "$cat_bin" 2>"${WORK_DIR}/pb_sc6.err")
    rc6=$?
    set -e
    err6=$(cat "${WORK_DIR}/pb_sc6.err")
    [ "$out6" = "per-binary payload" ] || ok=0
    [ "$rc6" -eq 0 ] || ok=0
    if printf '%s' "$err6" | grep -qi 'overlap'; then ok=0; fi

    # (7) AC 9264 CLI wins over BOTH file tiers: same shared/per-binary /ro grants as (6),
    # plus a CLI --mount roC=/ro:rw for the same guest path; the CLI grant wins.
    roC="${WORK_DIR}/pb_roC"; rm -rf "$roC"; mkdir -p "$roC"
    printf 'cli payload\n' > "$roC/payload.txt"; nat_roC=$(host_to_native "$roC")
    set +e
    out7=$(HOME="$nat_home" "$MAIZE" --no-root --mount "${nat_roC}=/ro:rw" "$cat_bin" 2>/dev/null)
    set -e
    [ "$out7" = "cli payload" ] || ok=0

    # (8) AC 9265 diagnostic file-naming: an out-of-range display-scale line names the
    # file it came from. Leg a: only in maize.config -> names maize.config. Leg b: only
    # in the shared config -> names the shared config (and NOT maize.config).
    rm -f "$cfg" "$mcfg" "$mgcfg"
    printf 'display-scale=99\n' > "$mcfg"
    set +e; err8a=$(HOME="$nat_home" "$MAIZE" --no-root "$hello_bin" 2>&1 >/dev/null); set -e
    printf '%s' "$err8a" | grep -qF 'display-scale' || ok=0
    printf '%s' "$err8a" | grep -qF "$nat_mcfg" || ok=0
    rm -f "$mcfg"
    printf 'display-scale=99\n' > "$cfg"
    set +e; err8b=$(HOME="$nat_home" "$MAIZE" --no-root "$hello_bin" 2>&1 >/dev/null); set -e
    printf '%s' "$err8b" | grep -qF "$nat_cfg" || ok=0
    if printf '%s' "$err8b" | grep -qF "$nat_mcfg"; then ok=0; fi

    # (9) AC 9266(a) + OQ 9257 warn-leg: input=keyboard in the console's OWN maize.config.
    # The maize-237 discard neutralizes it (the guest's piped stdin round-trips to
    # "hello|EOF", no byte theft on the maize-238 default path) AND the console build warns,
    # naming maize.config, that it ignored the graphical-only input default.
    rm -f "$cfg" "$mcfg" "$mgcfg"
    printf 'input=keyboard\n' > "$mcfg"
    set +e
    out9=$(printf 'hello' | HOME="$nat_home" "$MAIZE" --no-root "$sysread_bin" 2>"${WORK_DIR}/pb_sc9.err")
    set -e
    err9=$(cat "${WORK_DIR}/pb_sc9.err")
    [ "$out9" = "hello|EOF" ] || ok=0
    printf '%s' "$err9" | grep -qi 'ignoring input=' || ok=0
    printf '%s' "$err9" | grep -qF "$nat_mcfg" || ok=0

    # (10) OQ 9257 silent-leg: input=keyboard in the SHARED config stays silent (today's
    # behavior), even though the console build still discards it.
    rm -f "$cfg" "$mcfg" "$mgcfg"
    printf 'input=keyboard\n' > "$cfg"
    set +e; err10=$(HOME="$nat_home" "$MAIZE" --no-root "$hello_bin" 2>&1 >/dev/null); set -e
    if printf '%s' "$err10" | grep -qi 'ignoring input='; then ok=0; fi

    # (11) AC 9266(b) bare-VM default-invocation stdin under a LAYERED config (both files
    # present, harmless keys): the guest's own fd-0 read still receives every byte intact,
    # proving the second config load did not reintroduce the maize-237 stdin-theft hazard.
    rm -f "$cfg" "$mcfg" "$mgcfg"
    printf 'display-scale=5\n' > "$cfg"
    printf 'refresh-hz=45\n' > "$mcfg"
    set +e
    out11=$(printf 'hello' | HOME="$nat_home" "$MAIZE" --no-root "$sysread_bin" 2>/dev/null)
    set -e
    [ "$out11" = "hello|EOF" ] || ok=0

    if [ "$ok" -eq 1 ]; then
        echo "[PASS] ${name}"
    else
        echo "[FAIL] ${name}"
        echo "        (1) shared-only rootA dir:      $([ -d "$dirA/home/user" ] && echo yes || echo no)"
        echo "        (5) ro/home payloads:           \"${out5ro}\" / \"${out5hm}\""
        echo "        (6) same-path per-binary win:   rc=${rc6} out=\"${out6}\" err=\"${err6}\""
        echo "        (7) CLI-over-both:              \"${out7}\""
        echo "        (9) sysread + warn:             out=\"${out9}\" err=\"${err9}\""
        echo "        (10) shared-input silent err:   \"${err10}\""
        echo "        (11) layered-config sysread:    \"${out11}\""
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi
}

_mz_want "run_launcher_per_binary" && mz_timed "run_launcher_per_binary" run_launcher_per_binary
# =============================================================================

# maize-357 (AC 9853): large-period armed-timer cadence. asm/test_timer_period1.mazm
# only exercises a single-tick crossing (period 1). This bare-VM asm fixture programs a
# periodic timer with period 2000, far above the 512-instruction JIT block cap, so under
# the JIT the countdown is driven by the O(1) bulk subtract (advance_active_timer(t, n))
# across ~125 block boundaries per period, crossing zero exactly once and re-arming. The
# handler asserts the per-IRQ cadence lands in [$20, $400] (a subtract-1-per-block
# regression would fire ~2000 iterations apart and trip the high bound; a no-crossing
# regression would time out), then prints a bare "timerpL: PASS" with no cadence numbers.
# Two proofs, like quesos_satp_jit_equiv below: (1) the marker, and (2) byte-identical
# stdout under the interpreter (n = 1 per instruction) versus --jit bare (n > 1 per
# block), which is exactly the interpreter/JIT equivalence this card must preserve. The
# comparison runs through BARE_MAIZE (maize-360: the --bare, non-JIT wrapper, so the raw
# VM image still launches directly) rather than $MAIZE, which the MAIZE_JIT wrapper may
# have pinned to one mode, so the comparison always runs interpreter-vs-JIT regardless of
# the harness env.
run_timer_cadence_equiv() {
    src="test_timer_period_large.mazm"
    cp "${REPO_ROOT}/asm/${src}" "${WORK_DIR}/${src}"
    mzb="${WORK_DIR}/test_timer_period_large.mzb"
    TOTAL=$((TOTAL + 1))
    if ! "$MAZM" "${WORK_DIR}/${src}" >"${WORK_DIR}/timer_cadence.asm.log" 2>&1 || [ ! -f "$mzb" ]; then
        echo "[FAIL] timer_cadence_equiv: mazm failed to assemble ${src}"
        cat "${WORK_DIR}/timer_cadence.asm.log" >&2
        FAIL_COUNT=$((FAIL_COUNT + 1)); return
    fi
    raw_maize="$BARE_MAIZE"   # maize-360: bare (so it runs the raw image), non-JIT wrapper
    if [ -z "$raw_maize" ]; then
        echo "[FAIL] timer_cadence_equiv: raw maize binary not found in ${BUILD_DIR}"
        FAIL_COUNT=$((FAIL_COUNT + 1)); return
    fi
    ti="${WORK_DIR}/timer_cadence.interp.out"
    tj="${WORK_DIR}/timer_cadence.jit.out"
    set +e
    timeout 30 "$raw_maize" "$mzb" >"$ti" 2>/dev/null
    timeout 30 "$raw_maize" --jit --jit-threshold 50 "$mzb" >"$tj" 2>/dev/null
    set -e
    if grep -qF "timerpL: PASS" "$ti" && cmp -s "$ti" "$tj"; then
        echo "[PASS] timer_cadence_equiv (bulk subtract: interp n=1 byte-identical to --jit n>1, period 2000)"
    else
        echo "[FAIL] timer_cadence_equiv"
        echo "        interp: \"$(tr '\n' '|' < "$ti")\""
        echo "        jit:    \"$(tr '\n' '|' < "$tj")\""
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi
}

_mz_want "run_timer_cadence_equiv" && mz_timed "run_timer_cadence_equiv" run_timer_cadence_equiv
# =============================================================================

# maize-24 keystone (Piece 3): quesOS single-tasking exec/reap. Builds the two
# borrowed static guest printers (os/quesos/demo_child*.c) through the ordinary
# cc-maize.sh pipeline (stock .mzx at base 0x2000), links quesOS itself at its
# non-default base via mzcc build-quesos (maize-382), then runs quesOS as a directly-
# loaded image with the two children on its argv worklist (decision D7). The
# children live under a :ro mount at /progs, resolved by quesOS's execve through
# the passthrough file syscalls. The gate is the exact interleaved transcript:
# quesOS's init line, then for each child its own SYS $01 output followed by
# quesOS's reap line carrying the distinct recorded exit status, in order. That one
# transcript evidences AC1 (handler installed at cause 7 before any exec), AC2
# (execve loads a .mzx + builds the argv stack + transfers control; the child prints
# and exits), AC3 (the child's SYS $3C trapped into quesOS's dispatcher, not native
# power_off: the VM did NOT halt, it recorded the status and kept running), and AC4
# (the second child ran via the same path; both outputs + both statuses are
# observable in order).
run_quesos_selfcheck() {
    name="quesos"
    TOTAL=$((TOTAL + 1))

    c1="${REPO_ROOT}/os/quesos/demo_child1.c"
    c2="${REPO_ROOT}/os/quesos/demo_child2.c"
    builder="$MZCC"                              # maize-382: mzcc build-quesos, not the .sh
    if [ ! -f "$c1" ] || [ ! -f "$c2" ] || [ ! -f "$builder" ]; then
        echo "[FAIL] ${name}: missing quesOS sources under os/quesos/" >&2
        FAIL_COUNT=$((FAIL_COUNT + 1)); return
    fi

    progs="${WORK_DIR}/quesos-progs"
    rm -rf "$progs"; mkdir -p "$progs"
    log="${WORK_DIR}/quesos.build.log"

    if ! "$CC_MAIZE" --preset "$PRESET" -o "${progs}/child1.mzx" "$c1" >"$log" 2>&1 \
    || ! "$CC_MAIZE" --preset "$PRESET" -o "${progs}/child2.mzx" "$c2" >>"$log" 2>&1; then
        echo "[FAIL] ${name}: child compile failed"; cat "$log" >&2
        FAIL_COUNT=$((FAIL_COUNT + 1)); return
    fi

    quesos="${WORK_DIR}/quesos.mzx"
    if ! "$builder" build-quesos --preset "$PRESET" -o "$quesos" >>"$log" 2>&1 || [ ! -f "$quesos" ]; then
        echo "[FAIL] ${name}: quesOS link failed"; cat "$log" >&2
        FAIL_COUNT=$((FAIL_COUNT + 1)); return
    fi

    nat=$(host_to_native "$progs")
    # MSYS2_ARG_CONV_EXCL keeps the /progs guest paths from being rewritten to Windows
    # paths on the MinGW leg (same reason doom-render excludes /ro); harmless elsewhere.
    set +e
    # maize-359: quesOS boot now forwards argv[1..] to the launched program as its
    # argv, so multiple top-level programs are separated by an explicit `--` token.
    # Both children still get zero forwarded args; only the boundary syntax changes,
    # and the "running 2 program(s)" transcript is unchanged.
    # maize-372: the [quesos] init/reap lines are now quiet by default; this fixture
    # asserts on them, so opt into verbose boot explicitly with -e QUESOS_VERBOSE=1.
    actual=$(MSYS2_ARG_CONV_EXCL='/progs' "$MAIZE" --no-root --mount "${nat}=/progs:ro" \
        -e QUESOS_VERBOSE=1 "$quesos" /progs/child1.mzx -- /progs/child2.mzx 2>/dev/null | grep -v '^$')
    set -e

    expected=$(printf '%s\n' \
        '[quesos] init: cause-7 handler resident; running 2 program(s)' \
        'child one: hello from a quesos guest' \
        '[quesos] reaped /progs/child1.mzx status=7' \
        'child two: second guest reporting in' \
        '[quesos] reaped /progs/child2.mzx status=3')

    if [ "$actual" = "$expected" ]; then
        echo "[PASS] ${name}"
    else
        echo "[FAIL] ${name}"
        echo "        expected transcript:"
        printf '%s\n' "$expected" | sed 's/^/          | /'
        echo "        actual transcript:"
        printf '%s\n' "$actual" | sed 's/^/          | /'
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi
}

_mz_want "run_quesos_selfcheck" && mz_timed "run_quesos_selfcheck" run_quesos_selfcheck

# maize-359: quesOS boot-argv forwarding. quesOS now runs its FIRST boot token as the
# program and forwards the rest (argv[1..], split on `--` between programs) as that
# program's full argv, instead of treating every token as a separate no-arg program.
# argcheck.c is the quesOS analog of ctest/args.c: it prints each argv entry (bounded
# by argc) then each envp entry. Three legs:
#   1. single program with args: `quesos.mzx /progs/argcheck.mzx a b -c` must arrive as
#      argv == [/progs/argcheck.mzx, a, b, -c] (the operator repro: -flags now forward).
#   2. launcher --env still delivers K=V into the launched program's envp (maize-287).
#   3. a `--`-separated multi-program boot forwards each program's own args.
run_quesos_argcheck() {
    name="quesos_argcheck"

    ac="${REPO_ROOT}/os/quesos/argcheck.c"
    builder="$MZCC"                              # maize-382: mzcc build-quesos, not the .sh
    if [ ! -f "$ac" ] || [ ! -f "$builder" ]; then
        echo "[FAIL] ${name}: missing os/quesos/argcheck.c or mzcc" >&2
        TOTAL=$((TOTAL + 1)); FAIL_COUNT=$((FAIL_COUNT + 1)); return
    fi

    progs="${WORK_DIR}/quesos-argcheck"
    rm -rf "$progs"; mkdir -p "$progs"
    log="${WORK_DIR}/quesos-argcheck.build.log"

    if ! "$CC_MAIZE" --preset "$PRESET" -o "${progs}/argcheck.mzx" "$ac" >"$log" 2>&1; then
        echo "[FAIL] ${name}: argcheck compile failed"; cat "$log" >&2
        TOTAL=$((TOTAL + 1)); FAIL_COUNT=$((FAIL_COUNT + 1)); return
    fi

    quesos="${WORK_DIR}/quesos-argcheck.mzx"
    if ! "$builder" build-quesos --preset "$PRESET" -o "$quesos" >>"$log" 2>&1 || [ ! -f "$quesos" ]; then
        echo "[FAIL] ${name}: quesOS link failed"; cat "$log" >&2
        TOTAL=$((TOTAL + 1)); FAIL_COUNT=$((FAIL_COUNT + 1)); return
    fi

    nat=$(host_to_native "$progs")

    # Leg 1: single program with forwarded args. Exactly 4 argv lines, in order, with
    # the leading-dash token -c preserved verbatim, framed by quesOS's init/reap lines.
    TOTAL=$((TOTAL + 1))
    set +e
    # maize-372: this leg asserts on the now-gated [quesos] init/reap lines, so opt
    # into verbose boot explicitly with -e QUESOS_VERBOSE=1.
    actual=$(MSYS2_ARG_CONV_EXCL='/progs' "$MAIZE" --no-root --mount "${nat}=/progs:ro" \
        -e QUESOS_VERBOSE=1 "$quesos" /progs/argcheck.mzx a b -c 2>/dev/null | grep -v '^$')
    set -e
    # maize-360: quesOS gap-fills the five login-env keys (HOME/USER/LOGNAME/SHELL/PATH)
    # into EVERY top-level worklist process, so argcheck's envp now carries them after its
    # argv block (this leg passes no -e, so those five are the whole envp).
    # maize-374: the login identity is now coherently root, so HOME/USER/LOGNAME are
    # /root, root, root (SHELL/PATH unchanged).
    expected=$(printf '%s\n' \
        '[quesos] init: cause-7 handler resident; running 1 program(s)' \
        '/progs/argcheck.mzx' \
        'a' \
        'b' \
        '-c' \
        'HOME=/root' \
        'USER=root' \
        'LOGNAME=root' \
        'SHELL=/bin/oksh.mzx' \
        'PATH=/bin' \
        '[quesos] reaped /progs/argcheck.mzx status=0')
    if [ "$actual" = "$expected" ]; then
        echo "[PASS] ${name}_forward (argv[1..] reach the program in order)"
    else
        echo "[FAIL] ${name}_forward"
        echo "        expected transcript:"; printf '%s\n' "$expected" | sed 's/^/          | /'
        echo "        actual transcript:";   printf '%s\n' "$actual"   | sed 's/^/          | /'
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi

    # Leg 2: launcher --env still delivers K=V to the launched program's envp.
    TOTAL=$((TOTAL + 1))
    set +e
    envout=$(MSYS2_ARG_CONV_EXCL='/progs' "$MAIZE" --env QOSVAR=set --no-root \
        --mount "${nat}=/progs:ro" "$quesos" /progs/argcheck.mzx a b -c 2>/dev/null)
    set -e
    if printf '%s\n' "$envout" | grep -qF 'QOSVAR=set'; then
        echo "[PASS] ${name}_env (launcher --env reaches the forwarded program)"
    else
        echo "[FAIL] ${name}_env (QOSVAR=set not in envp)"
        printf '%s\n' "$envout" | sed 's/^/          | /'
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi

    # Leg 3: `--`-separated multi-program boot; each program gets its own forwarded args.
    TOTAL=$((TOTAL + 1))
    set +e
    # maize-372: this leg asserts on the now-gated [quesos] init/reap lines, so opt
    # into verbose boot explicitly with -e QUESOS_VERBOSE=1.
    multi=$(MSYS2_ARG_CONV_EXCL='/progs' "$MAIZE" --no-root --mount "${nat}=/progs:ro" \
        -e QUESOS_VERBOSE=1 "$quesos" /progs/argcheck.mzx one -- /progs/argcheck.mzx two 2>/dev/null | grep -v '^$')
    set -e
    # maize-360: each top-level worklist program gets the five gap-filled login-env keys
    # in its envp, so both argcheck runs print them after their own argv block.
    # maize-374: the login identity is now coherently root, so HOME/USER/LOGNAME are
    # /root, root, root (SHELL/PATH unchanged).
    expected_multi=$(printf '%s\n' \
        '[quesos] init: cause-7 handler resident; running 2 program(s)' \
        '/progs/argcheck.mzx' \
        'one' \
        'HOME=/root' \
        'USER=root' \
        'LOGNAME=root' \
        'SHELL=/bin/oksh.mzx' \
        'PATH=/bin' \
        '[quesos] reaped /progs/argcheck.mzx status=0' \
        '/progs/argcheck.mzx' \
        'two' \
        'HOME=/root' \
        'USER=root' \
        'LOGNAME=root' \
        'SHELL=/bin/oksh.mzx' \
        'PATH=/bin' \
        '[quesos] reaped /progs/argcheck.mzx status=0')
    if [ "$multi" = "$expected_multi" ]; then
        echo "[PASS] ${name}_multi (\`--\` splits programs; each keeps its own args)"
    else
        echo "[FAIL] ${name}_multi"
        echo "        expected transcript:"; printf '%s\n' "$expected_multi" | sed 's/^/          | /'
        echo "        actual transcript:";   printf '%s\n' "$multi"          | sed 's/^/          | /'
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi
}

_mz_want "run_quesos_argcheck" && mz_timed "run_quesos_argcheck" run_quesos_argcheck

# maize-360: quesOS-as-default-substrate. Two new behaviors ride on the SAME argcheck
# envp/argv dumper. This fixture drives the NEW default (ROM-wrapping) path directly via
# DEFAULT_MAIZE (the raw binary, deliberately NOT the --bare wrapper), with --rom naming
# the freshly built quesOS as the ROM, so it does not depend on quesos.mzx sitting beside
# the test binary. Legs:
#   1. QUESOS_INIT default init: an empty worklist (no app named, session mode) spawns the
#      program QUESOS_INIT points at instead of powering off; argcheck's argv is exactly
#      [<init-path>] (single token, no forced args).
#   2. QUESOS_INIT is consumed: it never appears in the launched program's envp, while a
#      distinct -e entry (QOSVAR=set) does. Proven the argcheck.c / maize-359 way.
#   3. Login-env gap-fill: the five defaults (HOME/USER/LOGNAME/SHELL/PATH) appear in the
#      envp, but an explicit -e HOME=/custom overrides the HOME default (gap-fill only).
run_quesos_default_init() {
    name="quesos_default_init"

    ac="${REPO_ROOT}/os/quesos/argcheck.c"
    builder="$MZCC"                              # maize-382: mzcc build-quesos, not the .sh
    if [ ! -f "$ac" ] || [ ! -f "$builder" ]; then
        echo "[FAIL] ${name}: missing os/quesos/argcheck.c or mzcc" >&2
        TOTAL=$((TOTAL + 1)); FAIL_COUNT=$((FAIL_COUNT + 1)); return
    fi

    progs="${WORK_DIR}/quesos-definit"
    rm -rf "$progs"; mkdir -p "$progs"
    log="${WORK_DIR}/quesos-definit.build.log"

    if ! "$CC_MAIZE" --preset "$PRESET" -o "${progs}/argcheck.mzx" "$ac" >"$log" 2>&1; then
        echo "[FAIL] ${name}: argcheck compile failed"; cat "$log" >&2
        TOTAL=$((TOTAL + 1)); FAIL_COUNT=$((FAIL_COUNT + 1)); return
    fi

    quesos="${WORK_DIR}/quesos-definit.mzx"
    if ! "$builder" build-quesos --preset "$PRESET" -o "$quesos" >>"$log" 2>&1 || [ ! -f "$quesos" ]; then
        echo "[FAIL] ${name}: quesOS link failed"; cat "$log" >&2
        TOTAL=$((TOTAL + 1)); FAIL_COUNT=$((FAIL_COUNT + 1)); return
    fi

    nat=$(host_to_native "$progs")

    # Leg 1 + 2: QUESOS_INIT set, empty worklist (no app token). argcheck runs as the
    # default init with argv == [/progs/argcheck.mzx]; its envp carries QOSVAR=set but
    # NOT QUESOS_INIT, framed by quesOS's init/reap lines.
    TOTAL=$((TOTAL + 1))
    set +e
    # maize-372: this leg asserts on the now-gated [quesos] init/reap lines, so opt
    # into verbose boot explicitly with -e QUESOS_VERBOSE=1.
    # maize-384: the exclusion list needs the `QUESOS_INIT=` prefix, not just `/progs`.
    # MSYS2_ARG_CONV_EXCL entries are matched against the START of the whole argument,
    # so a bare `/progs` covers the standalone guest-path arguments the other quesOS
    # fixtures pass but never matches `QUESOS_INIT=/progs/argcheck.mzx`, whose leading
    # text is the variable name. Git Bash therefore rewrote the value to
    # `C:/Program Files/Git/progs/argcheck.mzx` and quesOS answered
    # "[quesos] cannot start ...", which is what failed this leg on the Windows job
    # only (reproduced natively on a Windows host, then fixed by this list).
    out=$(MSYS2_ARG_CONV_EXCL='/progs;QUESOS_INIT=' "$DEFAULT_MAIZE" --rom "$quesos" --no-root \
        --mount "${nat}=/progs:ro" -e QUESOS_INIT=/progs/argcheck.mzx -e QOSVAR=set \
        -e QUESOS_VERBOSE=1 </dev/null 2>/dev/null | grep -v '^$')
    set -e
    # maize-374: the login identity is now coherently root, so HOME/USER/LOGNAME are
    # /root, root, root (SHELL/PATH unchanged).
    expected=$(printf '%s\n' \
        '[quesos] init: cause-7 handler resident; running 1 program(s)' \
        '/progs/argcheck.mzx' \
        'QOSVAR=set' \
        'HOME=/root' \
        'USER=root' \
        'LOGNAME=root' \
        'SHELL=/bin/oksh.mzx' \
        'PATH=/bin' \
        '[quesos] reaped /progs/argcheck.mzx status=0')
    if [ "$out" = "$expected" ]; then
        echo "[PASS] ${name}_init (QUESOS_INIT spawns default init; consumed from envp; login-env gap-filled)"
    else
        echo "[FAIL] ${name}_init"
        echo "        expected transcript:"; printf '%s\n' "$expected" | sed 's/^/          | /'
        echo "        actual transcript:";   printf '%s\n' "$out"      | sed 's/^/          | /'
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi

    # Leg 3: an explicit -e HOME wins over the gap-filled default.
    TOTAL=$((TOTAL + 1))
    set +e
    # maize-384: same assignment-form exclusion as leg 1 above, plus `HOME=`, since
    # -e HOME=/custom carries a guest path the same way.
    homeout=$(MSYS2_ARG_CONV_EXCL='/progs;QUESOS_INIT=;HOME=' "$DEFAULT_MAIZE" --rom "$quesos" --no-root \
        --mount "${nat}=/progs:ro" -e QUESOS_INIT=/progs/argcheck.mzx -e HOME=/custom \
        </dev/null 2>/dev/null)
    set -e
    if printf '%s\n' "$homeout" | grep -qxF 'HOME=/custom' \
        && ! printf '%s\n' "$homeout" | grep -qxF 'HOME=/home/user'; then
        echo "[PASS] ${name}_explicit_home (an -e HOME entry overrides the login-env default)"
    else
        echo "[FAIL] ${name}_explicit_home (expected HOME=/custom, not the /home/user default)"
        printf '%s\n' "$homeout" | grep -E '^HOME=' | sed 's/^/          | /'
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi
}

_mz_want "run_quesos_default_init" && mz_timed "run_quesos_default_init" run_quesos_default_init

# maize-372: quesOS quiet-boot gate. quesOS is now Unix-quiet by default: the
# informational [quesos] init/reap/unhandled-syscall trace lines are gated behind a
# reserved QUESOS_VERBOSE boot-env key and stay silent unless it is set to a non-empty
# value. Genuine error/warning [quesos] lines are never gated. Three legs, all on the
# same argcheck scaffold used by run_quesos_argcheck / run_quesos_default_init:
#   A default_quiet:         no QUESOS_VERBOSE; neither [quesos] init: nor [quesos] reaped
#                            appears, but argcheck's own argv+envp dump still runs and reaps.
#   B verbose:               -e QUESOS_VERBOSE=1; the init + reaped lines reappear exactly as
#                            before, and QUESOS_VERBOSE is stripped from the envp dump (armed).
#   C genuine_error_visible: booting a /progs entry that was never written still prints the
#                            ALWAYS "[quesos] cannot start ..." line under default quiet boot,
#                            while the GATED "[quesos] init:" line stays absent on the same run.
run_quesos_quiet_boot() {
    name="quesos_quiet_boot"

    ac="${REPO_ROOT}/os/quesos/argcheck.c"
    builder="$MZCC"                              # maize-382: mzcc build-quesos, not the .sh
    if [ ! -f "$ac" ] || [ ! -f "$builder" ]; then
        echo "[FAIL] ${name}: missing os/quesos/argcheck.c or mzcc" >&2
        TOTAL=$((TOTAL + 1)); FAIL_COUNT=$((FAIL_COUNT + 1)); return
    fi

    progs="${WORK_DIR}/quesos-quiet"
    rm -rf "$progs"; mkdir -p "$progs"
    log="${WORK_DIR}/quesos-quiet.build.log"

    if ! "$CC_MAIZE" --preset "$PRESET" -o "${progs}/argcheck.mzx" "$ac" >"$log" 2>&1; then
        echo "[FAIL] ${name}: argcheck compile failed"; cat "$log" >&2
        TOTAL=$((TOTAL + 1)); FAIL_COUNT=$((FAIL_COUNT + 1)); return
    fi

    quesos="${WORK_DIR}/quesos-quiet.mzx"
    if ! "$builder" build-quesos --preset "$PRESET" -o "$quesos" >>"$log" 2>&1 || [ ! -f "$quesos" ]; then
        echo "[FAIL] ${name}: quesOS link failed"; cat "$log" >&2
        TOTAL=$((TOTAL + 1)); FAIL_COUNT=$((FAIL_COUNT + 1)); return
    fi

    nat=$(host_to_native "$progs")

    # Leg A: default quiet boot. No QUESOS_VERBOSE, so neither the [quesos] init: line nor
    # any [quesos] reaped line appears; the transcript is exactly argcheck's own argv[0]
    # plus the five gap-filled login-env keys (the exact match proves both gated lines are
    # absent), and the program still runs and reaps clean.
    TOTAL=$((TOTAL + 1))
    set +e
    quiet=$(MSYS2_ARG_CONV_EXCL='/progs' "$MAIZE" --no-root --mount "${nat}=/progs:ro" \
        "$quesos" /progs/argcheck.mzx 2>/dev/null | grep -v '^$')
    set -e
    expected_quiet=$(printf '%s\n' \
        '/progs/argcheck.mzx' \
        'HOME=/root' \
        'USER=root' \
        'LOGNAME=root' \
        'SHELL=/bin/oksh.mzx' \
        'PATH=/bin')
    if [ "$quiet" = "$expected_quiet" ]; then
        echo "[PASS] ${name}_default_quiet (no [quesos] init:/reaped; program still runs and reaps)"
    else
        echo "[FAIL] ${name}_default_quiet"
        echo "        expected transcript:"; printf '%s\n' "$expected_quiet" | sed 's/^/          | /'
        echo "        actual transcript:";   printf '%s\n' "$quiet"          | sed 's/^/          | /'
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi

    # Leg B: verbose boot. -e QUESOS_VERBOSE=1 brings back the gated init + reaped lines
    # exactly as before, and QUESOS_VERBOSE is stripped from the captured env so it never
    # reaches argcheck's envp dump even while armed (the exact match, with no QUESOS_VERBOSE
    # line in the expected envp block, proves the strip).
    TOTAL=$((TOTAL + 1))
    set +e
    verbose=$(MSYS2_ARG_CONV_EXCL='/progs' "$MAIZE" --no-root --mount "${nat}=/progs:ro" \
        -e QUESOS_VERBOSE=1 "$quesos" /progs/argcheck.mzx 2>/dev/null | grep -v '^$')
    set -e
    expected_verbose=$(printf '%s\n' \
        '[quesos] init: cause-7 handler resident; running 1 program(s)' \
        '/progs/argcheck.mzx' \
        'HOME=/root' \
        'USER=root' \
        'LOGNAME=root' \
        'SHELL=/bin/oksh.mzx' \
        'PATH=/bin' \
        '[quesos] reaped /progs/argcheck.mzx status=0')
    if [ "$verbose" = "$expected_verbose" ] \
        && ! printf '%s\n' "$verbose" | grep -q 'QUESOS_VERBOSE'; then
        echo "[PASS] ${name}_verbose (QUESOS_VERBOSE restores the init/reap trace; key stripped from envp)"
    else
        echo "[FAIL] ${name}_verbose"
        echo "        expected transcript:"; printf '%s\n' "$expected_verbose" | sed 's/^/          | /'
        echo "        actual transcript:";   printf '%s\n' "$verbose"          | sed 's/^/          | /'
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi

    # Leg C: a genuine error is still visible under default quiet boot. Boot names a /progs
    # entry that was never written into the mount, so sys_open fails, load_segments returns
    # -1 on the silent fd<0 branch, and spawn() returns 0. The ALWAYS "[quesos] cannot start"
    # line fires; the GATED "[quesos] init:" line stays absent on the same quiet run.
    TOTAL=$((TOTAL + 1))
    set +e
    err=$(MSYS2_ARG_CONV_EXCL='/progs' "$MAIZE" --no-root --mount "${nat}=/progs:ro" \
        "$quesos" /progs/does-not-exist.mzx 2>/dev/null | grep -v '^$')
    set -e
    expected_err='[quesos] cannot start /progs/does-not-exist.mzx'
    if [ "$err" = "$expected_err" ]; then
        echo "[PASS] ${name}_genuine_error_visible (ALWAYS 'cannot start' fires; GATED init: stays gated)"
    else
        echo "[FAIL] ${name}_genuine_error_visible"
        echo "        expected transcript:"; printf '%s\n' "$expected_err" | sed 's/^/          | /'
        echo "        actual transcript:";   printf '%s\n' "$err"          | sed 's/^/          | /'
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi
}

_mz_want "run_quesos_quiet_boot" && mz_timed "run_quesos_quiet_boot" run_quesos_quiet_boot

# maize-93 process ladder: the multi-process quesOS acceptance fixtures. Each is a C
# program compiled by the ordinary cc-maize.sh pipeline (stock .mzx) and run UNDER
# quesOS, which exercises fork (eager copy on Sv48), execve, waitpid/zombies, pipes +
# dup2 + per-process fd tables, and the preemptive round-robin timer scheduler. quesOS
# is linked once; each scenario runs its launcher off the worklist (exec/pipeline
# targets are built into /progs but not on the worklist). The gate is the fixture's own
# self-checked PASS marker in the transcript. Wrapped in `timeout` so a scheduler or
# blocking-semantics regression that livelocks is a failure, not a hung suite.
run_quesos_ac_fixtures() {
    builder="$MZCC"                              # maize-382: mzcc build-quesos, not the .sh
    progs="${WORK_DIR}/quesos-ac"
    quesos="${WORK_DIR}/quesos-ac.mzx"
    log="${WORK_DIR}/quesos-ac.log"
    rm -rf "$progs"; mkdir -p "$progs"

    if ! "$builder" build-quesos --preset "$PRESET" -o "$quesos" >"$log" 2>&1 || [ ! -f "$quesos" ]; then
        echo "[FAIL] quesos_ac: quesOS link failed"; cat "$log" >&2
        TOTAL=$((TOTAL + 1)); FAIL_COUNT=$((FAIL_COUNT + 1)); return
    fi
    # maize-304: this loop is the densest fixture-compile section in the harness (~34
    # sources, each ~60-90 forked processes through cc-maize.sh). An earlier maize-304
    # cycle gated it with a machine-wide lock; the operator dropped that gate (comment
    # #3133) rather than land a cross-process lock. There is no cross-process lock
    # here: heavy Test-stage runs are expected to be run one at a time by orchestration
    # discipline, and cc_maize_compile_bounded's timeout wrap below bounds the damage
    # if two ever overlap anyway.
    for src in fork_isolation fork_multi exec_launch exec_target pipe_roundtrip \
               pipe_bigwrite bigwrite_native pipeline producer filter consumer stress20 preempt \
               blocked console_echo \
               sig_handler sig_default sig_chld sig_pgroup \
               sig_kill sig_exec_launch sig_exec_target \
               fb_register fb_reject fb_fork_cleanup fb_exec_launch fb_exec_target \
               fb_exit_cleanup \
               socketpair_echo unix_listen_connect unix_listen_close \
               poll_timeout poll_broken poll_multi select_console_pipe \
               poll_fd0_default stdin_owner_probe poll_unconnected_sock poll_fd0_eof \
               fb_mmap_paint fb_noncontig_reject fb_mmap_isolation fb_mmap_enomem \
               bulk_forward bulk_noncontig bulk_bounds bulk_kernel_range \
               bigalloc bigalloc_coalesce fb_present kbd_acl bigfootprint_fork loader_guard bigimage \
               palette_blit_guard satp_stress; do
        if ! cc_maize_compile_bounded "quesos_ac: ${src}.c" "${progs}/${src}.mzx" \
                "${REPO_ROOT}/os/quesos/${src}.c" "$log"; then
            TOTAL=$((TOTAL + 1)); FAIL_COUNT=$((FAIL_COUNT + 1))
            return
        fi
    done
    nat=$(host_to_native "$progs")

    # An optional 4th arg passes extra maize flags (used by the maize-236 fb_reject case,
    # which needs --fb-no-display so the device rejects the claim per-exec instead of
    # accepting it). Left empty, the invocation is byte-identical to before.
    quesos_ac_case() {
        name="$1"; marker="$2"; launcher="$3"; extra="${4:-}"
        TOTAL=$((TOTAL + 1))
        set +e
        out=$(MSYS2_ARG_CONV_EXCL='/progs' timeout 90 "$MAIZE" $extra --no-root \
            --mount "${nat}=/progs:ro" "$quesos" "/progs/${launcher}.mzx" 2>/dev/null \
            | grep -v '^$')
        set -e
        if printf '%s\n' "$out" | grep -qF "$marker"; then
            echo "[PASS] ${name}"
        else
            echo "[FAIL] ${name}"
            echo "        expected marker: \"${marker}\""
            printf '%s\n' "$out" | sed 's/^/          | /'
            FAIL_COUNT=$((FAIL_COUNT + 1))
        fi
    }

    quesos_ac_case quesos_fork_isolation "fork-isolation: PASS"  fork_isolation
    quesos_ac_case quesos_wait_anyorder  "wait-anyorder: PASS"   fork_multi
    quesos_ac_case quesos_execve         "exec: PASS"            exec_launch
    quesos_ac_case quesos_pipe_roundtrip "pipe-roundtrip: PASS"  pipe_roundtrip
    quesos_ac_case quesos_pipe_bigwrite  "pipe-bigwrite: PASS"   pipe_bigwrite
    quesos_ac_case quesos_pipeline       "pipeline: PASS"        pipeline
    quesos_ac_case quesos_stress20       "stress20: PASS"        stress20
    quesos_ac_case quesos_preempt        "preempt: PASS"         preempt
    quesos_ac_case quesos_blocked        "blocked-noslice: PASS" blocked

    # maize-250 (AC 9108): native_write must deliver a single write larger than
    # QUESOS_IOBUF_CAP (4096) in FULL, not silently truncate the tail (the root cause of
    # kilo's garbled full-screen paint). The fixture does ONE sys_write(1, buf, 10000) and
    # self-checks the returned count == 10000; the harness ALSO wc -c's stdout to confirm the
    # 10000-byte payload physically reached the host (belt-and-suspenders on the guest-side
    # return check). Before the fix the write returned 4096 and 5904 bytes were dropped.
    TOTAL=$((TOTAL + 1))
    set +e
    out=$(MSYS2_ARG_CONV_EXCL='/progs' timeout 90 "$MAIZE" --no-root \
        --mount "${nat}=/progs:ro" "$quesos" /progs/bigwrite_native.mzx 2>/dev/null)
    nbytes=$(printf '%s' "$out" | wc -c)
    set -e
    if printf '%s\n' "$out" | grep -qF "native-bigwrite: PASS n=10000" && [ "$nbytes" -ge 10000 ]; then
        echo "[PASS] quesos_bigwrite_native (single >4096-byte write delivered in full)"
    else
        echo "[FAIL] quesos_bigwrite_native (payload bytes on host: ${nbytes})"
        printf '%s\n' "$out" | grep -aF "native-bigwrite" | sed 's/^/          | /'
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi

    # maize-250 (AC 9114): the SAME native_write choke point feeds the windowed console's
    # text_console::write_out backend (bound headlessly by --console-dump) as feeds the
    # host_tty terminal path. Re-run the single >4096-byte write through that backend and
    # confirm the guest still observes the full 10000-byte count returned -- a concrete,
    # non-structural proof that the truncation fix reaches the windowed-console output path
    # (the full interactive kilo paint in an actual SDL window is operator-verified; headless
    # CI has no window, and windowed-console screen sizing is outside this card's scope).
    TOTAL=$((TOTAL + 1))
    set +e
    out=$(printf '' | MSYS2_ARG_CONV_EXCL='/progs' timeout 60 "$MAIZE" --console-dump \
        --no-root --mount "${nat}=/progs:ro" "$quesos" /progs/bigwrite_native.mzx 2>/dev/null)
    set -e
    if printf '%s\n' "$out" | grep -qF "native-bigwrite: PASS n=10000"; then
        echo "[PASS] quesos_bigwrite_native_windowed (full delivery via text_console backend)"
    else
        echo "[FAIL] quesos_bigwrite_native_windowed"
        printf '%s\n' "$out" | grep -aF "native-bigwrite" | sed 's/^/          | /'
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi

    # maize-174 guest signal subsystem. sig_handler proves the handler-dispatch path
    # (rt_sigaction -> kill -> user trampoline -> rt_sigreturn -> resume) deterministically
    # via a pipe-synchronized fork, with no console-input timing dependency.
    quesos_ac_case quesos_sig_handler    "sig-handler: PASS"     sig_handler
    quesos_ac_case quesos_sig_default    "sig-default: PASS"     sig_default
    quesos_ac_case quesos_sig_chld       "sig-chld: PASS"        sig_chld
    quesos_ac_case quesos_sig_pgroup     "sig-pgroup: PASS"      sig_pgroup
    quesos_ac_case quesos_sig_kill       "sig-kill: PASS"        sig_kill
    quesos_ac_case quesos_sig_exec       "sig-exec: PASS"        sig_exec_launch

    # maize-236 framebuffer registration table (quesOS half). fb_register: geometry +
    # slot 0 + -EBUSY + release/re-register. fb_reject: -ENODEV on a display-less view
    # (--fb-no-display) with the VM still running. fb_fork/exec/exit: fork non-propagation
    # (D4), exec-time release (D5), and exit-time release, each proven by a later
    # registration reclaiming slot 0.
    quesos_ac_case quesos_fb_register    "fb-register: PASS"     fb_register
    quesos_ac_case quesos_fb_reject      "fb-reject: PASS"       fb_reject       --fb-no-display
    quesos_ac_case quesos_fb_fork        "fb-fork: PASS"         fb_fork_cleanup
    quesos_ac_case quesos_fb_exec        "fb-exec: PASS"         fb_exec_launch
    quesos_ac_case quesos_fb_exit        "fb-exit: PASS"         fb_exit_cleanup

    # maize-238 Phase 3. Family A (unix sockets): socketpair full duplex + EOF/EPIPE;
    # bind/listen/accept/connect handshake ordering; listen-close wakes parked connectors
    # with -ECONNREFUSED and frees the path. Family B (select/poll): timeout semantics,
    # broken-pipe POLLERR, and the injector-mode fd-0 legs (--input=console cases below).
    # Family C (fb mmap): full ~63-page contiguity end to end, non-contiguous-buffer
    # rejection, per-process isolation, and graceful -ENOMEM pool exhaustion.
    quesos_ac_case quesos_socketpair     "socketpair-echo: PASS"      socketpair_echo
    quesos_ac_case quesos_unix_connect   "unix-listen-connect: PASS"  unix_listen_connect
    quesos_ac_case quesos_unix_close     "unix-listen-close: PASS"    unix_listen_close
    quesos_ac_case quesos_poll_timeout   "poll-timeout: PASS"         poll_timeout
    quesos_ac_case quesos_poll_broken    "poll-broken: PASS"          poll_broken
    quesos_ac_case quesos_poll_unconn    "poll-unconn-sock: PASS"     poll_unconnected_sock
    quesos_ac_case quesos_fb_mmap_paint  "fb-mmap-paint: PASS"        fb_mmap_paint
    quesos_ac_case quesos_fb_noncontig   "fb-noncontig: PASS"         fb_noncontig_reject
    quesos_ac_case quesos_fb_isolation   "fb-isolation: PASS"         fb_mmap_isolation

    # maize-251 guest-OS display surface (the DOOM-under-quesOS syscalls). bigalloc: size-0
    # -EINVAL, over-16-MiB -ENOMEM, writable+readback, and fork-exclusion (child's window is
    # NOT the parent's eager copy). fb_present: -EBADF unregistered, then present-after-register
    # returns 0 with no crash. Both run headless like the other fb cases.
    quesos_ac_case quesos_bigalloc       "bigalloc: PASS"             bigalloc
    # maize-348 rt bigalloc-window coalescing (AC 9927/9928/9930). Proves freed window-backed
    # grants merge and reuse (leg 1), that kilo's one-row-at-a-time realloc growth stays bounded
    # post-fix where it burned the window quadratically pre-fix (leg 2, the negative control),
    # and that a child's first post-fork grant is fresh and bounded despite the inherited cursor
    # (leg 3). Runs headless like the other window cases.
    quesos_ac_case quesos_bigalloc_coalesce "bigalloc-coalesce: PASS" bigalloc_coalesce
    quesos_ac_case quesos_fb_present     "fb-present: PASS"           fb_present

    # maize-251 sys_kbd_read ACL (AC 9315). Unlike the other quesos_ac cases this one needs a
    # latched scancode, so it runs with --input=keyboard and a single Set-1 byte (0x1E, 'a')
    # piped on stdin -- NOT the /dev/null stdin quesos_ac_case uses. The fixture proves all
    # three legs with one scancode: non-owner+pending -> -EACCES (no consume), owner+pending ->
    # the scancode, owner+none -> -EAGAIN. quesOS's new vector-34 keyboard IRQ sink keeps the
    # injected key from halting the VM.
    TOTAL=$((TOTAL + 1))
    set +e
    out=$(printf '\036' | MSYS2_ARG_CONV_EXCL='/progs' timeout 90 "$MAIZE" --input=keyboard \
        --no-root --mount "${nat}=/progs:ro" "$quesos" /progs/kbd_acl.mzx 2>/dev/null \
        | grep -v '^$')
    set -e
    if printf '%s\n' "$out" | grep -qF "kbd-acl: PASS"; then
        echo "[PASS] quesos_kbd_acl"
    else
        echo "[FAIL] quesos_kbd_acl"
        echo "        expected marker: \"kbd-acl: PASS\""
        printf '%s\n' "$out" | sed 's/^/          | /'
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi

    # maize-251 address-space addendum. bigfootprint_fork: a 320 KiB-BSS + 80 KiB-deep-stack
    # program forks and the child verifies BOTH regions copied byte-identically -- proves the
    # generalized do_fork L1-walk copies region 0 AND the relocated stack region. loader_guard:
    # a child execve's an oversized image (BSS past USER_BRK_MAX); the load_segments guard fires
    # post-teardown so the child exits 127 cleanly and the parent (VM) survives. bigimage is the
    # oversized target (built to /progs, never on the worklist).
    quesos_ac_case quesos_bigfootprint_fork "bigfootprint-fork: PASS" bigfootprint_fork
    quesos_ac_case quesos_loader_guard      "loader-guard: PASS"      loader_guard

    # maize-251 (Code Review #2961): do_palette_blit's ($F3) memory-safety guards. A crafted
    # kernel/unmapped VA or a base+len/npixels*4 wrap must return -EFAULT up front (never reach
    # the unguarded user_pa/as_write translation path), with no corruption (sentinel-checked).
    quesos_ac_case quesos_palette_guard     "palette-guard: PASS"     palette_blit_guard

    # maize-247: forward the bulk-memory accelerators ($F4 sys_bulk_copy / $F5 sys_bulk_set)
    # under quesOS paging. bulk_forward proves the raw contiguous forward (rv == n) for both
    # copy and set plus the libc entrypoint smoke test; bulk_noncontig proves $F4's
    # contiguity-check -ENOSYS branch via a fork-interleaved physically-scattered buffer (the
    # deterministic rv discriminator, cycle 3); bulk_bounds and bulk_kernel_range prove the
    # QOS_EFAULT gate (unmapped page / kernel-owned PTE_U=0 VA) rejects with no partial write
    # (sentinel verified).
    quesos_ac_case quesos_bulk_forward   "bulk-forward: PASS"         bulk_forward
    quesos_ac_case quesos_bulk_noncontig "bulk-noncontig: PASS"       bulk_noncontig
    quesos_ac_case quesos_bulk_bounds    "bulk-bounds: PASS"          bulk_bounds
    quesos_ac_case quesos_bulk_kernel    "bulk-kernel: PASS"          bulk_kernel_range

    # maize-238 fb-mmap pool exhaustion: repeated fork+fb_mmap+exit until alloc_frames_contig
    # returns -ENOMEM gracefully (no PANIC/poweroff). Slower than the others (it drains the
    # 64 MiB pool one ~63-page buffer per exited child, ~200 fork+eager-copy+wait cycles), so
    # it gets its own longer timeout -- generous enough for the ASan/UBSan leg, whose ~5-10x
    # instrumentation overhead pushed the exhaustion past the old 180s cap (the pre-existing
    # linux-sanitizers red; the guest frame accounting is identical, so it PASSES given time).
    TOTAL=$((TOTAL + 1))
    set +e
    out=$(MSYS2_ARG_CONV_EXCL='/progs' timeout 480 "$MAIZE" --no-root \
        --mount "${nat}=/progs:ro" "$quesos" /progs/fb_mmap_enomem.mzx 2>/dev/null | grep -v '^$')
    set -e
    if printf '%s\n' "$out" | grep -qF "fb-enomem: PASS"; then
        echo "[PASS] quesos_fb_enomem (alloc_frames_contig -ENOMEM, VM still running)"
    else
        echo "[FAIL] quesos_fb_enomem"
        printf '%s\n' "$out" | sed 's/^/          | /'
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi

    # maize-238 injector-mode fd-0 poll/select legs (AC 9190/9192): launched WITH
    # --input=console (the console as the active stdin injector) with a byte piped on
    # stdin, exactly as the console_echo case below. poll_multi polls a pipe fd, a socket
    # fd, and fd 0; select_console_pipe selects fd 0 + a pipe fd. Each self-checks PASS.
    for case in poll_multi:poll-multi select_console_pipe:select-console-pipe; do
        launcher="${case%%:*}"; marker="${case#*:}: PASS"
        TOTAL=$((TOTAL + 1))
        set +e
        out=$(printf 'Z' | MSYS2_ARG_CONV_EXCL='/progs' timeout 60 "$MAIZE" --input=console \
            --no-root --mount "${nat}=/progs:ro" "$quesos" "/progs/${launcher}.mzx" 2>/dev/null \
            | grep -v '^$')
        set -e
        if printf '%s\n' "$out" | grep -qF "$marker"; then
            echo "[PASS] quesos_${launcher}"
        else
            echo "[FAIL] quesos_${launcher}"
            printf '%s\n' "$out" | sed 's/^/          | /'
            FAIL_COUNT=$((FAIL_COUNT + 1))
        fi
    done

    # Console input rides the device IRQ/status path (vector 33), not a native blocking
    # read, so a parked fd-0 reader never freezes the VM (design doc 17). This case pipes
    # a known line and runs with --input=console (the console device as the active stdin
    # injector); the fixture parks on each byte, the console IRQ delivers it, and the
    # kernel idle-spins while the sole reader waits.
    TOTAL=$((TOTAL + 1))
    set +e
    out=$(printf 'hi\n' | MSYS2_ARG_CONV_EXCL='/progs' timeout 30 "$MAIZE" --input=console \
        --no-root --mount "${nat}=/progs:ro" "$quesos" /progs/console_echo.mzx 2>/dev/null \
        | grep -v '^$')
    set -e
    if printf '%s\n' "$out" | grep -qF "console: PASS"; then
        echo "[PASS] quesos_console_input"
    else
        echo "[FAIL] quesos_console_input"
        printf '%s\n' "$out" | sed 's/^/          | /'
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi

    # maize-238 Branch A (AC 9199): poll/select on fd 0 work on the plain DEFAULT invocation
    # (NO --input flag -- the operator's primary path), previously a deadlock. A quesOS test
    # program polls then selects fd 0 with two bytes piped on stdin; the migrated IRQ/readiness
    # model wakes it. This is the piped-stdin form of the pty fixture (the real-pty keystroke
    # variant is the Linux-only pty harness; native Windows is verified separately).
    TOTAL=$((TOTAL + 1))
    set +e
    out=$(printf 'XY' | MSYS2_ARG_CONV_EXCL='/progs' timeout 30 "$MAIZE" --no-root \
        --mount "${nat}=/progs:ro" "$quesos" /progs/poll_fd0_default.mzx 2>/dev/null | grep -v '^$')
    set -e
    if printf '%s\n' "$out" | grep -qF "poll-fd0-default: PASS"; then
        echo "[PASS] quesos_poll_fd0_default (default-path poll/select on fd 0)"
    else
        echo "[FAIL] quesos_poll_fd0_default"
        printf '%s\n' "$out" | sed 's/^/          | /'
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi

    # maize-238 Branch A (review cycle 1): a PARKED poll() on fd 0 wakes on console EOF, not
    # only on a keystroke (console EOF is poll-readable, the POSIX model). The feeder holds
    # stdin open and empty (a sleep that writes nothing) so the guest's poll(fd0,-1) genuinely
    # parks, then closes it; the readiness IRQ wakes the parked poll and the follow-on read
    # returns 0. `sleep 3` bounds the open window; the guest wakes and exits as soon as the
    # feeder closes, so the case runs in about that long, not longer.
    TOTAL=$((TOTAL + 1))
    set +e
    out=$(sleep 3 | MSYS2_ARG_CONV_EXCL='/progs' timeout 30 "$MAIZE" --no-root \
        --mount "${nat}=/progs:ro" "$quesos" /progs/poll_fd0_eof.mzx 2>/dev/null | grep -v '^$')
    set -e
    if printf '%s\n' "$out" | grep -qF "poll-fd0-eof: PASS"; then
        echo "[PASS] quesos_poll_fd0_eof (parked poll wakes on console EOF)"
    else
        echo "[FAIL] quesos_poll_fd0_eof"
        printf '%s\n' "$out" | sed 's/^/          | /'
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi

    # maize-238 Branch A byte-theft proof: a BARE-VM guest (run directly, not under quesOS)
    # reads fd 0 via SYS $00 on the default path, where the console device is the active stdin
    # injector eagerly pre-reading host stdin. sys.cpp routes the guest read through the device
    # latch (single host-stdin owner), so piping "hello" must round-trip all five bytes with
    # none stolen by the eager reader.
    TOTAL=$((TOTAL + 1))
    set +e
    out=$(printf 'hello' | MSYS2_ARG_CONV_EXCL='/progs' timeout 20 "$MAIZE" --no-root \
        "${progs}/stdin_owner_probe.mzx" 2>/dev/null | grep -v '^$')
    set -e
    if printf '%s\n' "$out" | grep -qF "stdin-owner: PASS"; then
        echo "[PASS] quesos_stdin_owner (bare-VM default-path stdin, no byte theft)"
    else
        echo "[FAIL] quesos_stdin_owner"
        printf '%s\n' "$out" | sed 's/^/          | /'
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi

    # maize-346 (AC 9830): SATP-safety of the paged JIT cross-page + indirect-transfer probe.
    # satp_stress forks NCHILD children that each run a long hot loop of indirect (function-
    # pointer) dispatches under quesOS paging; the scheduler forces SATP context switches
    # while paged probe entries and cross-page transfers are live. Two proofs. (1) Marker: it
    # runs to a deterministic PASS. (2) The real oracle: byte-identical stdout under plain
    # --jit vs the interpreter. --jit-check disables both the probe and chaining, so it cannot
    # exercise this path; a probe entry pointing at the wrong physical block after an SATP
    # switch would change a child's hash and diverge the two streams. Run through BARE_MAIZE
    # (maize-360: the --bare, non-JIT wrapper) rather than $MAIZE, which the MAIZE_JIT wrapper
    # may have pinned to one mode, so this comparison always runs interpreter-vs-JIT.
    quesos_ac_case quesos_satp_stress "satp-stress: PASS" satp_stress

    raw_maize="$BARE_MAIZE"   # maize-360: bare (so it runs the raw image), non-JIT wrapper
    TOTAL=$((TOTAL + 1))
    if [ -z "$raw_maize" ]; then
        echo "[FAIL] quesos_satp_jit_equiv: raw maize binary not found in ${BUILD_DIR}"
        FAIL_COUNT=$((FAIL_COUNT + 1))
    else
        satp_int="${WORK_DIR}/satp_stress.interp.out"
        satp_jit="${WORK_DIR}/satp_stress.jit.out"
        set +e
        MSYS2_ARG_CONV_EXCL='/progs' timeout 120 "$raw_maize" --no-root \
            --mount "${nat}=/progs:ro" "$quesos" /progs/satp_stress.mzx >"$satp_int" 2>/dev/null
        MSYS2_ARG_CONV_EXCL='/progs' timeout 120 "$raw_maize" --jit --jit-threshold 50 --no-root \
            --mount "${nat}=/progs:ro" "$quesos" /progs/satp_stress.mzx >"$satp_jit" 2>/dev/null
        set -e
        if grep -qF "satp-stress: PASS" "$satp_int" && cmp -s "$satp_int" "$satp_jit"; then
            echo "[PASS] quesos_satp_jit_equiv (--jit stdout byte-identical to interpreter under SATP churn)"
        else
            echo "[FAIL] quesos_satp_jit_equiv"
            echo "        interp: \"$(tr '\n' '|' < "$satp_int")\""
            echo "        jit:    \"$(tr '\n' '|' < "$satp_jit")\""
            FAIL_COUNT=$((FAIL_COUNT + 1))
        fi
    fi
}

_mz_want "run_quesos_ac_fixtures" && mz_timed "run_quesos_ac_fixtures" run_quesos_ac_fixtures

# maize-251 DOOM as a quesOS child: the North Star payoff. Boots the WHOLE DOOM engine as a
# quesOS worklist child against the synthetic min-IWAD, with video/input flowing through the
# fb registration + fb-mmap + keyboard-port machinery and DOOM's ~6 MiB zone heap satisfied by
# sys_bigalloc (over the sbrk ceiling). Three legs:
#   - render (both CI legs, AC 9309/9310): headless quesOS child renders + presents; the same
#     pixel-exact 3-point viewport check asserts a real 3D frame reading DG_MaizeFB. "doom: PASS".
#   - exit-4 (both CI legs, AC 9500): --fb-no-display makes sys_fb_register -ENODEV, so DG_Init
#     sets DG_MaizeInitError==3; doom_main.c's diagnostic path prints "doom: framebuffer init
#     failed (code 3)" to stderr and the child exits 4 (quesOS reaps status=4, VM keeps running).
#   - pty checksum (Linux only, AC 9499): under a real pty the full maize-264 launch-or-attach
#     machinery engages; the quesOS child's fb_register triggers the same presenter-launch hook
#     a bare-VM registration would, and the stub's FNV-1a checksum of the 320x200 frame matches
#     a pinned value.
# Same submodule-presence [SKIP] guard, --dev compile, mount, and MSYS2_ARG_CONV_EXCL handling
# as run_doom_render.
run_doom_quesos() {
    name="doom-quesos"
    doom_dir="${REPO_ROOT}/demos/doom"
    render="${doom_dir}/doom_render_selfcheck_quesos.c"
    platform="${doom_dir}/doomgeneric_maize.c"
    sources="${doom_dir}/doom.sources"
    generator="${doom_dir}/tools/make_min_iwad.c"
    builder="$MZCC"                              # maize-382: mzcc build-quesos, not the .sh
    probe="${doom_dir}/doomgeneric/doomgeneric/doomgeneric.c"

    if [ ! -f "$probe" ]; then
        echo "[SKIP] ${name}: demos/doom/doomgeneric submodule not initialized" \
             "(run 'git submodule update --init demos/doom/doomgeneric'); skipping DOOM-quesOS gate"
        return
    fi

    # Build quesOS once (its own non-default base link).
    quesos="${WORK_DIR}/doom-quesos.mzx"
    log="${WORK_DIR}/doom-quesos.log"
    if ! "$builder" build-quesos --preset "$PRESET" -o "$quesos" >"$log" 2>&1 || [ ! -f "$quesos" ]; then
        echo "[FAIL] ${name}: quesOS link failed"; cat "$log" >&2
        TOTAL=$((TOTAL + 1)); FAIL_COUNT=$((FAIL_COUNT + 1)); return
    fi

    # System C compiler for the synthetic IWAD generator (present on both CI hosts).
    gen_cc="${CC:-}"
    if [ -z "$gen_cc" ]; then
        if command -v cc >/dev/null 2>&1; then gen_cc=cc; else gen_cc=gcc; fi
    fi
    gen_exe="${WORK_DIR}/make_min_iwad_quesos"
    if ! "$gen_cc" -O2 -o "$gen_exe" "$generator" >>"$log" 2>&1; then
        echo "[FAIL] ${name}: min-IWAD generator failed to compile"; cat "$log" >&2
        TOTAL=$((TOTAL + 1)); FAIL_COUNT=$((FAIL_COUNT + 1)); return
    fi
    waddir="${WORK_DIR}/doom-quesos-wad"
    rm -rf "$waddir"; mkdir -p "$waddir"
    if ! "$gen_exe" "${waddir}/min.wad" >>"$log" 2>&1 || [ ! -f "${waddir}/min.wad" ]; then
        echo "[FAIL] ${name}: min-IWAD generation failed"; cat "$log" >&2
        TOTAL=$((TOTAL + 1)); FAIL_COUNT=$((FAIL_COUNT + 1)); return
    fi

    # Link the quesOS-child render harness: doom.sources core + platform + mzdev (--dev) + RT
    # libc, at the 320x200 geometry override, into a /progs mount dir.
    progs="${WORK_DIR}/doom-quesos-progs"
    rm -rf "$progs"; mkdir -p "$progs"
    child="${progs}/doom_render_selfcheck_quesos.mzx"
    if ! "$CC_MAIZE" --preset "$PRESET" --dev \
        -D DOOMGENERIC_RESX=320 -D DOOMGENERIC_RESY=200 \
        -o "$child" --sources "$sources" "$render" "$platform" >>"$log" 2>&1 \
    || [ ! -f "$child" ]; then
        echo "[FAIL] ${name}: quesOS-child render C compile/link failed"; cat "$log" >&2
        TOTAL=$((TOTAL + 1)); FAIL_COUNT=$((FAIL_COUNT + 1)); return
    fi

    nat_progs=$(host_to_native "$progs")
    nat_wad=$(host_to_native "$waddir")

    # Leg 1: headless render (both CI legs). The synthetic IWAD path is baked into the child's
    # synthesized argv (/ro/min.wad); MSYS2_ARG_CONV_EXCL exempts /progs and /ro from the
    # POSIX->Windows argv rewrite on the MinGW leg (see run_doom_render).
    TOTAL=$((TOTAL + 1))
    set +e
    actual=$(MSYS2_ARG_CONV_EXCL='/progs;/ro' timeout 120 "$MAIZE" --no-root \
        --mount "${nat_progs}=/progs:ro" --mount "${nat_wad}=/ro:ro" \
        "$quesos" /progs/doom_render_selfcheck_quesos.mzx 2>/dev/null | grep -v '^$')
    set -e
    if printf '%s\n' "$actual" | grep -qx "doom: PASS"; then
        echo "[PASS] ${name}_render"
    else
        echo "[FAIL] ${name}_render"
        echo "        expected a \"doom: PASS\" line (quesOS-mediated 3D viewport render)"
        echo "        actual:   \"$(printf '%s' "$actual" | tail -4 | tr '\n' '|')\""
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi

    # Leg 2: --fb-no-display exit-code-4 diagnostic (both CI legs). sys_fb_register -ENODEV ->
    # DG_MaizeInitError==3 -> doom_main.c-style diagnostic + child exit 4; quesOS reaps status=4
    # and keeps running. Capture stdout+stderr together.
    # maize-379: the reap-status assertion below reads the "[quesos] reaped" line that
    # maize-372 gated behind QUESOS_VERBOSE, so opt into verbose boot with
    # -e QUESOS_VERBOSE=1. Leg 1 above asserts only doom's own output and stays quiet.
    TOTAL=$((TOTAL + 1))
    set +e
    ejout=$(MSYS2_ARG_CONV_EXCL='/progs;/ro' timeout 120 "$MAIZE" --fb-no-display --no-root \
        --mount "${nat_progs}=/progs:ro" --mount "${nat_wad}=/ro:ro" \
        -e QUESOS_VERBOSE=1 "$quesos" /progs/doom_render_selfcheck_quesos.mzx 2>&1)
    set -e
    if printf '%s\n' "$ejout" | grep -qF "doom: framebuffer init failed (code 3)" \
    && printf '%s\n' "$ejout" | grep -qF "reaped /progs/doom_render_selfcheck_quesos.mzx status=4"; then
        echo "[PASS] ${name}_exit4"
    else
        echo "[FAIL] ${name}_exit4"
        echo "        expected stderr \"doom: framebuffer init failed (code 3)\" + reap status=4"
        echo "        actual:   \"$(printf '%s' "$ejout" | tail -4 | tr '\n' '|')\""
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi

    # Leg 3: cross-process pty checksum (Linux only; the Windows leg rides AC 9308). Needs a real
    # pty so the launch-or-attach machinery engages. Skipped where python3/pty is unavailable.
    case "$UNAME" in
        Linux)
            if command -v python3 >/dev/null 2>&1; then
                TOTAL=$((TOTAL + 1))
                set +e
                # Budget covers the fixture's content-based stabilization wait (maize-251): it
                # polls until DOOM's frame settles by content rather than sampling after a fixed
                # delay. With the selfcheck's deterministic clock the render settles fast on both
                # legs, but keep headroom for the slow asan/ubsan engine boot; 240s is ample.
                ptyout=$(timeout 240 python3 "${REPO_ROOT}/scripts/pty_presenter_doom_check.py" \
                    "$MAIZE" "$quesos" "$child" "$progs" "$waddir" 2>&1)
                set -e
                if printf '%s\n' "$ptyout" | grep -qF "pty-presenter-doom: PASS"; then
                    echo "[PASS] ${name}_pty"
                else
                    echo "[FAIL] ${name}_pty"
                    printf '%s\n' "$ptyout" | tail -6 | sed 's/^/          | /'
                    FAIL_COUNT=$((FAIL_COUNT + 1))
                fi
            else
                echo "[SKIP] ${name}_pty: python3 not available (Linux-only pty leg)"
            fi
            ;;
        *)
            echo "[SKIP] ${name}_pty: pty checksum leg is Linux-only (Windows rides AC 9308)"
            ;;
    esac
}

_mz_want "run_doom_quesos" && mz_timed "run_doom_quesos" run_doom_quesos

# maize-94 wave-1 kernel plumbing: quesOS forwards the native hostfs file/dir subset
# (decision 8941), owns a per-process cwd + relative-path resolution (decision 8940), and
# forwards the console termios calls (OQ 8951 operator ruling) so oksh can enter raw mode.
# fs_forward + cwd_resolve run under a writable /rw mount (alongside :ro /progs for the
# binary); termios_raw runs under --console-dump (which binds the grid console's termios).
# Each is a self-checked PASS marker; `timeout` guards a blocking-semantics regression.
run_quesos94_fixtures() {
    builder="$MZCC"                              # maize-382: mzcc build-quesos, not the .sh
    progs="${WORK_DIR}/quesos94"
    rw="${WORK_DIR}/quesos94-rw"
    bin="${WORK_DIR}/quesos94-bin"
    quesos="${WORK_DIR}/quesos94.mzx"
    log="${WORK_DIR}/quesos94.log"
    rm -rf "$progs" "$rw" "$bin"; mkdir -p "$progs" "$rw" "$bin"

    if ! "$builder" build-quesos --preset "$PRESET" -o "$quesos" >"$log" 2>&1 || [ ! -f "$quesos" ]; then
        echo "[FAIL] quesos94: quesOS link failed"; cat "$log" >&2
        TOTAL=$((TOTAL + 1)); FAIL_COUNT=$((FAIL_COUNT + 1)); return
    fi
    for src in fs_forward cwd_resolve termios_raw ttysize_console raw_reap_restore libc_proc execvp_run bin_echoer setjmp_launch; do
        if ! cc_maize_compile_bounded "quesos94: ${src}.c" "${progs}/${src}.mzx" \
                "${REPO_ROOT}/os/quesos/${src}.c" "$log"; then
            TOTAL=$((TOTAL + 1)); FAIL_COUNT=$((FAIL_COUNT + 1)); return
        fi
    done
    cp "${progs}/bin_echoer.mzx" "${bin}/bin_echoer.mzx"   # execvp PATH search target
    pnat=$(host_to_native "$progs")
    rnat=$(host_to_native "$rw")
    bnat=$(host_to_native "$bin")

    # File/dir forwarding + cwd resolution: :ro /progs (binaries) + :rw /rw (scratch).
    for case in fs_forward:fs-forward cwd_resolve:cwd-resolve; do
        launcher="${case%%:*}"; marker="${case#*:}: PASS"
        TOTAL=$((TOTAL + 1))
        rm -rf "${rw:?}/"* 2>/dev/null || true
        set +e
        # MSYS2_ARG_CONV_EXCL is a SEMICOLON-separated list of prefixes to exempt from the
        # MSYS2 POSIX->Windows argv rewrite; a colon-separated value is one literal prefix
        # ("/progs:/rw") that matches nothing, so the guest worklist path /progs/<x>.mzx
        # was rewritten to D:/.../progs/<x>.mzx and quesOS could not load it (Windows-leg
        # regression). Single-value cases work by accident (no separator); multi-value MUST
        # use ';'.
        out=$(MSYS2_ARG_CONV_EXCL='/progs;/rw' timeout 60 "$MAIZE" --no-root \
            --mount "${pnat}=/progs:ro" --mount "${rnat}=/rw:rw" \
            "$quesos" "/progs/${launcher}.mzx" 2>/dev/null | grep -v '^$')
        set -e
        if printf '%s\n' "$out" | grep -qF "$marker"; then
            echo "[PASS] quesos94_${launcher}"
        else
            echo "[FAIL] quesos94_${launcher}"
            echo "        expected marker: \"${marker}\""
            printf '%s\n' "$out" | sed 's/^/          | /'
            FAIL_COUNT=$((FAIL_COUNT + 1))
        fi
    done

    # Console termios forwarding (OQ 8951): --console-dump binds the grid console's termios
    # so tcgetattr/tcsetattr return 0; the fixture's get/set/get round trip proves the
    # forwarding + bounce path. The PASS marker rides the grid dump.
    TOTAL=$((TOTAL + 1))
    set +e
    out=$(printf '' | MSYS2_ARG_CONV_EXCL='/progs' timeout 60 "$MAIZE" --console-dump \
        --no-root --mount "${pnat}=/progs:ro" "$quesos" /progs/termios_raw.mzx 2>/dev/null)
    set -e
    if printf '%s\n' "$out" | grep -qF "termios-raw: PASS"; then
        echo "[PASS] quesos94_termios_raw"
    else
        echo "[FAIL] quesos94_termios_raw"
        printf '%s\n' "$out" | sed 's/^/          | /'
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi

    # Console terminal-size forwarding (maize-253): --console-dump --console-size 120x40 binds
    # a 120x40 grid console, so the forwarded SYS $F6 (do_ttysize -> native sys_ttysize) reports
    # that cell grid instead of returning -ENOTTY. The fixture asserts rv=0 and rows=40/cols=120.
    # The PASS marker rides the grid dump.
    TOTAL=$((TOTAL + 1))
    set +e
    out=$(printf '' | MSYS2_ARG_CONV_EXCL='/progs' timeout 60 "$MAIZE" --console-dump --console-size 120x40 \
        --no-root --mount "${pnat}=/progs:ro" "$quesos" /progs/ttysize_console.mzx 2>/dev/null)
    set -e
    if printf '%s\n' "$out" | grep -qF "ttysize-console: PASS"; then
        echo "[PASS] quesos94_ttysize_console"
    else
        echo "[FAIL] quesos94_ttysize_console"
        printf '%s\n' "$out" | sed 's/^/          | /'
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi

    # maize-250 killed-TUI restore (AC 9112, way 1): a raw-mode child killed by a signal must
    # not strand the console raw. --console-dump binds the grid console's termios; the fixture
    # forks a raw child, SIGTERMs it, and asserts the console's ICANON is restored to the
    # parent's canonical image by reap_tail -> restore_console_on_death. The PASS marker rides
    # the grid dump.
    TOTAL=$((TOTAL + 1))
    set +e
    out=$(printf '' | MSYS2_ARG_CONV_EXCL='/progs' timeout 60 "$MAIZE" --console-dump \
        --no-root --mount "${pnat}=/progs:ro" "$quesos" /progs/raw_reap_restore.mzx 2>/dev/null)
    set -e
    if printf '%s\n' "$out" | grep -qF "raw-reap-restore: PASS"; then
        echo "[PASS] quesos94_raw_reap_restore"
    else
        echo "[FAIL] quesos94_raw_reap_restore"
        printf '%s\n' "$out" | sed 's/^/          | /'
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi

    # Phase (b) libc: real environ/getenv/setenv (crt0 capture), getcwd, fork+waitpid+
    # WEXITSTATUS, pipe across fork, and heap (do_brk under paging -- malloc works under
    # quesOS). Standalone /progs run.
    TOTAL=$((TOTAL + 1))
    set +e
    out=$(MSYS2_ARG_CONV_EXCL='/progs' timeout 60 "$MAIZE" --no-root \
        --mount "${pnat}=/progs:ro" "$quesos" /progs/libc_proc.mzx 2>/dev/null | grep -v '^$')
    set -e
    if printf '%s\n' "$out" | grep -qF "libc-proc: PASS"; then
        echo "[PASS] quesos94_libc_proc"
    else
        echo "[FAIL] quesos94_libc_proc"
        printf '%s\n' "$out" | sed 's/^/          | /'
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi

    # setjmp/longjmp/sigsetjmp/siglongjmp (OQ 9082 ruling: minimal setjmp in-card,
    # toolchain/rt/setjmp.mazm, the oksh error-unwind enabler). The fixture proves the
    # 0-then-value return across a real call chain, local survival, and the sigsetjmp(.,1)
    # signal-mask save/restore over SYS $0E. Standalone /progs run.
    TOTAL=$((TOTAL + 1))
    set +e
    out=$(MSYS2_ARG_CONV_EXCL='/progs' timeout 60 "$MAIZE" --no-root \
        --mount "${pnat}=/progs:ro" "$quesos" /progs/setjmp_launch.mzx 2>/dev/null | grep -v '^$')
    set -e
    if printf '%s\n' "$out" | grep -qF "setjmp-launch: PASS"; then
        echo "[PASS] quesos94_setjmp"
    else
        echo "[FAIL] quesos94_setjmp"
        printf '%s\n' "$out" | sed 's/^/          | /'
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi

    # execvp PATH search (decision 8939): the launcher forks, the child sets PATH=/bin and
    # execvp's a bare command name; execvp walks PATH and execve's /bin/bin_echoer.mzx. The
    # binaries are mounted at BOTH /progs (worklist) and /bin (PATH).
    TOTAL=$((TOTAL + 1))
    set +e
    out=$(MSYS2_ARG_CONV_EXCL='/progs;/bin' timeout 60 "$MAIZE" --no-root \
        --mount "${pnat}=/progs:ro" --mount "${bnat}=/bin:ro" \
        "$quesos" /progs/execvp_run.mzx 2>/dev/null | grep -v '^$')
    set -e
    if printf '%s\n' "$out" | grep -qF "execvp: PASS"; then
        echo "[PASS] quesos94_execvp"
    else
        echo "[FAIL] quesos94_execvp"
        printf '%s\n' "$out" | sed 's/^/          | /'
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi
}

_mz_want "run_quesos94_fixtures" && mz_timed "run_quesos94_fixtures" run_quesos94_fixtures

# maize-94 wave-1 userland: the VENDORED sbase binaries (userland/oksh + userland/sbase
# submodules, built by mzcc build-userland through the same cross-toolchain pipeline),
# run UNDER quesOS. Distinct from run_quesos94_fixtures above, which proves the kernel/libc
# plumbing with hand-written os/quesos/*.c fixtures; this proves the actual borrowed
# programs. Two acceptance shapes:
#   - AC 8935 (standalone): each no-arg wave-1 util (true/false/pwd) runs as a bare quesOS
#     worklist entry, with true -> reaped status 0, false -> status 1, pwd -> prints the
#     process cwd ("/", the per-PCB default) then status 0.
#   - AC 8931 (pipeline substrate): a real two-stage pipeline of vendored binaries,
#     `echo payload | cat`, driven by os/quesos/sbase_launch.c through fork+pipe+dup2+
#     execve+wait4, with the /bin set mounted at both /progs (worklist) and /bin (PATH).
# The arg-taking utils (echo, cat) are exercised via the launcher fixture per decision 9078
# (quesOS worklist entries take no args). The userland build needs cp/find (and, once the
# oksh overlay lands, patch); the Windows MSYS leg installs patch+diffutils via ci.yml.
run_userland94_fixtures() {
    builder="$MZCC"                              # maize-382: mzcc build-quesos, not the .sh
    ubuild="$MZCC"                               # maize-382: mzcc build-userland, not the .sh
    progs="${WORK_DIR}/ul94-progs"
    bindir="${WORK_DIR}/ul94-bin"
    rwdir="${WORK_DIR}/ul94-rw"
    quesos="${WORK_DIR}/ul94-quesos.mzx"
    log="${WORK_DIR}/ul94.log"
    rm -rf "$progs" "$bindir" "$rwdir"; mkdir -p "$progs" "$bindir" "$rwdir"

    # The userland build stages a scratch checkout with cp -a + find; skip loudly on a host
    # lacking them rather than reporting a spurious failure (never silently pass, though:
    # a SKIP is visible in the CI log).
    if ! command -v find >/dev/null 2>&1 || ! command -v cp >/dev/null 2>&1; then
        echo "[SKIP] userland94: cp/find unavailable (cannot stage the sbase scratch tree)"
        return
    fi

    if ! "$builder" build-quesos --preset "$PRESET" -o "$quesos" >"$log" 2>&1 || [ ! -f "$quesos" ]; then
        echo "[FAIL] userland94: quesOS link failed"; cat "$log" >&2
        TOTAL=$((TOTAL + 1)); FAIL_COUNT=$((FAIL_COUNT + 1)); return
    fi
    # maize-304: this function's heavy compile burst (the wave-1 sbase+oksh build via
    # $ubuild below, plus the bin_echoer/kilo/*_launch compiles that follow) was gated
    # by an earlier maize-304 cycle the same way as run_quesos_ac_fixtures's loop. The
    # operator dropped that gate (comment #3133) rather than land a cross-process
    # lock. There is no cross-process lock here: heavy Test-stage runs are expected to
    # be run one at a time by orchestration discipline, and the timeout wraps below
    # (cc_maize_compile_bounded and the direct $ubuild timeout) bound the damage if two
    # ever overlap anyway.
    # Build the shipped wave-1 /bin set through the userland harness (the vendored sbase
    # plus the oksh shell). oksh is the wave's central deliverable (ACs 8929-8934).
    # maize-304: bounded by `timeout` (mzcc build-userland fork-execs cproc-qbe/qbe/mazm/mzld
    # per translation unit, the same fork-dense pipeline the quesOS loops use, over 11 tools).
    # `-k GRACE` (cycle-2 review fix, see cc_maize_compile_bounded's comment above):
    # a plain `timeout` only sends SIGTERM and waits, which a compile wedged in the
    # actual dofork resource-exhaustion condition may never honor; SIGKILL after the
    # grace window is the real backstop.
    #
    # Test-stage finding (comment #3142, cycle 4): this timeout bounds a whole-BATCH
    # build (every wave-1 tool compiled in one $ubuild call), not a single compile, so
    # its budget has to be sized for the batch, not for one tool. A flat 300s default
    # is a per-tool-ish number; on this host a legitimate cold, uncached build (no
    # object cache; cc-maize.sh recompiles the RT from scratch per tool) costs
    # ~80-100s PER TOOL, so 300s fired at rc=124 after only 3/11 tools on a healthy,
    # non-hung build, a spurious new failure. The fix: scale the budget to the actual
    # tool count (so it stays correct as wave-3 grows the set) with a generous
    # per-tool allowance and a floor, rather than a fixed number sized for today's
    # count. This is a stall-catcher for indefinite fork-resource exhaustion, not a
    # compile-speed gate, so err generous.
    UBUILD_TOOLS="true false echo cat pwd printf cp mv rm ls oksh"
    _ubuild_tool_count=$(set -- $UBUILD_TOOLS; echo $#)
    UBUILD_PER_TOOL_TIMEOUT="${MAIZE_UBUILD_PER_TOOL_TIMEOUT:-180}"
    UBUILD_TIMEOUT_FLOOR="${MAIZE_UBUILD_TIMEOUT_FLOOR:-600}"
    _ubuild_scaled=$((_ubuild_tool_count * UBUILD_PER_TOOL_TIMEOUT))
    if [ "$_ubuild_scaled" -lt "$UBUILD_TIMEOUT_FLOOR" ]; then
        _ubuild_scaled="$UBUILD_TIMEOUT_FLOOR"
    fi
    UBUILD_TIMEOUT="${MAIZE_UBUILD_TIMEOUT:-$_ubuild_scaled}"
    UBUILD_KILL_GRACE="${MAIZE_UBUILD_KILL_GRACE:-10}"
    set +e
    # maize-382 (decision D17): $UBUILD_TOOLS still word-splits into 11 explicit
    # program names, which mzcc build-userland's trailing positional scan
    # (src/mzcc_userland.c:345-373) collects exactly the way build-userland.sh's own
    # parser did, so this fixture keeps building 11 tools and never the 43-tool
    # default. `timeout` wraps a native binary the same way it wrapped the script.
    timeout -k "$UBUILD_KILL_GRACE" "$UBUILD_TIMEOUT" "$ubuild" build-userland --preset "$PRESET" \
            --out "$bindir" $UBUILD_TOOLS >>"$log" 2>&1
    _ubuild_rc=$?
    set -e
    if [ "$_ubuild_rc" -ne 0 ]; then
        # 124: timeout's normal "I killed it with TERM" code. 137 (128+9): the -k
        # grace's SIGKILL actually fired because the child ignored TERM (this is
        # `timeout`'s own documented exit-status contract, confirmed directly against
        # a SIGTERM-ignoring stub); both mean the same thing here, a timed-out build.
        if [ "$_ubuild_rc" -eq 124 ] || [ "$_ubuild_rc" -eq 137 ]; then
            echo "[FAIL] userland94: mzcc build-userland timed out after ${UBUILD_TIMEOUT}s (possible fork-resource exhaustion)"
        elif grep -qiE 'dofork.*Resource temporarily unavailable' "$log" 2>/dev/null; then
            echo "[FAIL] userland94: mzcc build-userland failed (fork-resource exhaustion: dofork Resource temporarily unavailable)"
        else
            echo "[FAIL] userland94: mzcc build-userland failed to build the wave-1 sbase + oksh set"
        fi
        cat "$log" >&2
        TOTAL=$((TOTAL + 1)); FAIL_COUNT=$((FAIL_COUNT + 1))
        return
    fi
    # bin_echoer is the execvp-fallback resolution target (decision 9084), placed in /bin.
    if ! cc_maize_compile_bounded "userland94: bin_echoer.c" "${bindir}/bin_echoer.mzx" \
            "${REPO_ROOT}/os/quesos/bin_echoer.c" "$log"; then
        TOTAL=$((TOTAL + 1)); FAIL_COUNT=$((FAIL_COUNT + 1))
        return
    fi
    # maize-250: kilo (the full-screen editor) built into /bin so the pty fixture can launch
    # it as a child of oksh under quesOS. Compiles clean through cc-maize with no source
    # changes (demos/kilo/README.md).
    if ! cc_maize_compile_bounded "userland94: kilo.c" "${bindir}/kilo.mzx" \
            "${REPO_ROOT}/demos/kilo/kilo.c" "$log"; then
        TOTAL=$((TOTAL + 1)); FAIL_COUNT=$((FAIL_COUNT + 1))
        return
    fi
    # The launcher drivers are quesOS worklist entries (compiled like any fixture):
    # sbase_launch (echo|cat pipeline), printf_launch, and the cp/mv/rm fs launchers
    # (each seeds a file on the /rw mount, execve's its util, and verifies the result).
    for _drv in sbase_launch printf_launch cp_launch mv_launch rm_launch ls_launch \
                oksh_shell execvp_ext oksh_interactive; do
        if ! cc_maize_compile_bounded "userland94: ${_drv}.c" "${progs}/${_drv}.mzx" \
                "${REPO_ROOT}/os/quesos/${_drv}.c" "$log"; then
            TOTAL=$((TOTAL + 1)); FAIL_COUNT=$((FAIL_COUNT + 1))
            return
        fi
    done
    # The no-arg utils run as direct worklist entries: stage them under /progs too.
    cp "${bindir}/true.mzx" "${bindir}/false.mzx" "${bindir}/pwd.mzx" "$progs/"
    pnat=$(host_to_native "$progs")
    bnat=$(host_to_native "$bindir")
    rnat=$(host_to_native "$rwdir")

    # Helper: run one worklist program under quesOS with /progs + /bin (ro) and a
    # writable /rw scratch mounted (the cp/mv/rm launchers seed + verify files there).
    # MSYS2_ARG_CONV_EXCL is SEMICOLON-separated (a colon value is one literal prefix that
    # matches nothing, so the /progs/<x>.mzx worklist arg got rewritten to a Windows path
    # and quesOS could not load it on the Windows leg).
    # maize-379: maize-372 gated the "[quesos] reaped <path> status=N" line behind the
    # QUESOS_VERBOSE boot-env key, and three fixtures driven through this helper
    # (userland94_true, userland94_false, userland94_pwd) assert on that line, so opt
    # into verbose boot here. quesOS strips QUESOS_VERBOSE from the captured launcher
    # environment before spawning anything, so it never reaches a program's envp. Every
    # other fixture on this helper asserts a positive self-check marker only, so the
    # restored trace lines cannot perturb them.
    ul94_run() {
        MSYS2_ARG_CONV_EXCL='/progs;/bin;/rw' timeout 90 "$MAIZE" --no-root \
            --mount "${pnat}=/progs:ro" --mount "${bnat}=/bin:ro" \
            --mount "${rnat}=/rw:rw" \
            -e QUESOS_VERBOSE=1 "$quesos" "$1" 2>/dev/null
    }

    # AC 8931 substrate: the vendored echo|cat pipeline.
    TOTAL=$((TOTAL + 1))
    set +e; out=$(ul94_run /progs/sbase_launch.mzx); set -e
    if printf '%s\n' "$out" | grep -qF "sbase-launch: PASS"; then
        echo "[PASS] userland94_pipeline (vendored echo | cat)"
    else
        echo "[FAIL] userland94_pipeline"; printf '%s\n' "$out" | sed 's/^/          | /'
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi

    # AC 8935 standalone (arg-taking): printf, exercised via its launcher fixture
    # (decision 9078). `printf 'x=%s:%d\n' hi 42` must emit "x=hi:42\n" (literal +
    # %s + %d integer parse + \n unescape), driven through fork+execve+pipe+wait4.
    TOTAL=$((TOTAL + 1))
    set +e; out=$(ul94_run /progs/printf_launch.mzx); set -e
    if printf '%s\n' "$out" | grep -qF "printf-launch: PASS"; then
        echo "[PASS] userland94_printf (vendored printf %s/%d)"
    else
        echo "[FAIL] userland94_printf"; printf '%s\n' "$out" | sed 's/^/          | /'
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi

    # AC 8935 standalone (arg-taking, filesystem): cp / mv / rm, each via its launcher
    # fixture (decision 9078) against the writable /rw mount. cp copies a seeded file
    # and the content is verified; mv renames it (dst present, src gone); rm unlinks it
    # (target gone). Each drives fork+execve+wait4 plus real hostfs open/creat/read/
    # write/unlink/rename through the quesOS dispatcher.
    ul94_fslaunch() {
        _name="$1"; _prog="$2"; _needle="$3"
        TOTAL=$((TOTAL + 1))
        set +e; _out=$(ul94_run "$_prog"); set -e
        if printf '%s\n' "$_out" | grep -qF "$_needle"; then
            echo "[PASS] userland94_${_name}"
        else
            echo "[FAIL] userland94_${_name}"
            echo "        expected marker: \"${_needle}\""
            printf '%s\n' "$_out" | sed 's/^/          | /'
            FAIL_COUNT=$((FAIL_COUNT + 1))
        fi
    }
    ul94_fslaunch cp /progs/cp_launch.mzx "cp-launch: PASS"
    ul94_fslaunch mv /progs/mv_launch.mzx "mv-launch: PASS"
    ul94_fslaunch rm /progs/rm_launch.mzx "rm-launch: PASS"
    ul94_fslaunch ls /progs/ls_launch.mzx "ls-launch: PASS"

    # AC 8935 standalone: true (status 0), false (status 1), pwd (prints "/" then status 0).
    ul94_standalone() {
        _name="$1"; _prog="$2"; _needle="$3"
        TOTAL=$((TOTAL + 1))
        set +e; _out=$(ul94_run "$_prog"); set -e
        if printf '%s\n' "$_out" | grep -qF "$_needle"; then
            echo "[PASS] userland94_${_name}"
        else
            echo "[FAIL] userland94_${_name}"
            echo "        expected marker: \"${_needle}\""
            printf '%s\n' "$_out" | sed 's/^/          | /'
            FAIL_COUNT=$((FAIL_COUNT + 1))
        fi
    }
    ul94_standalone true  /progs/true.mzx  "reaped /progs/true.mzx status=0"
    ul94_standalone false /progs/false.mzx "reaped /progs/false.mzx status=1"

    # ACs 8930-8934 (the shell story, FROM oksh): oksh_shell forks `oksh -c <script>`
    # non-interactively with PATH=/bin and drives, through oksh's own fork/pipe/dup2/
    # execve/wait4, a two-stage vendored pipeline (echo.mzx | cat), > / >> redirection
    # into /rw read back with cat, $? after false.mzx (1) and true.mzx (0), cd + pwd,
    # export made visible to a child's getenv (nested oksh -c), and the shell's own
    # `exit 7` observed via WEXITSTATUS. Extensionless `cat` / `oksh` also exercise the
    # decision-9084 name fallback in oksh's command lookup. Self-checked PASS marker.
    TOTAL=$((TOTAL + 1))
    set +e; out=$(ul94_run /progs/oksh_shell.mzx); set -e
    if printf '%s\n' "$out" | grep -qF "oksh-shell: PASS"; then
        echo "[PASS] userland94_oksh_shell (pipeline/redirect/exit-status/builtins)"
    else
        echo "[FAIL] userland94_oksh_shell"; printf '%s\n' "$out" | sed 's/^/          | /'
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi

    # AC 8930 INTERACTIVE path (operator reopen): oksh_interactive forks `oksh -i` (forces
    # interactive init even without a controlling tty), so it runs the line-editor startup
    # that queries the terminal size via $F6 sys_ttysize and opens /dev/tty. Before $F6 was
    # forwarded this stranded the shell on an unhandled syscall with no prompt; now quesOS
    # forwards it (a pipe fd returns -ENOTTY, so oksh degrades to its default size), the
    # shell emits its prompt and executes the piped command. Asserts BOTH a prompt marker
    # and the pwd output are present.
    TOTAL=$((TOTAL + 1))
    set +e; out=$(ul94_run /progs/oksh_interactive.mzx); set -e
    if printf '%s\n' "$out" | grep -qF "oksh-interactive: PASS"; then
        echo "[PASS] userland94_oksh_interactive (interactive prompt + ttysize + command)"
    else
        echo "[FAIL] userland94_oksh_interactive"; printf '%s\n' "$out" | sed 's/^/          | /'
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi

    # AC 8930 REAL-KEYSTROKE acceptance (operator reopen #2): the one that PRESSES KEYS.
    # pty_oksh_check.py forks `maize --rom <quesos> /bin/oksh.mzx` (the DEFAULT input path,
    # no --input flag) into a pseudo terminal, waits for the prompt, types "pwd" + an echo +
    # "exit" as keystrokes, and asserts the shell echoed and executed them. This is the
    # acceptance bar the piped/-c fixtures missed twice: with the default-path console input
    # fixed (demand-driven con_data read) an interactive shell now works from a real
    # terminal. Real-pty variant only (CI-safe, stdlib pty); the Windows ConPTY equivalent is
    # operator/local. Skips loudly where python3's pty module does not actually work
    # (python3_has_pty, maize-257: a native Windows python3 exists but its pty module raises
    # ModuleNotFoundError on import, since it has no termios).
    if python3_has_pty; then
        TOTAL=$((TOTAL + 1))
        set +e
        out=$(python3 "${REPO_ROOT}/scripts/pty_oksh_check.py" \
            "$DEFAULT_MAIZE" "$quesos" "$bindir" "$rwdir" 2>&1)
        set -e
        if printf '%s\n' "$out" | grep -qF "pty-oksh: PASS"; then
            echo "[PASS] userland94_oksh_keystrokes (real pty, default input path)"
        else
            echo "[FAIL] userland94_oksh_keystrokes"; printf '%s\n' "$out" | sed 's/^/          | /'
            FAIL_COUNT=$((FAIL_COUNT + 1))
        fi
    else
        echo "[SKIP] userland94_oksh_keystrokes (no working python3 pty/termios on this host; real-pty leg is Linux/macOS/POSIX-python only)"
    fi

    # maize-250 (AC 9109/9110/9111/9112): full-screen kilo as a quesOS CHILD of oksh through a
    # real pty on the console binary. pty_oksh_kilo_check.py boots oksh, launches
    # `kilo /rw/t.txt`, asserts a clean alt-screen paint (enter + home + frame-end, no
    # unhandled syscall), types text, saves (Ctrl-S), quits (Ctrl-Q), asserts the oksh prompt
    # returns and a follow-on pwd round-trips; the harness then reads /rw/t.txt back from the
    # host and asserts the exact typed content. A second (kill) mode SIGTERMs the maize
    # process mid-edit and asserts a non-hang reap (the in-VM killed-TUI console restore is
    # proven deterministically by quesos94_raw_reap_restore, not through the pty). Real-pty
    # variant only, like userland94_oksh_keystrokes; skips loudly where python3's pty module
    # does not actually work (python3_has_pty, maize-257).
    if python3_has_pty; then
        rm -f "${rwdir}/t.txt"
        TOTAL=$((TOTAL + 1))
        set +e
        out=$(python3 "${REPO_ROOT}/scripts/pty_oksh_kilo_check.py" \
            "$DEFAULT_MAIZE" "$quesos" "$bindir" "$rwdir" edit 2>&1)
        rc=$?
        set -e
        saved=""
        [ -f "${rwdir}/t.txt" ] && saved=$(cat "${rwdir}/t.txt" 2>/dev/null)
        if [ "$rc" -eq 0 ] && printf '%s\n' "$out" | grep -qF "pty-kilo: PASS" \
        && [ "$saved" = "hello from kilo as a quesos child" ]; then
            echo "[PASS] userland94_kilo_edit (paint + type + save + quit + prompt-return)"
        else
            echo "[FAIL] userland94_kilo_edit (saved=\"${saved}\")"
            printf '%s\n' "$out" | sed 's/^/          | /'
            FAIL_COUNT=$((FAIL_COUNT + 1))
        fi

        TOTAL=$((TOTAL + 1))
        set +e
        out=$(python3 "${REPO_ROOT}/scripts/pty_oksh_kilo_check.py" \
            "$DEFAULT_MAIZE" "$quesos" "$bindir" "$rwdir" kill 2>&1)
        rc=$?
        set -e
        if [ "$rc" -eq 0 ] && printf '%s\n' "$out" | grep -qF "pty-kilo: PASS"; then
            echo "[PASS] userland94_kilo_kill (raw-child pty launch reaps without hanging)"
        else
            echo "[FAIL] userland94_kilo_kill"
            printf '%s\n' "$out" | sed 's/^/          | /'
            FAIL_COUNT=$((FAIL_COUNT + 1))
        fi

        # maize-350 (AC4/AC5): largefile mode synthesizes a ~380 KB highlighted file into the
        # /rw mount, launches `kilo /rw/big.c`, asserts a clean paint, byte-compares an
        # open/edit/save round-trip, and measures the wall-clock load-and-first-paint time. The
        # LOAD_MS number is recorded (echoed into the PASS line here and captured for the AC5
        # before/after comparison), NOT gated on a hardcoded threshold (decision 9960), since
        # cross-machine timing variance makes a fixed CI percentage unreliable.
        rm -f "${rwdir}/big.c"
        TOTAL=$((TOTAL + 1))
        set +e
        out=$(python3 "${REPO_ROOT}/scripts/pty_oksh_kilo_check.py" \
            "$DEFAULT_MAIZE" "$quesos" "$bindir" "$rwdir" largefile 2>&1)
        rc=$?
        set -e
        if [ "$rc" -eq 0 ] && printf '%s\n' "$out" | grep -qF "pty-kilo: PASS"; then
            lm=$(printf '%s\n' "$out" | grep -oE 'LOAD_MS [0-9]+' | awk '{print $2}')
            echo "[PASS] userland94_kilo_largefile (380KB highlighted load + edit/save round-trip; LOAD_MS=${lm})"
        else
            echo "[FAIL] userland94_kilo_largefile"
            printf '%s\n' "$out" | sed 's/^/          | /'
            FAIL_COUNT=$((FAIL_COUNT + 1))
        fi
    else
        echo "[SKIP] userland94_kilo (no working python3 pty/termios on this host; real-pty leg is Linux/macOS/POSIX-python only)"
    fi

    # AC 8930 EOF (operator reopen #2, cycle 2): a piped default-path shell with NO explicit
    # `exit` must TERMINATE when host stdin drains, not livelock on a synthesized NUL. Feed
    # just "pwd\n" (no exit) and assert oksh ran pwd (prints "/") and reaped clean; `timeout`
    # fails a regression of the on-demand EOF path loudly instead of hanging the suite. Plain
    # pipe (no pty), so it runs on every leg.
    # maize-379: the reap assertion below reads the "[quesos] reaped" line that maize-372
    # gated behind QUESOS_VERBOSE, so opt into verbose boot explicitly with
    # -e QUESOS_VERBOSE=1. The assertion itself is what proves the shell actually reaped,
    # so it stays.
    TOTAL=$((TOTAL + 1))
    set +e
    out=$(printf 'pwd\n' | MSYS2_ARG_CONV_EXCL='/bin;/rw' timeout 60 "$MAIZE" --no-root \
        --mount "${bnat}=/bin:ro" --mount "${rnat}=/rw:rw" \
        -e QUESOS_VERBOSE=1 "$quesos" /bin/oksh.mzx 2>/dev/null | grep -v '^$')
    set -e
    if printf '%s\n' "$out" | grep -qxF "/" \
    && printf '%s\n' "$out" | grep -qF "reaped /bin/oksh.mzx status=0"; then
        echo "[PASS] userland94_oksh_eof (piped, no exit, terminates on stdin EOF)"
    else
        echo "[FAIL] userland94_oksh_eof"; printf '%s\n' "$out" | sed 's/^/          | /'
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi

    # Decision 9084: libc execvp's exact -> .mzx -> .mzb name fallback. execvp_ext resolves
    # the BARE name "bin_echoer" to /bin/bin_echoer.mzx (positive) and confirms a name with
    # no existing form returns ENOENT so the child reaches its own _exit (negative), which
    # also exercises quesOS execve returning -ENOENT instead of destroying the caller.
    TOTAL=$((TOTAL + 1))
    set +e; out=$(ul94_run /progs/execvp_ext.mzx); set -e
    if printf '%s\n' "$out" | grep -qF "execvp-ext: PASS"; then
        echo "[PASS] userland94_execvp_ext (name .mzx/.mzb fallback + ENOENT)"
    else
        echo "[FAIL] userland94_execvp_ext"; printf '%s\n' "$out" | sed 's/^/          | /'
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi
    # pwd: the "/" line proves getcwd returned the per-PCB default; status 0 proves it exited
    # clean. Grep both, so a util that printed nothing but exited 0 cannot pass.
    TOTAL=$((TOTAL + 1))
    set +e; out=$(ul94_run /progs/pwd.mzx); set -e
    if printf '%s\n' "$out" | grep -qxF "/" \
    && printf '%s\n' "$out" | grep -qF "reaped /progs/pwd.mzx status=0"; then
        echo "[PASS] userland94_pwd (cwd \"/\")"
    else
        echo "[FAIL] userland94_pwd"; printf '%s\n' "$out" | sed 's/^/          | /'
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi

    # maize-352: headless --console-dump input injection INTO a quesOS world. The pty fixtures
    # above (userland94_oksh_keystrokes / userland94_kilo_*) drive keystrokes through a real
    # pseudo terminal on the DEFAULT input path; those are POSIX-python-only and SKIP on Windows
    # and on any host whose python3 lacks a working pty/termios. This pair drives input through
    # the headless, deterministic --console-dump channel with --input=console instead, so it runs
    # on EVERY leg (a plain pipe, no pty, no python).
    #
    # WHY --input=console (and why no VM code change): quesOS reads its console through port I/O
    # (IN $00/$01 -> devices::console_device), parking a process on BLK_CONSOLE and relying on
    # IRQ 33 to complete a read that must genuinely block. IRQ 33 is raised only by
    # console_device::on_input_tick, which runs only while console_device is the CPU's active
    # input. --console-dump ALONE binds only text_console (the bare-guest Set-1-scancode SYS $00
    # path quesOS never uses), so a quesOS process that parks waiting for a not-yet-arrived byte
    # never wakes. --input=console wires console_device as the active input (src/maize.cpp
    # input_source=="console" branch, already unconditional with respect to --console-dump), so
    # on_input_tick raises IRQ 33 on the rising edge of readiness and the parked reader wakes.
    # Verified on maize-352: with a byte fed LATE, --console-dump alone hangs (the reader parks
    # and never wakes) while --console-dump --input=console exits cleanly, and the composition
    # needs zero VM code change.
    #
    # DETERMINISM (AC 9983, decision 9975): the whole injected payload is resident in the host
    # pipe from process start (a single printf), so every console_read() status check finds its
    # byte already pending and the fixture NEVER sleeps. The pass signal is the VM's own clean
    # exit (the injected `exit` ends the top-level oksh session so quesOS shuts the VM down and
    # emits the grid dump, which sys::exit() produces only at exit) plus the dumped grid content;
    # the `timeout` is a hang backstop, never the thing that produces the output.
    #
    # INPUT CONTRACT (maize-352, OQ 9979): --console-dump --input=console feeds RAW already-
    # decoded bytes on stdin (literal ASCII / control codes: \r for Enter, octal \021 = 0x11 for
    # Ctrl-Q), because console_device's data port is a raw read() passthrough. This is a SEPARATE,
    # ADDITIVE contract from the --console-dump-ALONE bare-guest path (run_console_selfcheck),
    # which feeds Set-1 scancodes decoded through text_console. Both are correct for their own
    # device; do not confuse them.
    cdump_inject_run() {
        # Reads the raw-byte payload from stdin, prints the exit-time grid dump. $MAIZE is the
        # --bare wrapper (maize-360), so this boots quesOS as the loaded image and runs
        # /bin/oksh.mzx as its worklist init. MSYS2_ARG_CONV_EXCL keeps the /bin;/rw mount-arg
        # paths from being rewritten on the Windows leg.
        # maize-379: both fixtures on this helper assert the "[quesos] reaped" line that
        # maize-372 gated behind QUESOS_VERBOSE, so opt into verbose boot with
        # -e QUESOS_VERBOSE=1. That also re-arms their negative "unhandled syscall" guard,
        # which the same gate had silenced into a check that could never fire.
        MSYS2_ARG_CONV_EXCL='/bin;/rw' timeout 60 "$MAIZE" --no-root \
            --console-dump --input=console \
            --mount "${bnat}=/bin:ro" --mount "${rnat}=/rw:rw" \
            -e QUESOS_VERBOSE=1 "$quesos" /bin/oksh.mzx 2>/dev/null
    }

    # AC 9980 (mandatory): inject `ls` then `exit` as RAW bytes at the oksh prompt. oksh's
    # per-PCB cwd is "/", where the two mounts appear, so a bare `ls` lists `bin` and `rw`; a
    # follow-on `ls /rw` lists a file seeded into the writable mount, giving a distinctive marker
    # no other grid content can false-positive on. Assert the grid shows ls's output AND a clean
    # shell exit (reaped status=0) AND no unhandled-syscall / halt diagnostic.
    # rm -rf, not rm -f: an earlier fixture (ls_launch) leaves the /rw/lsd DIRECTORY
    # behind, and a non-recursive rm -f cannot remove a directory ("Is a directory"),
    # which under `set -e` would abort the whole run-ctest.sh tail before this fixture.
    rm -rf "${rwdir}"/*
    echo "console-dump-inject marker" > "${rwdir}/injected.txt"
    TOTAL=$((TOTAL + 1))
    set +e; out=$(printf 'ls\rls /rw\rexit\r' | cdump_inject_run); set -e
    if printf '%s\n' "$out" | grep -qx "bin" \
    && printf '%s\n' "$out" | grep -qx "rw" \
    && printf '%s\n' "$out" | grep -qx "injected.txt" \
    && printf '%s\n' "$out" | grep -qF "reaped /bin/oksh.mzx status=0" \
    && ! printf '%s\n' "$out" | grep -qiE "unhandled syscall|halt"; then
        echo "[PASS] userland94_console_dump_inject_oksh (ls output + clean exit via injected raw bytes)"
    else
        echo "[FAIL] userland94_console_dump_inject_oksh"; printf '%s\n' "$out" | sed 's/^/          | /'
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi

    # AC 9981: launch kilo on a fresh /rw file through the same channel, inject a lone Ctrl-Q
    # (octal \021 = 0x11) after the launch line, then `exit`. The buffer is clean, so Ctrl-Q
    # quits kilo immediately (no dirty-buffer confirm), control returns to oksh, and the session
    # exits. ASSERTION NOTE (maize-352): --console-dump dumps the RENDERED terminal GRID
    # (text_console::dump_text emits printable glyphs; control sequences are already interpreted),
    # NOT the raw VT byte stream, so kilo's alt-screen-exit sequence \x1b[?1049l cannot appear as
    # literal text here (that raw-sequence assertion lives in the pty fixture
    # pty_oksh_kilo_check.py). The equivalent, observable proof on THIS channel is: kilo painted
    # (its HELP status line and the /rw/t.txt status bar), THEN the oksh prompt returned and the
    # shell exited cleanly (reaped status=0), i.e. Ctrl-Q quit kilo and left no stuck editor or
    # hang. text_console does not model the 1049 alt-screen, so kilo's last paint stays resident
    # in the lower grid while the returned oksh prompt overwrites the top rows.
    # rm -rf (not rm -f): clears the /rw/lsd DIRECTORY left by the earlier ls_launch
    # fixture too, so the cleanup cannot fail under `set -e` (see the oksh case above).
    rm -rf "${rwdir}"/*
    TOTAL=$((TOTAL + 1))
    set +e; out=$(printf 'kilo /rw/t.txt\r\021exit\r' | cdump_inject_run); set -e
    if printf '%s\n' "$out" | grep -qF "HELP: Ctrl-S = save" \
    && printf '%s\n' "$out" | grep -qF "/rw/t.txt" \
    && printf '%s\n' "$out" | grep -qF "reaped /bin/oksh.mzx status=0" \
    && ! printf '%s\n' "$out" | grep -qiE "unhandled syscall|halt"; then
        echo "[PASS] userland94_console_dump_inject_kilo (kilo paint + Ctrl-Q quit + prompt return)"
    else
        echo "[FAIL] userland94_console_dump_inject_kilo"; printf '%s\n' "$out" | sed 's/^/          | /'
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi

    # --- maize-361: controlling terminal (/dev/tty) + job control --------------------
    # Five fixtures over the same headless --console-dump --input=console channel the two
    # above use, so they run on every leg (no pty, no python). Each one was proven to FAIL
    # against the parent commit before the fix landed; the transcripts are on the card.
    #
    # Two differences from cdump_inject_run: a taller grid (the job-control transcripts run
    # past 25 rows, and the grid dump shows only what is still ON the grid at exit), and
    # -e QUESOS_VERBOSE=1 so the maize-372-gated "reaped ... status=0" line is available as
    # the clean-shutdown assertion. Ctrl-Z is the raw byte 0x1A (octal \032) and Ctrl-C is
    # 0x03 (octal \003) on this channel: --input=console feeds already-decoded bytes, NOT
    # Set-1 scancodes (that is the --console-dump-ALONE contract, a different device).
    #
    # cat is the foreground job in every case because the console byte that raises a signal
    # is recognized where it is CONSUMED (console_read / the console IRQ), so the job has to
    # be one that actually reads stdin. A job that never reads (sleep 100) would not observe
    # Ctrl-Z here any more than it observes Ctrl-C today.
    jc_run() {
        MSYS2_ARG_CONV_EXCL='/bin;/rw' timeout 60 "$MAIZE" --no-root \
            --console-dump --console-size 100x45 --input=console -e QUESOS_VERBOSE=1 \
            --mount "${bnat}=/bin:ro" --mount "${rnat}=/rw:rw" \
            "$quesos" /bin/oksh.mzx 2>/dev/null
    }

    # AC 10050/10051 (Tier 1): with /dev/tty resolving to the console, an interactive oksh
    # prints NEITHER startup warning. Parent commit: both present.
    TOTAL=$((TOTAL + 1))
    set +e; out=$(printf 'exit\r' | jc_run); set -e
    if ! printf '%s\n' "$out" | grep -qF "No controlling tty" \
    && ! printf '%s\n' "$out" | grep -qF "won't have full job control" \
    && printf '%s\n' "$out" | grep -qF "reaped /bin/oksh.mzx status=0"; then
        echo "[PASS] userland361_devtty_warnings (no ctty / no job-control warning)"
    else
        echo "[FAIL] userland361_devtty_warnings"; printf '%s\n' "$out" | sed 's/^/          | /'
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi

    # AC 10143/10155: Ctrl-Z stops a foreground cat (job reported Stopped, prompt returns),
    # jobs lists it, fg resumes it, and the resumed read completes with a REAL byte (cat
    # echoes "resumed"), not an EINTR error. This is the fixture that isolates the
    # BLK_CONSOLE completion in raise_on_pcb: with points 1-5 only, the SIGTSTP bit sits on
    # the parked cat forever, no Stopped line is ever printed and the prompt never returns.
    # Parent commit: 0x1A is delivered to cat as an ordinary byte and nothing is stopped.
    TOTAL=$((TOTAL + 1))
    set +e; out=$(printf 'cat\r\032jobs\rfg\rresumed\n' | jc_run); set -e
    if printf '%s\n' "$out" | grep -qE "\[1\] \+ Stopped +cat" \
    && printf '%s\n' "$out" | grep -qx "resumed" \
    && printf '%s\n' "$out" | grep -qF "reaped /bin/oksh.mzx status=0" \
    && ! printf '%s\n' "$out" | grep -qiE "unhandled syscall|halt"; then
        echo "[PASS] userland361_ctrlz_fg (Ctrl-Z stops cat, jobs lists it, fg resumes it)"
    else
        echo "[FAIL] userland361_ctrlz_fg"; printf '%s\n' "$out" | sed 's/^/          | /'
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi

    # AC 10156: Ctrl-C on a foreground cat that is idly blocked reading the console actually
    # terminates it (prompt returns, the next command runs), instead of leaving the SIGINT
    # pending forever. Parent commit: no job control means the shell shares cat's process
    # group, so the SIGINT reaches the SHELL, which dies with status 130 and never runs the
    # follow-on echo.
    TOTAL=$((TOTAL + 1))
    set +e; out=$(printf 'cat\r\003echo alive\rexit\r' | jc_run); set -e
    if printf '%s\n' "$out" | grep -qx "alive" \
    && printf '%s\n' "$out" | grep -qF "reaped /bin/oksh.mzx status=0" \
    && ! printf '%s\n' "$out" | grep -qiE "unhandled syscall|halt"; then
        echo "[PASS] userland361_ctrlc_blocked_reader (SIGINT reaches an idle console reader)"
    else
        echo "[FAIL] userland361_ctrlc_blocked_reader"; printf '%s\n' "$out" | sed 's/^/          | /'
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi

    # AC 10144 (first half): a job backgrounded from the outset is stopped automatically the
    # first time it tries to read the console, with no Ctrl-Z involved: do_read's SIGTTIN
    # gate end to end. The second `exit` is what ends a session that still has a stopped job.
    # Parent commit: the background cat just reads, and jobs reports it Running.
    TOTAL=$((TOTAL + 1))
    set +e; out=$(printf 'cat &\rjobs\rjobs\rexit\rexit\r' | jc_run); set -e
    if printf '%s\n' "$out" | grep -qE "Stopped \(tty input\) +cat" \
    && printf '%s\n' "$out" | grep -qF "reaped /bin/oksh.mzx status=0" \
    && ! printf '%s\n' "$out" | grep -qiE "unhandled syscall|halt"; then
        echo "[PASS] userland361_bg_read_sigttin (background cat stops on its first read)"
    else
        echo "[FAIL] userland361_bg_read_sigttin"; printf '%s\n' "$out" | sed 's/^/          | /'
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi

    # AC 10144 (second half): bg resumes a stopped job into the BACKGROUND. The shell prints
    # its "[1] cat" continue report and the prompt comes straight back (the job is not
    # foregrounded), and the following jobs sees it running rather than stopped. Parent
    # commit: there is no stopped job to resume in the first place.
    TOTAL=$((TOTAL + 1))
    set +e; out=$(printf 'cat\r\032bg\rjobs\rexit\rexit\r' | jc_run); set -e
    if printf '%s\n' "$out" | grep -qE "\[1\] \+ Stopped +cat" \
    && printf '%s\n' "$out" | grep -qE "^\[1\] cat" \
    && printf '%s\n' "$out" | grep -qE "Running +cat" \
    && printf '%s\n' "$out" | grep -qF "reaped /bin/oksh.mzx status=0" \
    && ! printf '%s\n' "$out" | grep -qiE "unhandled syscall|halt"; then
        echo "[PASS] userland361_bg_resume (bg resumes the stopped job in the background)"
    else
        echo "[FAIL] userland361_bg_resume"; printf '%s\n' "$out" | sed 's/^/          | /'
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi

    # Cycle-2 review, major finding: a SIGCONT that arrives while a console reader is still
    # in raise_on_pcb's TRANSIENT state (already flipped to P_RUNNABLE with its BLK_CONSOLE
    # park intact, waiting for a pending stop that has not been delivered yet) must put that
    # reader back where it was. Cancelling the pending stop and leaving it runnable resumes
    # its saved read frame with the RV slot never written, and sys_read
    # (toolchain/rt/syscall.mazm) is a bare SYS/RET that normalizes nothing, so cat acts on a
    # stale register: here it takes the value for end-of-input and exits, and `jobs` reports
    # "Done" for a job that was never continued into anything.
    #
    # WHY TWO SIGCONTs RATHER THAN `kill -TSTP %1; kill -CONT %1`, which is how the finding
    # names the race: a shell cannot drive that literal pair into the window. By the time a
    # prompt is available to type at, the target has already had a schedule pass and is
    # P_STOPPED, and a stop signal raised on a P_STOPPED process is a no-op (raise_on_pcb's
    # state gate drops it), so the following SIGCONT is the ordinary resume. The transient
    # state a shell CAN reach is the one the background re-gate creates: continuing a stopped
    # console reader into the background re-raises SIGTTIN, which the BLK_CONSOLE completion
    # turns straight back into the same transient state. A second SIGCONT on the SAME command
    # line then cancels that stop with no schedule pass in between, which is the identical
    # window through the identical door. Both SIGCONTs must be on one line: the shell parks
    # reading the console between prompts, and that park is the schedule pass that settles the
    # target properly.
    #
    # Negative control (transcripts on the card): against the seven-commit branch WITHOUT the
    # re-park guard this reports "[1] + Done  cat" on all five of five runs, and with the
    # guard "[1] + Stopped (tty input)  cat" on all five, so the assertion below discriminates
    # deterministically rather than by timing.
    TOTAL=$((TOTAL + 1))
    set +e; out=$(printf 'cat\r\032kill -CONT %%1; kill -CONT %%1\rjobs\rexit\rexit\r' | jc_run); set -e
    if printf '%s\n' "$out" | grep -qE "\[1\] \+ Stopped +cat" \
    && printf '%s\n' "$out" | grep -qE "Stopped \(tty input\) +cat" \
    && ! printf '%s\n' "$out" | grep -qE "Done +cat" \
    && printf '%s\n' "$out" | grep -qF "reaped /bin/oksh.mzx status=0" \
    && ! printf '%s\n' "$out" | grep -qiE "unhandled syscall|halt"; then
        echo "[PASS] userland361_cont_cancels_pending_stop (SIGCONT re-parks a not-yet-stopped reader)"
    else
        echo "[FAIL] userland361_cont_cancels_pending_stop"; printf '%s\n' "$out" | sed 's/^/          | /'
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi
}

_mz_want "run_userland94_fixtures" && mz_timed "run_userland94_fixtures" run_userland94_fixtures

# maize-292: wave-2 userland (31 additional sbase tools, 12 Group-A + 19 Group-B,
# plus patched kill, on top of wave-1). Mirrors run_userland94_fixtures' shape
# (build the /bin set, compile the launcher fixtures as quesOS worklist entries,
# boot + check). The driver's no-explicit-progs default is the FULL
# union (wave-1 + wave-2 + kill + oksh; build-userland.sh:126 and
# src/mzcc_userland.c:383-394 agree tool-for-tool), so no explicit prog
# list is needed here.
run_userland_wave2_fixtures() {
    builder="$MZCC"                              # maize-382: mzcc build-quesos, not the .sh
    ubuild="$MZCC"                               # maize-382: mzcc build-userland, not the .sh
    progs="${WORK_DIR}/ul292-progs"
    bindir="${WORK_DIR}/ul292-bin"
    rwdir="${WORK_DIR}/ul292-rw"
    tmpdir="${WORK_DIR}/ul292-tmp"
    quesos="${WORK_DIR}/ul292-quesos.mzx"
    log="${WORK_DIR}/ul292.log"
    rm -rf "$progs" "$bindir" "$rwdir" "$tmpdir"; mkdir -p "$progs" "$bindir" "$rwdir" "$tmpdir"

    if ! command -v find >/dev/null 2>&1 || ! command -v cp >/dev/null 2>&1; then
        echo "[SKIP] userland292: cp/find unavailable (cannot stage the sbase scratch tree)"
        return
    fi

    if ! "$builder" build-quesos --preset "$PRESET" -o "$quesos" >"$log" 2>&1 || [ ! -f "$quesos" ]; then
        echo "[FAIL] userland292: quesOS link failed"; cat "$log" >&2
        TOTAL=$((TOTAL + 1)); FAIL_COUNT=$((FAIL_COUNT + 1)); return
    fi
    # No explicit prog list: the driver's default is the full wave-1 + wave-2
    # + kill + oksh union (AC 9691 both-drivers-same-default-set discipline). Both
    # drivers must agree on that union tool-for-tool; maize-382 AC 10252 fixed the one
    # place they had drifted (uname, added to build-userland.sh's SBASE_WAVE2 by
    # maize-374 but not to src/mzcc_userland.c's WAVE2[]).
    if ! "$ubuild" build-userland --preset "$PRESET" --out "$bindir" >>"$log" 2>&1; then
        echo "[FAIL] userland292: mzcc build-userland failed to build the default (wave-1+wave-2+kill+oksh) set"
        cat "$log" >&2
        TOTAL=$((TOTAL + 1)); FAIL_COUNT=$((FAIL_COUNT + 1)); return
    fi
    for _drv in wave2_launch_a wave2_launch_b wave2_launch_c wave2_launch_d kill_launch wave2_stdin_pipe wave2_stdin_reuse; do
        if ! "$CC_MAIZE" --preset "$PRESET" -o "${progs}/${_drv}.mzx" \
                "${REPO_ROOT}/os/quesos/${_drv}.c" >>"$log" 2>&1; then
            echo "[FAIL] userland292: ${_drv}.c compile failed"; cat "$log" >&2
            TOTAL=$((TOTAL + 1)); FAIL_COUNT=$((FAIL_COUNT + 1)); return
        fi
    done
    pnat=$(host_to_native "$progs")
    bnat=$(host_to_native "$bindir")
    rnat=$(host_to_native "$rwdir")
    tnat=$(host_to_native "$tmpdir")

    # /tmp:rw is required for sponge.mzx (sponge.c's mkstemp target is hardcoded to
    # /tmp regardless of its own file argument; --no-root means no path exists at all
    # without an explicit --mount grant).
    ul292_run() {
        MSYS2_ARG_CONV_EXCL='/progs;/bin;/rw;/tmp' timeout 90 "$MAIZE" --no-root \
            --mount "${pnat}=/progs:ro" --mount "${bnat}=/bin:ro" \
            --mount "${rnat}=/rw:rw" --mount "${tnat}=/tmp:rw" \
            "$quesos" "$1" 2>/dev/null
    }

    # AC 9683/9684: every wave-2 tool this card ships (31 of 46 the spec's own
    # triage listed; see build-userland.sh's SBASE_WAVE2 comment for the 15 that
    # this card's own build/run discovered cannot ship, and why) responds to a
    # trivial smoke invocation with the expected exit code. Split four ways
    # (part A: 12 tools; part B: 10; part C: 5; part D: 4), each its own quesOS
    # boot: an earlier, larger single-fixture draft crashed the whole VM partway
    # through (an uncaught page fault; quesOS has no user-mode fault recovery
    # yet), and smaller boots stay well under whatever fork/pipe/stack-shape
    # threshold triggered it. Every tool is independently confirmed via a
    # standalone single-check harness during this card's own implementation.
    # uuencode is excluded entirely (not just moved to another part): even as the
    # SOLE check in its own single-tool fixture it reproducibly crashed the VM the
    # same way, so this is a real defect in that tool's own execution path (or its
    # interaction with the RT/quesOS), not a fixture-shape artifact; out of scope
    # to root-cause further here (decision 9695's stdin-only RT change), flagged
    # as a candidate for its own dedicated card.
    TOTAL=$((TOTAL + 1))
    set +e; out=$(ul292_run /progs/wave2_launch_a.mzx); set -e
    if printf '%s\n' "$out" | grep -qF "wave2-launch-a: PASS"; then
        echo "[PASS] userland292_wave2_launch_a (12 sbase tools, trivial smoke)"
    else
        echo "[FAIL] userland292_wave2_launch_a"; printf '%s\n' "$out" | sed 's/^/          | /'
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi

    TOTAL=$((TOTAL + 1))
    set +e; out=$(ul292_run /progs/wave2_launch_b.mzx); set -e
    if printf '%s\n' "$out" | grep -qF "wave2-launch-b: PASS"; then
        echo "[PASS] userland292_wave2_launch_b (10 sbase tools, trivial smoke)"
    else
        echo "[FAIL] userland292_wave2_launch_b"; printf '%s\n' "$out" | sed 's/^/          | /'
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi

    TOTAL=$((TOTAL + 1))
    set +e; out=$(ul292_run /progs/wave2_launch_c.mzx); set -e
    if printf '%s\n' "$out" | grep -qF "wave2-launch-c: PASS"; then
        echo "[PASS] userland292_wave2_launch_c (5 sbase tools, trivial smoke)"
    else
        echo "[FAIL] userland292_wave2_launch_c"; printf '%s\n' "$out" | sed 's/^/          | /'
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi

    TOTAL=$((TOTAL + 1))
    set +e; out=$(ul292_run /progs/wave2_launch_d.mzx); set -e
    if printf '%s\n' "$out" | grep -qF "wave2-launch-d: PASS"; then
        echo "[PASS] userland292_wave2_launch_d (4 sbase tools, trivial smoke)"
    else
        echo "[FAIL] userland292_wave2_launch_d"; printf '%s\n' "$out" | sed 's/^/          | /'
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi

    # AC 9685/9686: real stdin through oksh's own pipeline machinery (uniq dedup)
    # and libutil/crypt.c's stdin path specifically (md5sum's known "abc" digest).
    TOTAL=$((TOTAL + 1))
    set +e; out=$(ul292_run /progs/wave2_stdin_pipe.mzx); set -e
    if printf '%s\n' "$out" | grep -qF "wave2-stdin-pipe: PASS"; then
        echo "[PASS] userland292_stdin_pipe (uniq dedup + md5sum digest, real oksh pipe)"
    else
        echo "[FAIL] userland292_stdin_pipe"; printf '%s\n' "$out" | sed 's/^/          | /'
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi

    # Cycle-2 fix (code-review push-back): fclose(stdin) freed a static buffer and a
    # static FILE object onto the RT allocator's free-list, corrupting memory. This
    # is the read-back regression check the smoke tests above cannot catch, since
    # every process they drive exits immediately after its own fshut(stdin, ...): one
    # process reads real piped stdin, calls fclose(stdin), then allocates and reads
    # back a batch of heap blocks to confirm the free-list is still sound.
    TOTAL=$((TOTAL + 1))
    set +e; out=$(ul292_run /progs/wave2_stdin_reuse.mzx); set -e
    if printf '%s\n' "$out" | grep -qF "wave2-stdin-reuse: PASS"; then
        echo "[PASS] userland292_stdin_reuse (fclose(stdin) does not corrupt the heap)"
    else
        echo "[FAIL] userland292_stdin_reuse"; printf '%s\n' "$out" | sed 's/^/          | /'
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi

    # AC 9687: the patched kill against real target pids (signals 1/9/15) plus the
    # documented, honest kill -0 deviation.
    TOTAL=$((TOTAL + 1))
    set +e; out=$(ul292_run /progs/kill_launch.mzx); set -e
    if printf '%s\n' "$out" | grep -qF "kill-launch: PASS"; then
        echo "[PASS] userland292_kill (TERM/KILL/HUP against real pids + honest sig-0 deviation)"
    else
        echo "[FAIL] userland292_kill"; printf '%s\n' "$out" | sed 's/^/          | /'
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi

    # AC 9688: wave-1 unaffected in the sense that matters operationally (build
    # success + verify_mzx + correct execution). NOTE (found during this card's own
    # implementation): the literal "byte-identical .mzx" half of AC 9688 is
    # structurally impossible for ANY stdin-fix shape, because mzld links the whole
    # fixed RT object set into every image with no dead-code elimination (verified:
    # a build-only, code-comment-free RT diff still perturbs every wave-1 binary's
    # bytes). Re-running true/false/pwd/oksh here (already exercised above by
    # run_userland94_fixtures against THIS SAME build) is the behavioral half of
    # that AC; a byte-diff against a pre-stdin-fix baseline is a one-time Implement-
    # stage check (recorded on the card), not a standing CI assertion, since after
    # this card lands there IS no pre-stdin-fix baseline left to diff against.
}

_mz_want "run_userland_wave2_fixtures" && mz_timed "run_userland_wave2_fixtures" run_userland_wave2_fixtures

# maize-382 (decision D20): every heavy quesOS/userland build above now goes through
# mzcc, which leaves os/quesos/build-quesos.sh and userland/build-userland.sh with no
# automated exercise at all. Both are still live user-facing code:
# scripts/build-quesos.ps1 and scripts/build-userland.ps1 forward to them unmodified,
# and src/maize.cpp's missing-ROM diagnostic tells a user with no quesos.mzx to run
# them. This fixture keeps their real logic (staging, patching, linking, their own
# W^X and base-address choices) covered end to end. It runs ONCE per suite run and
# builds ONE cheap userland tool rather than the full set, so it stays off the
# per-fixture critical path this card exists to shorten.
#
# _smoke_verify_mzx reproduces build-userland.sh's own verify_mzx check
# (userland/build-userland.sh:34-48, size floor plus MZX magic) rather than booting
# the image: same definition of "loadable" the rest of the codebase already uses,
# at a fraction of the cost.
_smoke_verify_mzx() {
    _f="$1"
    [ -f "$_f" ] || return 1
    _sz=$(wc -c < "$_f" 2>/dev/null | tr -d ' ')
    [ -n "$_sz" ] && [ "$_sz" -ge 24 ] || return 1
    [ "$(dd if="$_f" bs=1 count=3 2>/dev/null)" = "MZX" ] || return 1
    return 0
}

run_sh_builder_smoke() {
    name="sh_builder_smoke"

    # quesOS: the actual .sh script, not mzcc, proving build-quesos.ps1's forwarded
    # path still works.
    q_out="${WORK_DIR}/sh-smoke-quesos.mzx"
    q_log="${WORK_DIR}/sh-smoke-quesos.log"
    rm -f "$q_out"
    TOTAL=$((TOTAL + 1))
    if ! sh "${REPO_ROOT}/os/quesos/build-quesos.sh" --preset "$PRESET" -o "$q_out" >"$q_log" 2>&1; then
        echo "[FAIL] ${name}: build-quesos.sh failed"; cat "$q_log" >&2
        FAIL_COUNT=$((FAIL_COUNT + 1))
    elif ! _smoke_verify_mzx "$q_out"; then
        echo "[FAIL] ${name}: build-quesos.sh produced an unloadable ${q_out}"
        FAIL_COUNT=$((FAIL_COUNT + 1))
    else
        echo "[PASS] ${name}: build-quesos.sh (POSIX) still builds a loadable quesos.mzx"
    fi

    # userland: ONE cheap tool only (not the full 43), to stay off the critical path;
    # proves build-userland.ps1's forwarded path still works.
    u_dir="${WORK_DIR}/sh-smoke-userland"
    u_log="${WORK_DIR}/sh-smoke-userland.log"
    rm -rf "$u_dir"; mkdir -p "$u_dir"
    TOTAL=$((TOTAL + 1))
    if ! command -v find >/dev/null 2>&1 || ! command -v cp >/dev/null 2>&1; then
        echo "[SKIP] ${name}: cp/find unavailable (cannot stage the sbase scratch tree)"
    elif ! sh "${REPO_ROOT}/userland/build-userland.sh" --preset "$PRESET" --out "$u_dir" true >"$u_log" 2>&1; then
        echo "[FAIL] ${name}: build-userland.sh failed to build 'true'"; cat "$u_log" >&2
        FAIL_COUNT=$((FAIL_COUNT + 1))
    elif ! _smoke_verify_mzx "${u_dir}/true.mzx"; then
        echo "[FAIL] ${name}: build-userland.sh produced an unloadable true.mzx"
        FAIL_COUNT=$((FAIL_COUNT + 1))
    else
        echo "[PASS] ${name}: build-userland.sh (POSIX) still builds a loadable true.mzx"
    fi
}

_mz_want "run_sh_builder_smoke" && mz_timed "run_sh_builder_smoke" run_sh_builder_smoke

# maize-376: a --only label that matched nothing is a wiring bug (a renamed dispatch
# site, a typo in the CMake test list), and it would otherwise report a vacuous
# "0 passed, 0 failed" success. Fail loudly instead.
if [ -n "$ONLY" ] && [ "$MZ_ONLY_MATCHED" -ne 1 ]; then
    echo "run-ctest.sh: --only '${ONLY}' matched no fixture dispatch label." >&2
    exit 2
fi

# maize-382 (AC 10193): where the suite's wall-clock actually goes, slowest first,
# printed just before the pass/fail line. maize-376: suppressed under --only, where
# it would be a one-line restatement of a number ctest already reports itself.
if [ -z "$ONLY" ] && [ -s "$TIMING_LOG" ]; then
    echo "-----------------------------------------------------------------------"
    echo "Fixture timings (seconds, slowest first):"
    sort -t' ' -k2,2rn "$TIMING_LOG" | while IFS=' ' read -r _lbl _secs; do
        printf '  %6ss  %s\n' "$_secs" "$_lbl"
    done
fi

echo "-----------------------------------------------------------------------"
if [ "$FAIL_COUNT" -eq 0 ]; then
    echo "C toolchain: ${TOTAL} passed, 0 failed."
    exit 0
else
    echo "C toolchain: $((TOTAL - FAIL_COUNT)) passed, ${FAIL_COUNT} failed."
    exit 1
fi
