#!/bin/sh
# Build both Maize binaries and run the in-scope asm/ test suite (Linux/CI entry
# point; this is what maize-36 invokes).
#
# Configures and builds the linux-debug preset (or an override given via
# --preset), then assembles and runs each of the 21 in-scope tests under asm/,
# comparing captured stdout against the expected output embedded below. Prints
# a per-test PASS/FAIL report plus a summary line. Never prompts for input, so
# it is safe for non-interactive CI use.
#
# Exit codes:
#   0 - all tests passed
#   1 - one or more tests failed
#   2 - environment/setup failure (missing cmake/ninja, configure/build
#       failure, missing built executables) that prevented tests from running
#       at all
#
# Usage: scripts/run-tests.sh [--preset <name>] [--skip-build]

set -eu

# --- Paths resolved relative to THIS script, not the caller's CWD ----------------
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "${SCRIPT_DIR}/.." && pwd)
ASM_DIR="${REPO_ROOT}/asm"

# --- Preset selection: dispatch on uname, with a manual override ------------------
UNAME=$(uname -s)
case "$UNAME" in
    Linux)  DEFAULT_PRESET='linux-debug' ;;
    Darwin) DEFAULT_PRESET='macos-debug' ;;
    *)
        echo "unsupported platform for run-tests.sh: ${UNAME}" >&2
        exit 2
        ;;
esac

PRESET="$DEFAULT_PRESET"
SKIP_BUILD=0

while [ $# -gt 0 ]; do
    case "$1" in
        --preset)
            PRESET="${2:-}"
            if [ -z "$PRESET" ]; then
                echo "--preset requires a value" >&2
                exit 2
            fi
            shift 2
            ;;
        --preset=*)
            PRESET="${1#--preset=}"
            shift
            ;;
        --skip-build)
            SKIP_BUILD=1
            shift
            ;;
        *)
            echo "Unknown argument: $1" >&2
            exit 2
            ;;
    esac
done

BUILD_DIR="${REPO_ROOT}/build/${PRESET}"
TEST_RUN_DIR="${BUILD_DIR}/test-run"

# --- Resolve build tools before attempting a configure ----------------------------
resolve_cmake() {
    if command -v cmake >/dev/null 2>&1; then
        command -v cmake
        return 0
    fi
    return 1
}

resolve_ninja() {
    if command -v ninja >/dev/null 2>&1; then
        command -v ninja
        return 0
    fi
    return 1
}

if CMAKE_EXE=$(resolve_cmake); then
    :
else
    echo "cmake not found on PATH." >&2
    echo "Install it: sudo apt install cmake" >&2
    exit 2
fi

if NINJA_EXE=$(resolve_ninja); then
    :
else
    echo "ninja not found on PATH." >&2
    echo "Install it: sudo apt install ninja-build" >&2
    exit 2
fi

# --- Configure + build -------------------------------------------------------------
if [ "$SKIP_BUILD" -eq 0 ]; then
    if ! (cd "$REPO_ROOT" && "$CMAKE_EXE" --preset "$PRESET"); then
        echo "cmake configure failed for preset '${PRESET}'." >&2
        exit 2
    fi
    if ! (cd "$REPO_ROOT" && "$CMAKE_EXE" --build --preset "$PRESET"); then
        echo "cmake build failed for preset '${PRESET}'." >&2
        exit 2
    fi
fi

# --- Locate the built executables --------------------------------------------------
MAIZE_EXE="${BUILD_DIR}/maize"
MAZM_EXE="${BUILD_DIR}/mazm"
MZLD_EXE="${BUILD_DIR}/mzld"

if [ ! -x "$MAZM_EXE" ]; then
    echo "Expected built executable not found: ${MAZM_EXE}" >&2
    exit 2
fi
if [ ! -x "$MAIZE_EXE" ]; then
    echo "Expected built executable not found: ${MAIZE_EXE}" >&2
    exit 2
fi
if [ ! -x "$MZLD_EXE" ]; then
    echo "Expected built executable not found: ${MZLD_EXE}" >&2
    exit 2
fi

mkdir -p "$TEST_RUN_DIR"

# --- Per-test execution -------------------------------------------------------------
FAIL_COUNT=0
TOTAL=0

# Expected values already have trailing newlines stripped: command substitution
# strips all trailing newlines from captured output, which is exactly the
# comparison rule (src/maize.cpp appends an extra trailing newline on Linux
# only; this keeps the same expected table valid on both platforms).
run_test() {
    name="$1"
    file="$2"
    expected="$3"
    golden="$4"
    expect_asm_error="${5:-0}"
    TOTAL=$((TOTAL + 1))

    if [ "$golden" -eq 1 ]; then
        asm_path="${ASM_DIR}/${file}"
    else
        src_path="${ASM_DIR}/${file}"
        if [ ! -f "$src_path" ]; then
            echo "Missing test source file: ${src_path}" >&2
            echo "Setup failure: declared test '${name}' has no corresponding .mazm file." >&2
            exit 2
        fi
        cp "$src_path" "${TEST_RUN_DIR}/${file}"
        asm_path="${TEST_RUN_DIR}/${file}"
    fi
    bin_path="${asm_path%.mazm}.bin"

    mazm_log=$(mktemp)
    if "$MAZM_EXE" "$asm_path" >"$mazm_log" 2>&1; then
        mazm_exit=0
    else
        mazm_exit=$?
    fi

    # Negative test: the assembler must reject this source with a diagnostic
    # containing "$expected". Passes iff mazm exits nonzero and says so.
    if [ "$expect_asm_error" -eq 1 ]; then
        actual=$(cat "$mazm_log")
        rm -f "$mazm_log"
        if [ "$mazm_exit" -ne 0 ] && printf '%s' "$actual" | grep -qF "$expected"; then
            echo "[PASS] ${name}"
        else
            FAIL_COUNT=$((FAIL_COUNT + 1))
            echo "[FAIL] ${name}"
            echo "        expected: assembler rejects with: \"${expected}\""
            echo "        actual:   \"${actual}\""
        fi
        return
    fi

    if [ "$mazm_exit" -ne 0 ] || [ ! -f "$bin_path" ]; then
        actual=$(cat "$mazm_log")
        rm -f "$mazm_log"
        FAIL_COUNT=$((FAIL_COUNT + 1))
        echo "[FAIL] ${name}"
        echo "        expected: \"${expected}\""
        echo "        actual:   \"${actual}\""
        return
    fi
    rm -f "$mazm_log"

    stdout_file=$(mktemp)
    if "$MAIZE_EXE" "$bin_path" >"$stdout_file" 2>/dev/null; then
        maize_exit=0
    else
        maize_exit=$?
    fi
    actual=$(cat "$stdout_file")
    rm -f "$stdout_file"

    if [ "$maize_exit" -eq 0 ] && [ "$actual" = "$expected" ]; then
        echo "[PASS] ${name}"
    else
        FAIL_COUNT=$((FAIL_COUNT + 1))
        echo "[FAIL] ${name}"
        echo "        expected: \"${expected}\""
        echo "        actual:   \"${actual}\""
    fi
}

run_test "hello"             "hello.mazm"             "Hello, world!"                 1
run_test "test_mul"          "test_mul.mazm"          "MUL test: PASS (1/2/4/8-byte)" 0
run_test "test_mulw"         "test_mulw.mazm"         "MULW test: PASS"               0
run_test "test_flags_arith"  "test_flags_arith.mazm"  "flags arith: PASS"             0
run_test "test_flags_branch" "test_flags_branch.mazm" "flags branch: PASS"            0
run_test "test_flags_shl"    "test_flags_shl.mazm"    "flags shl: PASS"               0
run_test "test_flags_shr"    "test_flags_shr.mazm"    "flags shr: PASS"               0
run_test "test_flags_mul8"   "test_flags_mul8.mazm"   "flags mul8: PASS"              0
run_test "test_flags_move"   "test_flags_move.mazm"   "flags move: PASS"              0
run_test "test_addr64"       "test_addr64.mazm"       "addr64: PASS"                  0
run_test "test_cmptest"      "test_cmptest.mazm"      "cmptest: PASS"                 0
run_test "test_ldimm"        "test_ldimm.mazm"        "ld imm: PASS"                  0
run_test "test_stack64"      "test_stack64.mazm"      "stack64: PASS"                 0
run_test "test_div"          "test_div.mazm"          "div: PASS"                     0
run_test "test_jcc"          "test_jcc.mazm"          "jcc: PASS"                     0
run_test "test_memblock"     "test_memblock.mazm"     "memblock: PASS"                0
run_test "test_crossblock"   "test_crossblock.mazm"   "crossblk: PASS"                0
run_test "test_adc"          "test_adc.mazm"          "adc: PASS"                     0
run_test "test_copywidth"    "test_copywidth.mazm"    "copywidth: PASS"               0
run_test "reject_ld_value"   "test_reject_ldval.mazm" "reads from a memory address"   0 1
run_test "reject_ldz"        "test_reject_ldz.mazm"   "unknown keyword or opcode 'LDZ'" 0 1

# --- maize-12: multi-TU assemble -> link -> run --------------------------------------
# Two separately-assembled objects (link_a defines _start and imports from link_b)
# are linked into one .mzx executable and run under maize.
emit_object() {
    src="$1"
    cp "${ASM_DIR}/${src}" "${TEST_RUN_DIR}/${src}"
    "$MAZM_EXE" -c "${TEST_RUN_DIR}/${src}" >/dev/null 2>&1
}

run_link_run_test() {
    name="link_multi_tu"
    expected="Linked!"
    TOTAL=$((TOTAL + 1))
    emit_object "link_a.mazm"
    emit_object "link_b.mazm"
    log=$(mktemp)
    if ! "$MZLD_EXE" -o "${TEST_RUN_DIR}/link.mzx" \
            "${TEST_RUN_DIR}/link_a.mzo" "${TEST_RUN_DIR}/link_b.mzo" >"$log" 2>&1; then
        FAIL_COUNT=$((FAIL_COUNT + 1))
        echo "[FAIL] ${name} (link failed)"
        cat "$log"
        rm -f "$log"
        return
    fi
    rm -f "$log"
    out=$(mktemp)
    if "$MAIZE_EXE" "${TEST_RUN_DIR}/link.mzx" >"$out" 2>/dev/null; then
        me=0
    else
        me=$?
    fi
    actual=$(cat "$out")
    rm -f "$out"
    if [ "$me" -eq 0 ] && [ "$actual" = "$expected" ]; then
        echo "[PASS] ${name}"
    else
        FAIL_COUNT=$((FAIL_COUNT + 1))
        echo "[FAIL] ${name}"
        echo "        expected: \"${expected}\""
        echo "        actual:   \"${actual}\""
    fi
}

# name, expected-substring, then mzld object arguments
run_link_reject_test() {
    name="$1"
    expected="$2"
    shift 2
    TOTAL=$((TOTAL + 1))
    log=$(mktemp)
    if "$MZLD_EXE" -o "${TEST_RUN_DIR}/err.mzx" "$@" >"$log" 2>&1; then
        ec=0
    else
        ec=$?
    fi
    actual=$(cat "$log")
    rm -f "$log"
    if [ "$ec" -ne 0 ] && printf '%s' "$actual" | grep -qF "$expected"; then
        echo "[PASS] ${name}"
    else
        FAIL_COUNT=$((FAIL_COUNT + 1))
        echo "[FAIL] ${name}"
        echo "        expected reject: \"${expected}\""
        echo "        actual:          \"${actual}\""
    fi
}

run_link_run_test
run_link_reject_test "link_undefined_symbol" "undefined symbol 'msgB'" "${TEST_RUN_DIR}/link_a.mzo"
emit_object "link_range.mazm"
run_link_reject_test "link_range_overflow" "does not fit in 8-bit" "${TEST_RUN_DIR}/link_range.mzo"

PASS_COUNT=$((TOTAL - FAIL_COUNT))
echo ""
echo "${PASS_COUNT} passed, ${FAIL_COUNT} failed (${TOTAL} total)"

if [ "$FAIL_COUNT" -gt 0 ]; then
    exit 1
fi
exit 0
