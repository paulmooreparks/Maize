#!/bin/sh
# Compile + run the C hello-world through the full Maize C toolchain and diff its
# stdout against the committed fixture (maize-62, maize-11 AC 6397 / 6399).
#
# Pipeline (maize-77 segmented .mzo -> mzld -> .mzx object model):
#
#   ctest/hello.c
#     -> cproc-qbe            (C11 -> QBE IL)
#     -> normalize            (drop the `extern` call-linkage annotation; see below)
#     -> qbe -t maize         (QBE IL -> mazm, with SECTION/GLOBAL/ALIGN/DREF)
#     -> mazm -c              (body.mazm -> body.mzo relocatable object)
#     -> mzld                 (crt0.mzo syscall.mzo puts.mzo body.mzo -> hello.mzx;
#                              default entry _start; W^X, per-section alignment)
#     -> maize                (load_mzx sets RP=_start; execute; capture stdout)
#     -> diff vs ctest/hello.expected
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
# `extern` normalization: the pinned cproc (d1c53dd) emits `call extern $sym`, a
# call-linkage annotation the pinned qbe (4420727) predates and rejects. In the
# Maize single-TU whole-program model (decision 6411/6636) all symbols resolve in
# mazm's one shared label table, so the annotation carries no meaning and is
# normalized away deterministically here.
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

CPROC_QBE=$(resolve_exe "${CPROC_DIR}/cproc-qbe") || {
    echo "run-ctest.sh: cproc-qbe not found; run scripts/build-toolchain.sh first." >&2; exit 2; }
QBE=$(resolve_exe "${QBE_DIR}/obj/qbe") || {
    echo "run-ctest.sh: qbe not found; run scripts/build-toolchain.sh first." >&2; exit 2; }
MAZM=$(resolve_exe "${BUILD_DIR}/mazm") || {
    echo "run-ctest.sh: mazm not found in ${BUILD_DIR}; run scripts/run-tests.sh first." >&2; exit 2; }
MAIZE=$(resolve_exe "${BUILD_DIR}/maize") || {
    echo "run-ctest.sh: maize not found in ${BUILD_DIR}; run scripts/run-tests.sh first." >&2; exit 2; }
MZLD=$(resolve_exe "${BUILD_DIR}/mzld") || {
    echo "run-ctest.sh: mzld not found in ${BUILD_DIR}; run scripts/run-tests.sh first." >&2; exit 2; }

mkdir -p "${WORK_DIR}"

# Assemble the freestanding runtime as relocatable objects once per run (maize-77
# decision 7168): crt0/syscall/puts each become a .mzo, linked into every fixture's
# .mzx. mazm -c writes <input>.mzo beside its input, so the sources are copied into
# WORK_DIR first (keeping the source tree clean). A failure here is a setup failure.
RT_OBJS=""
for rt in crt0 syscall puts; do
    cp "${RT_DIR}/${rt}.mazm" "${WORK_DIR}/${rt}.mazm"
    if ! "$MAZM" -c "${WORK_DIR}/${rt}.mazm" >"${WORK_DIR}/${rt}.mazm.log" 2>&1 \
    || [ ! -f "${WORK_DIR}/${rt}.mzo" ]; then
        echo "run-ctest.sh: failed to assemble runtime object ${rt}.mazm:" >&2
        cat "${WORK_DIR}/${rt}.mazm.log" >&2
        exit 2
    fi
    RT_OBJS="${RT_OBJS} ${WORK_DIR}/${rt}.mzo"
done

FAIL_COUNT=0
TOTAL=0

# Compile a C fixture through the full SEGMENTED pipeline (maize-77):
#   cproc-qbe -> normalize -> qbe -t maize -> mazm -c (body -> .mzo)
#     -> mzld (crt0.mzo syscall.mzo puts.mzo body.mzo -> .mzx, default entry _start)
# On success sets BIN to the linked .mzx and returns 0; on failure prints a [FAIL]
# line, bumps FAIL_COUNT, and returns 1. Shared by the stdout runner (run_ctest), the
# exit-status runner (run_exit_status_test), and the argv runner (run_args_test) so
# all three exercise the identical toolchain path. The runtime is no longer cat-
# prepended: it is assembled to objects (RT_OBJS, above) and linked.
compile_c() {
    name="$1"
    src="${CTEST_DIR}/${name}.c"

    if [ ! -f "$src" ]; then
        echo "[FAIL] ${name}: missing source fixture" >&2
        FAIL_COUNT=$((FAIL_COUNT + 1))
        return 1
    fi

    lfsrc="${WORK_DIR}/${name}.lf.c"
    ssa="${WORK_DIR}/${name}.ssa"
    norm="${WORK_DIR}/${name}.norm.ssa"
    body="${WORK_DIR}/${name}.body.mazm"
    bodyobj="${WORK_DIR}/${name}.body.mzo"
    mzx="${WORK_DIR}/${name}.mzx"

    # Defense in depth for CRLF checkouts (belt-and-suspenders with .gitattributes
    # eol=lf): cproc is strict C11 and treats a bare CR as a stray token, so strip
    # any CR bytes before feeding the source to the front-end. A clean LF checkout
    # makes this a no-op. (maize-62)
    tr -d '\r' < "$src" > "$lfsrc"

    if ! "$CPROC_QBE" < "$lfsrc" > "$ssa" 2>"${WORK_DIR}/${name}.cproc.log"; then
        echo "[FAIL] ${name}: cproc-qbe failed"; cat "${WORK_DIR}/${name}.cproc.log" >&2
        FAIL_COUNT=$((FAIL_COUNT + 1)); return 1
    fi
    # Normalize two IL-version-skew points between the pinned cproc and the
    # pinned qbe:
    #   - `<op> extern $sym` -> `<op> $sym`  (symbol-linkage annotation the pinned
    #     qbe predates). cproc tags EVERY external-symbol operand `extern`, not only
    #     call targets: `call extern $puts`, but also `loadsb extern $g`,
    #     `add extern $arr, 4`, etc. In the Maize linked-image model every `$sym`
    #     resolves through mzld regardless, so the annotation carries no meaning and
    #     is stripped wherever it prefixes a `$`. (maize-77: file-scope globals are
    #     the first fixtures to read externs outside a call, surfacing the non-call
    #     forms; hello/capstone only ever hit `call extern`.)
    #   - `=<w|l> neg X`      -> `=<w|l> sub 0, X`  (cproc emits the `neg` unary op,
    #     which this pinned qbe's parser predates; `sub 0, X` is the identity
    #     lowering and carries the same class/semantics).
    # NOTE: keep this sed in sync with the maize-cc local test harness's normalizer.
    sed -e 's/extern \$/$/g' \
        -e 's/\(=[wl]\) neg /\1 sub 0, /' "$ssa" > "$norm"
    if ! "$QBE" -t maize "$norm" > "$body" 2>"${WORK_DIR}/${name}.qbe.log"; then
        echo "[FAIL] ${name}: qbe -t maize failed"; cat "${WORK_DIR}/${name}.qbe.log" >&2
        FAIL_COUNT=$((FAIL_COUNT + 1)); return 1
    fi
    if ! "$MAZM" -c "$body" >"${WORK_DIR}/${name}.mazm.log" 2>&1 || [ ! -f "$bodyobj" ]; then
        echo "[FAIL] ${name}: mazm -c failed"; cat "${WORK_DIR}/${name}.mazm.log" >&2
        FAIL_COUNT=$((FAIL_COUNT + 1)); return 1
    fi
    if ! "$MZLD" -o "$mzx" ${RT_OBJS} "$bodyobj" >"${WORK_DIR}/${name}.mzld.log" 2>&1 \
    || [ ! -f "$mzx" ]; then
        echo "[FAIL] ${name}: mzld failed"; cat "${WORK_DIR}/${name}.mzld.log" >&2
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

    probe="${WORK_DIR}/wx_probe.mzo"
    cp "${WORK_DIR}/crt0.mzo" "$probe"
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
run_exit_status_test "exitcode" 42
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
