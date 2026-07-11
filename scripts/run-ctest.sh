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
#     -> cc-maize.sh --compile-only -o <name>.mzx   (tr -> cpp -E -> cproc-qbe ->
#                              normalize -> qbe -t maize -> mazm -c -> mzld over the
#                              crt0/syscall + C runtime (errno/string/ctype/stdio/stdlib); entry _start; W^X)
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

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "${SCRIPT_DIR}/.." && pwd)
CTEST_DIR="${REPO_ROOT}/ctest"
RT_DIR="${REPO_ROOT}/toolchain/rt"
QBE_DIR="${REPO_ROOT}/toolchain/qbe"
CPROC_DIR="${REPO_ROOT}/toolchain/cproc"

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
        *) echo "Unknown argument: $1" >&2; exit 2 ;;
    esac
done

BUILD_DIR="${REPO_ROOT}/build/${PRESET}"
WORK_DIR="${BUILD_DIR}/ctest-run"

# Resolve an executable path, tolerating a .exe suffix on Windows.
resolve_exe() {
    if [ -x "$1" ] || [ -f "$1" ]; then echo "$1"; return 0; fi
    if [ -x "$1.exe" ] || [ -f "$1.exe" ]; then echo "$1.exe"; return 0; fi
    return 1
}

# Build the C toolchain if the compilers are absent (fresh-clone one-command).
if [ "$SKIP_BUILD" -eq 0 ]; then
    if ! resolve_exe "${QBE_DIR}/obj/qbe" >/dev/null \
    || ! resolve_exe "${CPROC_DIR}/cproc-qbe" >/dev/null; then
        "${SCRIPT_DIR}/build-toolchain.sh"
    fi
fi

# The whole C compile pipeline (tr -> cpp -> cproc-qbe -> normalize -> qbe -> mazm -c
# -> mzld) lives in scripts/cc-maize.sh (maize-96); this harness drives it via
# --compile-only so CI exercises the EXACT pipeline the operator acceptance-tests with.
# run-ctest therefore no longer resolves cproc-qbe / qbe / the system cpp itself: the
# driver owns those. It still needs mazm (to re-assemble the W^X probe's crt0.mzo),
# maize (to run each linked image), and mzld (the W^X negative case).
CC_MAIZE="${SCRIPT_DIR}/cc-maize.sh"
[ -f "$CC_MAIZE" ] || { echo "run-ctest.sh: driver ${CC_MAIZE} not found." >&2; exit 2; }
MAZM=$(resolve_exe "${BUILD_DIR}/mazm") || {
    echo "run-ctest.sh: mazm not found in ${BUILD_DIR}; run scripts/run-tests.sh first." >&2; exit 2; }
MAIZE=$(resolve_exe "${BUILD_DIR}/maize") || {
    echo "run-ctest.sh: maize not found in ${BUILD_DIR}; run scripts/run-tests.sh first." >&2; exit 2; }
MZLD=$(resolve_exe "${BUILD_DIR}/mzld") || {
    echo "run-ctest.sh: mzld not found in ${BUILD_DIR}; run scripts/run-tests.sh first." >&2; exit 2; }

mkdir -p "${WORK_DIR}"

FAIL_COUNT=0
TOTAL=0

# Compile a C fixture to a runnable .mzx by delegating to the shared driver in
# --compile-only mode (maize-96). cc-maize.sh owns the whole pipeline end to end
# (tr -> cpp -> cproc-qbe -> normalize -> qbe -t maize -> mazm -c -> mzld over the
# crt0/syscall + C runtime (errno/string/ctype/stdio/stdlib) set); this harness just asks for the linked image
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

    mzx="${WORK_DIR}/${name}.mzx"
    if ! "$CC_MAIZE" --preset "$PRESET" --compile-only -o "$mzx" "$src" \
        >"${WORK_DIR}/${name}.cc.log" 2>&1 || [ ! -f "$mzx" ]; then
        echo "[FAIL] ${name}: C compile failed"; cat "${WORK_DIR}/${name}.cc.log" >&2
        FAIL_COUNT=$((FAIL_COUNT + 1)); return 1
    fi
    BIN="$mzx"
    return 0
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
    out="${WORK_DIR}/${name}.out"
    exp="${WORK_DIR}/${name}.exp"
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
    # compile_c wrote ${WORK_DIR}/args.mzx; run from WORK_DIR so argv[0] == args.mzx.

    out="${WORK_DIR}/${name}.out"
    exp="${WORK_DIR}/${name}.exp"
    ( cd "$WORK_DIR" && "$MAIZE" --env GREETING=hi --env TARGET=maize args.mzx alpha beta ) \
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

echo "=== C toolchain end-to-end (cproc -> qbe -t maize -> mazm -c -> mzld -> maize) ==="
run_ctest "hello"
run_ctest "capstone"
run_ctest "globals"
run_ctest "ptrdata"
# maize-101 codegen-gap regressions: bug #1 (void call with args -> spill.c dead
# reg) and bug #3 (&&/ternary phi cycle -> Oswap die), both overlay-only.
run_ctest "voidcall"
run_ctest "freelist"
# maize-103 codegen-gap regression: an &local carried DIRECTLY as a loop-carried
# phi argument (freelist's inverse, no opaque() barrier). Pre-fix maize_isel never
# ran fixarg over successor phi args, so the alloc temp reached rega and the phi
# edge became a plain slot MOVE of the local's contents instead of a LEA of its
# address: a silent wrong answer. Overlay-only fix in qbe-maize/isel.c.
run_ctest "addrlocalphi"
# maize-74 syscall C binding: raw stub direct (AC 7290), wrapper success returns the
# byte count (AC 7291), and error-range translation sets errno + returns -1 (AC 7292).
run_ctest "syscall_raw"
run_ctest "syscall_write"
run_ctest "syscall_errno"
# maize-76 freestanding libc slice: string.h (str), ctype.h (ctype), the malloc
# family over the sbrk free-list allocator (malloc), and the sbrk wrapper itself
# (sbrk). Each is a self-checking fixture printing a single PASS line.
run_ctest "str"
run_ctest "ctype"
run_ctest "sbrk"
run_ctest "malloc"
# maize-98 varargs / stdarg ABI: a self-checking fixture exercising the register
# save area, va_arg over mixed scalar classes, the register->overflow boundary,
# and va_copy. Prints a single PASS line.
run_ctest "varargs"
# maize-99 variadic printf over the stdarg ABI: direct-emit correctness for every
# conversion (%d %i %u %x %X %c %s %p %%, %ld/%lu/%lx, width + zero-pad, INT_MIN /
# LONG_MIN) matched byte-for-byte, plus an snprintf return/truncation self-check
# and a >256-byte line proving chunked flush. Ends in a single "selfcheck PASS".
run_ctest "printf"
run_exit_status_test "exitcode" 42
# maize-76: abort() terminates with status 134 (128 + SIGABRT(6); no signals).
run_exit_status_test "abort" 134
# maize-102: an own-TU _Noreturn function (die) calls exit(57); its `hlt` end block
# (and main's tail block, which calls the _Noreturn-declared die) traverse cfg.c
# simpljmp before emit, so a regression in the hlt-guard hunk crashes this at
# compile time rather than passing silently. Proves qbe -t maize parses/lowers hlt.
run_exit_status_test "noreturn" 57
run_args_test
run_wx_reject_test

echo "-----------------------------------------------------------------------"
if [ "$FAIL_COUNT" -eq 0 ]; then
    echo "C toolchain: ${TOTAL} passed, 0 failed."
    exit 0
else
    echo "C toolchain: $((TOTAL - FAIL_COUNT)) passed, ${FAIL_COUNT} failed."
    exit 1
fi
