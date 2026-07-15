#!/bin/sh
# Build both Maize binaries and run the in-scope asm/ test suite (Linux/CI entry
# point; this is what maize-36 invokes).
#
# Configures and builds the linux-debug preset (or an override given via
# --preset), then assembles and runs each of the 49 in-scope tests under asm/,
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
MZDIS_EXE="${BUILD_DIR}/mzdis"

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
if [ ! -x "$MZDIS_EXE" ]; then
    echo "Expected built executable not found: ${MZDIS_EXE}" >&2
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
    bin_path="${asm_path%.mazm}.mzb"

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
run_test "test_flags_arith"  "test_flags_arith.mazm"  "flags arith: PASS"             1
run_test "test_flags_branch" "test_flags_branch.mazm" "flags branch: PASS"            0
run_test "test_flags_shl"    "test_flags_shl.mazm"    "flags shl: PASS"               0
run_test "test_flags_shr"    "test_flags_shr.mazm"    "flags shr: PASS"               0
run_test "test_flags_sar"    "test_flags_sar.mazm"    "flags sar: PASS"               0
run_test "test_flags_mul8"   "test_flags_mul8.mazm"   "flags mul8: PASS"              0
run_test "test_mul_zero"     "test_mul_zero.mazm"     "mul zero: PASS"                1
run_test "test_flags_move"   "test_flags_move.mazm"   "flags move: PASS"              0
# maize-197 lazy / on-demand flag computation: boundary tests for the materialize
# hook (SETcc/Jcc across an intervening non-flag op) and the narrow / neutral flag
# producers (SETCRY/CLRCRY, CMPXCHG, LEA, FCMP) run after an unresolved pending op.
run_test "test_flags_lazy_setcc"   "test_flags_lazy_setcc.mazm"   "flags lazy setcc: PASS"   1
run_test "test_flags_lazy_setcry"  "test_flags_lazy_setcry.mazm"  "flags lazy setcry: PASS"  1
run_test "test_flags_lazy_cmpxchg" "test_flags_lazy_cmpxchg.mazm" "flags lazy cmpxchg: PASS" 1
run_test "test_flags_lazy_lea"     "test_flags_lazy_lea.mazm"     "flags lazy lea: PASS"     1
run_test "test_flags_lazy_fcmp"    "test_flags_lazy_fcmp.mazm"    "flags lazy fcmp: PASS"    1
run_test "test_addr64"       "test_addr64.mazm"       "addr64: PASS"                  0
run_test "test_cmptest"      "test_cmptest.mazm"      "cmptest: PASS"                 0
run_test "test_ldimm"        "test_ldimm.mazm"        "ld imm: PASS"                  0
run_test "test_stack64"      "test_stack64.mazm"      "stack64: PASS"                 0
run_test "test_rsinit"       "test_rsinit.mazm"       "rsinit: PASS"                  0
run_test "test_div"          "test_div.mazm"          "div: PASS"                     0
run_test "test_jcc"          "test_jcc.mazm"          "jcc: PASS"                     0
run_test "test_jcc_all"      "test_jcc_all.mazm"      "jcc-all: PASS"                 0
run_test "test_neg"          "test_neg.mazm"          "neg: PASS"                     0
run_test "test_setcc"        "test_setcc.mazm"        "setcc: PASS"                   0
run_test "test_setcc_alias"  "test_setcc_alias.mazm"  "setcc-alias: PASS"             0
run_test "test_memblock"     "test_memblock.mazm"     "memblock: PASS"                0
run_test "test_widecount"    "test_widecount.mazm"    "widecount: PASS"               0
run_test "test_crossblock"   "test_crossblock.mazm"   "crossblk: PASS"                0
run_test "test_adc"          "test_adc.mazm"          "adc: PASS"                     0
run_test "test_copywidth"    "test_copywidth.mazm"    "copywidth: PASS"               0
run_test "oob_subreg_guard"  "test_oob_subreg.mazm"   "oob subreg: PASS"              0
run_test "reject_ld_value"   "test_reject_ldval.mazm" "reads from a memory address"   0 1
run_test "test_ldz"          "test_ldz.mazm"          "ldz: PASS"                     0
run_test "test_call_ind"     "test_call_ind.mazm"     "call ind: PASS"                0
run_test "test_setint"       "test_setint.mazm"       "setint: PASS"                  0
run_test "test_outr_in"      "test_outr_in.mazm"      "outr/in: PASS"                 0
run_test "test_unpop_port"   "test_unpop_port.mazm"   "unpop: PASS"                   0
run_test "test_portio"       "test_portio.mazm"       "portio: PASS"                  0
run_test "test_timer"        "test_timer.mazm"        "timer: PASS"                   0
run_test "test_framebuffer"  "test_framebuffer.mazm"  "framebuffer: PASS"             0
run_test "test_sysbrk"       "test_sysbrk.mazm"       "sysbrk: PASS"                  0
run_test "test_syserrno"     "test_syserrno.mazm"     "syserrno: PASS"                0
run_test "test_sysroute"     "test_sysroute.mazm"     "sysroute: PASS"                0
run_test "test_tstind"       "test_tstind.mazm"       "tstind: PASS"                  0
run_test "reject_bad_register"  "test_reject_badreg.mazm"        "unknown register 'R99'" 0 1
run_test "reject_bad_literal"   "test_reject_badliteral.mazm"    "malformed hex literal"  0 1
run_test "reject_include_self"  "test_reject_include_self.mazm"  "circular INCLUDE"       0 1
run_test "reject_label_trunc"   "test_reject_label_trunc.mazm"   "unexpected end of file" 0 1
run_test "reject_address_trunc" "test_reject_address_trunc.mazm" "unexpected end of file" 0 1
run_test "reject_jcc_reg"       "test_reject_jcc_reg.mazm"       "immediate target only"  0 1
run_test "reject_jmp_subreg"    "test_reject_jmp_subreg.mazm"    "full 64-bit width"      0 1
run_test "test_fp"              "test_fp.mazm"                   "fp: PASS"               0
run_test "reject_fp_subreg"     "test_fp_reject_subreg.mazm"     "B* or Q* subregister"   0 1
run_test "reject_fp_mixwidth"   "test_fp_reject_mixwidth.mazm"   "same floating-point width" 0 1
run_test "nested_include"       "test_nested_include.mazm"       "nested include: PASS"   1
run_test "address_fwdlabel"     "test_address_fwdlabel.mazm"     "address fwd-ref: PASS"  0
run_test "mmu_cr_roundtrip"     "test_mmu_cr_roundtrip.mazm"     "cr-roundtrip: PASS"     0

# maize-194: Sv48 translation, software TLB, and cause-8 page fault. Each fixture builds a
# small page table in guest RAM, MOVTCRs MODE=1 into CR0, and self-checks (PASS/FAIL
# marker) the translation, permission, TLB-invalidation, and page-fault paths.
run_test "mmu_translate_rw"     "test_mmu_translate_rw.mazm"          "mmu-xlate-rw: PASS"    0
run_test "mmu_pagefault_np"     "test_mmu_pagefault_notpresent.mazm"  "mmu-fault-np: PASS"    0
run_test "mmu_pagefault_ro"     "test_mmu_pagefault_ro.mazm"          "mmu-fault-ro: PASS"    0
run_test "mmu_pagefault_user"   "test_mmu_pagefault_user.mazm"        "mmu-fault-user: PASS"  0
run_test "mmu_tlb_invalidate"   "test_mmu_tlb_invalidate.mazm"        "mmu-tlb: PASS"         0
run_test "mmu_translate_alu_ra" "test_mmu_translate_alu_regaddr.mazm" "mmu-alu-ra: PASS"      0
run_test "mmu_translate_ind_cf" "test_mmu_translate_indirect_cf.mazm" "mmu-ind-cf: PASS"      0
run_test "mmu_translate_out"    "test_mmu_translate_out.mazm"         "mmu-out: PASS"         0
run_test "mmu_pushfault_restart" "test_mmu_pushfault_restart.mazm"    "mmu-pushfault: PASS"   0

# maize-72: per-reference undefined-label diagnostics. Several distinct undefined
# labels referenced from distinct lines must each report at their OWN file:line, and a
# label referenced from TWO different lines (undefined_beta, lines 14 and 19) must
# report on BOTH lines rather than twice on the first and never on the second. The
# generic run_test negative form only greps for a single substring, so this fixture
# gets a bespoke check that asserts each expected file:line diagnostic is present.
run_undef_multiref_test() {
    name="undef_multiref"
    TOTAL=$((TOTAL + 1))
    src="test_reject_undef_multiref.mazm"
    cp "${ASM_DIR}/${src}" "${TEST_RUN_DIR}/${src}"
    asm_path="${TEST_RUN_DIR}/${src}"

    log=$(mktemp)
    if "$MAZM_EXE" "$asm_path" >"$log" 2>&1; then
        ec=0
    else
        ec=$?
    fi
    actual=$(cat "$log")
    rm -f "$log"

    if [ "$ec" -ne 0 ] \
        && printf '%s' "$actual" | grep -qE ":14: error: undefined label 'undefined_beta'" \
        && printf '%s' "$actual" | grep -qE ":15: error: undefined label 'undefined_alpha'" \
        && printf '%s' "$actual" | grep -qE ":19: error: undefined label 'undefined_beta'" \
        && printf '%s' "$actual" | grep -qE ":20: error: undefined label 'undefined_gamma'"; then
        echo "[PASS] ${name}"
    else
        FAIL_COUNT=$((FAIL_COUNT + 1))
        echo "[FAIL] ${name}"
        echo "        expected: nonzero exit; diagnostics at lines 14, 15, 19, 20, each on its own site"
        echo "        actual:   exit ${ec}; \"${actual}\""
    fi
}

run_undef_multiref_test

# --- maize-78: BRK is a defined breakpoint trap, not a no-op --------------------------
# BRK ($FF) raises a breakpoint trap (cause 3). With no handler installed (the maize-21
# vector table does not exist yet) an unhandled synchronous trap halts the VM
# deterministically with the cause surfaced, through the same throw-and-exit mechanism
# raise_divide_error uses. test_brk.mazm places an unreachable "brk: FAIL" marker right
# after BRK; the generic run_test cannot express "expected to trap", so this bespoke
# runner asserts the VM exits nonzero, surfaces a "breakpoint" diagnostic on stderr, and
# never reaches the fall-through marker (which the old no-op behavior would have printed).
run_brk_trap_test() {
    name="brk_trap"
    TOTAL=$((TOTAL + 1))
    src="test_brk.mazm"
    cp "${ASM_DIR}/${src}" "${TEST_RUN_DIR}/${src}"
    asm_path="${TEST_RUN_DIR}/${src}"
    bin_path="${asm_path%.mazm}.mzb"
    if ! "$MAZM_EXE" "$asm_path" >/dev/null 2>&1 || [ ! -f "$bin_path" ]; then
        FAIL_COUNT=$((FAIL_COUNT + 1))
        echo "[FAIL] ${name} (mazm failed to assemble)"
        return
    fi
    out_file=$(mktemp)
    err_file=$(mktemp)
    if "$MAIZE_EXE" "$bin_path" >"$out_file" 2>"$err_file"; then
        me=0
    else
        me=$?
    fi
    out=$(cat "$out_file")
    err=$(cat "$err_file")
    rm -f "$out_file" "$err_file"
    if [ "$me" -ne 0 ] \
        && printf '%s' "$err" | grep -qiF "breakpoint" \
        && ! printf '%s' "$out" | grep -qF "FAIL"; then
        echo "[PASS] ${name}"
    else
        FAIL_COUNT=$((FAIL_COUNT + 1))
        echo "[FAIL] ${name}"
        echo "        expected: nonzero exit, 'breakpoint' on stderr, no fall-through marker on stdout"
        echo "        actual:   exit ${me}; stdout=\"${out}\"; stderr=\"${err}\""
    fi
}

run_brk_trap_test

# --- maize-21: IN / OUT / OUTR are privileged (cause-4 fault in user mode) -------------
# test_priv_fault.mazm drops to user mode via the IRET trampoline, executes an IN, and
# takes the cause-4 privileged-operation fault. With no handler installed this keeps the
# frozen throw-and-exit behavior, so (like the breakpoint trap) it cannot be expressed
# with the generic run_test. This bespoke runner asserts the VM exits nonzero, surfaces a
# "privileg" diagnostic on stderr, and never reaches the fall-through "priv: FAIL" marker.
run_priv_fault_trap_test() {
    name="priv_fault_trap"
    TOTAL=$((TOTAL + 1))
    src="test_priv_fault.mazm"
    cp "${ASM_DIR}/${src}" "${TEST_RUN_DIR}/${src}"
    asm_path="${TEST_RUN_DIR}/${src}"
    bin_path="${asm_path%.mazm}.mzb"
    if ! "$MAZM_EXE" "$asm_path" >/dev/null 2>&1 || [ ! -f "$bin_path" ]; then
        FAIL_COUNT=$((FAIL_COUNT + 1))
        echo "[FAIL] ${name} (mazm failed to assemble)"
        return
    fi
    out_file=$(mktemp)
    err_file=$(mktemp)
    if "$MAIZE_EXE" "$bin_path" >"$out_file" 2>"$err_file"; then
        me=0
    else
        me=$?
    fi
    out=$(cat "$out_file")
    err=$(cat "$err_file")
    rm -f "$out_file" "$err_file"
    if [ "$me" -ne 0 ] \
        && printf '%s' "$err" | grep -qiF "privileg" \
        && ! printf '%s' "$out" | grep -qF "FAIL"; then
        echo "[PASS] ${name}"
    else
        FAIL_COUNT=$((FAIL_COUNT + 1))
        echo "[FAIL] ${name}"
        echo "        expected: nonzero exit, 'privileg' on stderr, no fall-through marker on stdout"
        echo "        actual:   exit ${me}; stdout=\"${out}\"; stderr=\"${err}\""
    fi
}

run_priv_fault_trap_test

# --- maize-180: the four new privileged instructions + the previously-ungated ops -------
# Each fixture drops to user mode via the IRET trampoline and executes one privileged
# instruction (MOVTCR / TLBINV, or the previously-ungated HALT / SETINT / SETSYSG / a
# forged-RF IRET escalation attempt). With the head-of-dispatch privilege guard the VM
# takes the cause-4 fault: exits nonzero, surfaces a "privileg" diagnostic on stderr, and
# never reaches the fall-through / escalation-target "FAIL" marker. The IRET-escalation
# case additionally proves the forged supervisor target is never reached. Wrapped in
# `timeout` so a regression that (e.g.) leaves interrupts enabled and parks in HALT is
# reported as a failure rather than hanging the suite.
run_priv_user_fault_test() {
    name="$1"
    src="$2"
    TOTAL=$((TOTAL + 1))
    cp "${ASM_DIR}/${src}" "${TEST_RUN_DIR}/${src}"
    asm_path="${TEST_RUN_DIR}/${src}"
    bin_path="${asm_path%.mazm}.mzb"
    if ! "$MAZM_EXE" "$asm_path" >/dev/null 2>&1 || [ ! -f "$bin_path" ]; then
        FAIL_COUNT=$((FAIL_COUNT + 1))
        echo "[FAIL] ${name} (mazm failed to assemble)"
        return
    fi
    out_file=$(mktemp)
    err_file=$(mktemp)
    if timeout 10 "$MAIZE_EXE" "$bin_path" >"$out_file" 2>"$err_file"; then
        me=0
    else
        me=$?
    fi
    out=$(cat "$out_file")
    err=$(cat "$err_file")
    rm -f "$out_file" "$err_file"
    if [ "$me" -ne 0 ] \
        && printf '%s' "$err" | grep -qiF "privileg" \
        && ! printf '%s' "$out" | grep -qF "FAIL"; then
        echo "[PASS] ${name}"
    else
        FAIL_COUNT=$((FAIL_COUNT + 1))
        echo "[FAIL] ${name}"
        echo "        expected: nonzero exit, 'privileg' on stderr, no FAIL marker on stdout"
        echo "        actual:   exit ${me}; stdout=\"${out}\"; stderr=\"${err}\""
    fi
}

run_priv_user_fault_test "mmu_priv_movtcr"        "test_mmu_priv_movtcr.mazm"
run_priv_user_fault_test "mmu_priv_tlbinv"        "test_mmu_priv_tlbinv.mazm"
run_priv_user_fault_test "mmu_priv_iret_escalate" "test_mmu_priv_iret_escalation.mazm"
run_priv_user_fault_test "mmu_priv_halt"          "test_mmu_priv_halt.mazm"
run_priv_user_fault_test "mmu_priv_setint"        "test_mmu_priv_setint.mazm"
run_priv_user_fault_test "mmu_priv_setsysg"       "test_mmu_priv_setsysg.mazm"
run_priv_user_fault_test "mmu_priv_rf_write"      "test_mmu_priv_rf_write.mazm"

# --- maize-180: mzdis decodes the four new instructions (six encodings) ----------------
# A code-only fixture exercising $26/$66/$A6 (MOVTCR/MOVFCR) and $28/$68 (TLBINV/TLBINVA):
# assemble, disassemble, assert every new mnemonic decoded (no unknown/malformed lines),
# then reassemble mzdis's own output and diff the .mzb byte-for-byte (the four-surface
# sync check, same shape as run_mzdis_roundtrip_test).
run_mmu_mzdis_test() {
    name="mmu_mzdis_roundtrip"
    TOTAL=$((TOTAL + 1))
    asm_path="${TEST_RUN_DIR}/test_mmu_mzdis.mazm"
    cp "${ASM_DIR}/test_mmu_mzdis.mazm" "$asm_path"
    bin_path="${asm_path%.mazm}.mzb"
    if ! "$MAZM_EXE" "$asm_path" >/dev/null 2>&1 || [ ! -f "$bin_path" ]; then
        FAIL_COUNT=$((FAIL_COUNT + 1))
        echo "[FAIL] ${name} (mazm failed to assemble)"
        return
    fi
    dis_path="${TEST_RUN_DIR}/test_mmu_mzdis.dis.mazm"
    if ! "$MZDIS_EXE" -o "$dis_path" "$bin_path"; then
        FAIL_COUNT=$((FAIL_COUNT + 1))
        echo "[FAIL] ${name} (mzdis exited nonzero)"
        return
    fi
    if grep -qiE 'unknown opcode|malformed|TRUNCATED' "$dis_path"; then
        FAIL_COUNT=$((FAIL_COUNT + 1))
        echo "[FAIL] ${name} (decode diagnostic in a code-only fixture)"
        return
    fi
    if ! grep -qE '^\s+MOVTCR R5 \$00\b' "$dis_path" \
        || ! grep -qE '^\s+MOVTCR \$ABCD \$01\b' "$dis_path" \
        || ! grep -qE '^\s+MOVFCR \$02 R6\b' "$dis_path" \
        || ! grep -qE '^\s+TLBINV\b' "$dis_path" \
        || ! grep -qE '^\s+TLBINVA R7\b' "$dis_path"; then
        FAIL_COUNT=$((FAIL_COUNT + 1))
        echo "[FAIL] ${name}"
        echo "        expected: all six new encodings decode to their mnemonics"
        echo "        actual:   one or more missing; see ${dis_path}"
        return
    fi
    reasm_bin="${dis_path%.mazm}.mzb"
    if ! "$MAZM_EXE" "$dis_path" >/dev/null 2>&1 || [ ! -f "$reasm_bin" ]; then
        FAIL_COUNT=$((FAIL_COUNT + 1))
        echo "[FAIL] ${name} (mzdis output failed to reassemble)"
        return
    fi
    if cmp -s "$bin_path" "$reasm_bin"; then
        echo "[PASS] ${name}"
    else
        FAIL_COUNT=$((FAIL_COUNT + 1))
        echo "[FAIL] ${name} (reassembled .mzb not byte-identical)"
    fi
}

run_mmu_mzdis_test

# --- maize-21: a period-1 timer must not lose an IRQ raised during the masked handler --
# A period-1 periodic timer raises on every tick, including inside the masked handler
# window (the post-ack POP/IRET instructions each tick it). Delivery must gate on the
# durable irq_pending latch, not the RF interrupt-set mirror (which IRET restores clear),
# or the guest services exactly one tick and then livelocks. The handler drives
# termination (prints on the Nth tick), so this is an exact-stdout check BOUNDED by a
# timeout: a lost-IRQ regression trips the timeout (exit 124) instead of hanging the suite.
run_timer_period1_test() {
    name="timer_period1"
    expected="timerp1: PASS"
    TOTAL=$((TOTAL + 1))
    src="test_timer_period1.mazm"
    cp "${ASM_DIR}/${src}" "${TEST_RUN_DIR}/${src}"
    asm_path="${TEST_RUN_DIR}/${src}"
    bin_path="${asm_path%.mazm}.mzb"
    if ! "$MAZM_EXE" "$asm_path" >/dev/null 2>&1 || [ ! -f "$bin_path" ]; then
        FAIL_COUNT=$((FAIL_COUNT + 1))
        echo "[FAIL] ${name} (mazm failed to assemble)"
        return
    fi
    out_file=$(mktemp)
    if timeout 10 "$MAIZE_EXE" "$bin_path" >"$out_file" 2>/dev/null; then
        me=0
    else
        me=$?
    fi
    actual=$(cat "$out_file")
    rm -f "$out_file"
    if [ "$me" -eq 0 ] && [ "$actual" = "$expected" ]; then
        echo "[PASS] ${name}"
    else
        FAIL_COUNT=$((FAIL_COUNT + 1))
        echo "[FAIL] ${name}"
        echo "        expected: \"${expected}\" (exit 0, no livelock)"
        echo "        actual:   \"${actual}\" (exit ${me}; 124 = timed out = lost-IRQ livelock)"
    fi
}

run_timer_period1_test

# --- maize-75: sys_read byte-count fix (needs a known stdin) --------------------------
# The generic run_test gives no stdin, so this bespoke runner pipes "hello" and
# checks the program echoes exactly the bytes read plus an EOF marker: "hello|EOF".
# The old fall-through-to-0 bug (and any short-read tail spill or nonzero EOF
# return) produces different output, so a byte-exact match gates the fix.
run_sysread_test() {
    name="sysread_count"
    expected="hello|EOF"
    TOTAL=$((TOTAL + 1))
    src="test_sysread.mazm"
    cp "${ASM_DIR}/${src}" "${TEST_RUN_DIR}/${src}"
    asm_path="${TEST_RUN_DIR}/${src}"
    bin_path="${asm_path%.mazm}.mzb"
    if ! "$MAZM_EXE" "$asm_path" >/dev/null 2>&1 || [ ! -f "$bin_path" ]; then
        FAIL_COUNT=$((FAIL_COUNT + 1))
        echo "[FAIL] ${name} (mazm failed to assemble)"
        return
    fi
    if out=$(printf 'hello' | "$MAIZE_EXE" "$bin_path" 2>/dev/null); then
        me=0
    else
        me=$?
    fi
    if [ "$me" -eq 0 ] && [ "$out" = "$expected" ]; then
        echo "[PASS] ${name}"
    else
        FAIL_COUNT=$((FAIL_COUNT + 1))
        echo "[FAIL] ${name}"
        echo "        expected: \"${expected}\""
        echo "        actual:   \"${out}\" (exit ${me})"
    fi
}

run_sysread_test

# --- keyboard device: injected-scancode round trip (needs a known piped stdin) --------
# The generic run_test gives no stdin, so this bespoke runner pipes a known four-byte
# Set-1 scancode sequence (make codes for A, B, C, D) and runs maize with
# --input=keyboard so the keyboard is the sole stdin consumer. The guest installs an
# IRQ-34 handler, collects the four scancodes, verifies them, and prints "keyboard: PASS".
run_keyboard_test() {
    name="keyboard"
    expected="keyboard: PASS"
    TOTAL=$((TOTAL + 1))
    src="test_keyboard.mazm"
    cp "${ASM_DIR}/${src}" "${TEST_RUN_DIR}/${src}"
    asm_path="${TEST_RUN_DIR}/${src}"
    bin_path="${asm_path%.mazm}.mzb"
    if ! "$MAZM_EXE" "$asm_path" >/dev/null 2>&1 || [ ! -f "$bin_path" ]; then
        FAIL_COUNT=$((FAIL_COUNT + 1))
        echo "[FAIL] ${name} (mazm failed to assemble)"
        return
    fi
    # Inject scancodes $1E $30 $2E $20 (octal 036 060 056 040).
    if out=$(printf '\036\060\056\040' | "$MAIZE_EXE" --input=keyboard "$bin_path" 2>/dev/null); then
        me=0
    else
        me=$?
    fi
    if [ "$me" -eq 0 ] && [ "$out" = "$expected" ]; then
        echo "[PASS] ${name}"
    else
        FAIL_COUNT=$((FAIL_COUNT + 1))
        echo "[FAIL] ${name}"
        echo "        expected: \"${expected}\""
        echo "        actual:   \"${out}\" (exit ${me})"
    fi
}

run_keyboard_test

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

# --- maize-89: single-object assemble -> link -> run for DREF / ALIGN ----------------
# Each fixture assembles with -c, links to a .mzx, and runs under maize; the program
# proves at runtime that DREF references resolve to a symbol's linked address (plus a
# signed addend) and that a datum after ALIGN lands on the aligned boundary.
run_obj_pipeline_test() {
    name="$1"
    src="$2"
    expected="$3"
    TOTAL=$((TOTAL + 1))
    emit_object "$src"
    obj="${TEST_RUN_DIR}/${src%.mazm}.mzo"
    mzx="${TEST_RUN_DIR}/${src%.mazm}.mzx"
    if [ ! -f "$obj" ]; then
        FAIL_COUNT=$((FAIL_COUNT + 1))
        echo "[FAIL] ${name} (mazm -c produced no .mzo)"
        return
    fi
    log=$(mktemp)
    if ! "$MZLD_EXE" -o "$mzx" "$obj" >"$log" 2>&1; then
        FAIL_COUNT=$((FAIL_COUNT + 1))
        echo "[FAIL] ${name} (link failed)"
        cat "$log"
        rm -f "$log"
        return
    fi
    rm -f "$log"
    out=$(mktemp)
    if "$MAIZE_EXE" "$mzx" >"$out" 2>/dev/null; then
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

# Object-mode reject: mazm -c must exit nonzero with a diagnostic containing $expected.
run_obj_reject_test() {
    name="$1"
    src="$2"
    expected="$3"
    TOTAL=$((TOTAL + 1))
    cp "${ASM_DIR}/${src}" "${TEST_RUN_DIR}/${src}"
    log=$(mktemp)
    if "$MAZM_EXE" -c "${TEST_RUN_DIR}/${src}" >"$log" 2>&1; then
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

# maize-95: a BACKWARD local-label JMP must be relocated in object mode. The main
# fixture is linked AFTER a leading pad object so mzld gives it a nonzero vaddr; if
# regimm_compiler emits no relocation for the backward JMP the jump mis-targets (or
# hangs) and the PASS line never prints. The maize run is wrapped in `timeout` so a
# mis-target that spins cannot wedge the suite; a timeout counts as a failure.
run_obj_backjmp_test() {
    name="obj_backjmp"
    expected="backjmp: PASS"
    TOTAL=$((TOTAL + 1))
    emit_object "test_obj_backjmp_pad.mazm"
    emit_object "test_obj_backjmp.mazm"
    pad="${TEST_RUN_DIR}/test_obj_backjmp_pad.mzo"
    main="${TEST_RUN_DIR}/test_obj_backjmp.mzo"
    mzx="${TEST_RUN_DIR}/test_obj_backjmp.mzx"
    if [ ! -f "$pad" ] || [ ! -f "$main" ]; then
        FAIL_COUNT=$((FAIL_COUNT + 1))
        echo "[FAIL] ${name} (mazm -c produced no .mzo)"
        return
    fi
    log=$(mktemp)
    # Pad object FIRST so the main object's CODE section lands at a nonzero vaddr.
    if ! "$MZLD_EXE" -o "$mzx" "$pad" "$main" >"$log" 2>&1; then
        FAIL_COUNT=$((FAIL_COUNT + 1))
        echo "[FAIL] ${name} (link failed)"
        cat "$log"
        rm -f "$log"
        return
    fi
    rm -f "$log"
    out=$(mktemp)
    if timeout 10 "$MAIZE_EXE" "$mzx" >"$out" 2>/dev/null; then
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
        echo "        expected: \"${expected}\" (exit 0)"
        echo "        actual:   \"${actual}\" (exit ${me})"
    fi
}

run_obj_pipeline_test "obj_dref"         "test_obj_dref.mazm"         "dref: PASS"
run_obj_pipeline_test "obj_dref_addend"  "test_obj_dref_addend.mazm"  "dref-addend: PASS"
run_obj_pipeline_test "obj_align"        "test_obj_align.mazm"        "align: PASS"
run_obj_reject_test   "obj_align_reject" "test_reject_align.mazm"     "power of two"
run_obj_backjmp_test

# --- maize-150: two-TU data relocations (label/symbol operands in ST + DREF) ----------
# TU A holds a static pointer table (DREF 8 local_target + DREF 8 ext_target, the
# latter NOT declared EXTERN -> the GAP 3 data-context auto-import) and an
# ST ext_target @Rn source (GAP 2). Both TUs assemble with -c, link into one .mzx,
# and run: the program compares each read-back pointer against the symbol's linked
# address and prints PASS only when the local reloc, the external auto-imported
# reloc, and the ST-stored address all match. Proves runtime correctness, not just
# "it assembled".
run_obj_datareloc_test() {
    name="obj_datareloc"
    expected="datareloc: PASS"
    TOTAL=$((TOTAL + 1))
    emit_object "test_obj_datareloc_a.mazm"
    emit_object "test_obj_datareloc_b.mazm"
    a="${TEST_RUN_DIR}/test_obj_datareloc_a.mzo"
    b="${TEST_RUN_DIR}/test_obj_datareloc_b.mzo"
    mzx="${TEST_RUN_DIR}/test_obj_datareloc.mzx"
    if [ ! -f "$a" ] || [ ! -f "$b" ]; then
        FAIL_COUNT=$((FAIL_COUNT + 1))
        echo "[FAIL] ${name} (mazm -c produced no .mzo)"
        return
    fi
    log=$(mktemp)
    if ! "$MZLD_EXE" -o "$mzx" "$a" "$b" >"$log" 2>&1; then
        FAIL_COUNT=$((FAIL_COUNT + 1))
        echo "[FAIL] ${name} (link failed)"
        cat "$log"
        rm -f "$log"
        return
    fi
    rm -f "$log"
    out=$(mktemp)
    if "$MAIZE_EXE" "$mzx" >"$out" 2>/dev/null; then
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
        echo "        expected: \"${expected}\" (exit 0)"
        echo "        actual:   \"${actual}\" (exit ${me})"
    fi
}

run_obj_datareloc_test

# --- maize-71: EXTERN / PUBLIC declared module interfaces ----------------------------
# --check accepts a fragment that declares EXTERN for its cross-module references
# (the editor must not squiggle a valid fragment), but still errors on an
# UNDECLARED undefined reference so typos remain diagnosable. expect_ok=1 means
# --check must exit 0; expect_ok=0 means it must exit nonzero with $expected.
run_check_test() {
    name="$1"
    src="$2"
    expect_ok="$3"
    expected="$4"
    TOTAL=$((TOTAL + 1))
    cp "${ASM_DIR}/${src}" "${TEST_RUN_DIR}/${src}"
    log=$(mktemp)
    if "$MAZM_EXE" --check "${TEST_RUN_DIR}/${src}" >"$log" 2>&1; then
        ec=0
    else
        ec=$?
    fi
    actual=$(cat "$log")
    rm -f "$log"
    if [ "$expect_ok" -eq 1 ]; then
        if [ "$ec" -eq 0 ]; then
            echo "[PASS] ${name}"
        else
            FAIL_COUNT=$((FAIL_COUNT + 1))
            echo "[FAIL] ${name} (expected --check to accept, exit 0)"
            echo "        actual: exit ${ec}: \"${actual}\""
        fi
    else
        if [ "$ec" -ne 0 ] && printf '%s' "$actual" | grep -qF "$expected"; then
            echo "[PASS] ${name}"
        else
            FAIL_COUNT=$((FAIL_COUNT + 1))
            echo "[FAIL] ${name} (expected --check reject: \"${expected}\")"
            echo "        actual: exit ${ec}: \"${actual}\""
        fi
    fi
}

# PUBLIC is a co-equal alias of GLOBAL: two fixtures that differ ONLY in the
# export directive keyword must assemble to byte-identical .mzo objects.
run_public_alias_test() {
    name="public_global_identical"
    TOTAL=$((TOTAL + 1))
    emit_object "test_export_global.mazm"
    emit_object "test_export_public.mazm"
    g="${TEST_RUN_DIR}/test_export_global.mzo"
    p="${TEST_RUN_DIR}/test_export_public.mzo"
    if [ ! -f "$g" ] || [ ! -f "$p" ]; then
        FAIL_COUNT=$((FAIL_COUNT + 1))
        echo "[FAIL] ${name} (mazm -c produced no .mzo)"
        return
    fi
    if cmp -s "$g" "$p"; then
        echo "[PASS] ${name}"
    else
        FAIL_COUNT=$((FAIL_COUNT + 1))
        echo "[FAIL] ${name} (GLOBAL and PUBLIC .mzo differ)"
    fi
}

# Object-mode strict reject: an undefined non-EXTERN reference under `mazm -c`
# exits nonzero, names the symbol, and suggests EXTERN.
run_obj_reject_test "obj_undeclared_ref" "test_reject_undeclared_obj.mazm" "undefined symbol 'mystery'"
# maize-150 instruction-path guard: an undeclared label used as a CP instruction
# SOURCE (routing through obj_emit_label_ref, NOT CALL/write_label and NOT a DREF)
# must still fatal. This pins the GAP 3 relaxation to data context: obj_data_refs
# is populated only at dref_compiler, so an undefined instruction-operand label is
# never importable. Without this fixture a future over-broad obj_data_refs populate
# would silently re-import an undefined CP/LD/ST source.
run_obj_reject_test "obj_undeclared_src" "test_reject_undeclared_obj_src.mazm" "undefined symbol 'undefinedtypo'"
# Flat-mode reject: an EXTERN'd-but-locally-undefined reference has no linker to
# resolve it, so flat assembly exits nonzero with `unresolved external`.
run_test "flat_unresolved_extern" "test_reject_unresolved_extern.mazm" "unresolved external 'undefsym'" 0 1
# Check-mode: EXTERN'd import accepted (exit 0); undeclared undefined reference
# still rejected with `undefined label`.
run_check_test "check_extern_ok"   "test_check_extern_ok.mazm"   1 ""
run_check_test "check_undeclared"  "test_check_undeclared.mazm"  0 "undefined label 'ghost'"
# PUBLIC / GLOBAL byte-identical export encoding.
run_public_alias_test

# --- maize-14: mzdis disassembler ---------------------------------------------------
# Round trip (AC6477/AC6478/AC6483): assemble a code-only, SECTION-clean fixture that
# hits every addressing-mode family and operand-count shape, disassemble it, reassemble
# mzdis's own output text, and diff the resulting .mzb against the original byte-for-
# byte -- the strongest test the spec names.
run_mzdis_roundtrip_test() {
    name="mzdis_roundtrip"
    TOTAL=$((TOTAL + 1))
    asm_path="${TEST_RUN_DIR}/test_mzdis_roundtrip.mazm"
    cp "${ASM_DIR}/test_mzdis_roundtrip.mazm" "$asm_path"
    bin_path="${asm_path%.mazm}.mzb"

    if ! "$MAZM_EXE" "$asm_path" >/dev/null 2>&1 || [ ! -f "$bin_path" ]; then
        FAIL_COUNT=$((FAIL_COUNT + 1))
        echo "[FAIL] ${name}"
        echo "        expected: fixture assembles cleanly"
        echo "        actual:   mazm failed to produce a .mzb"
        return
    fi

    dis_path="${TEST_RUN_DIR}/test_mzdis_roundtrip.dis.mazm"
    if ! "$MZDIS_EXE" -o "$dis_path" "$bin_path"; then
        FAIL_COUNT=$((FAIL_COUNT + 1))
        echo "[FAIL] ${name}"
        echo "        expected: mzdis exits 0 with clean decode"
        echo "        actual:   mzdis exited nonzero"
        return
    fi
    if grep -qiE 'unknown opcode|malformed|TRUNCATED' "$dis_path"; then
        FAIL_COUNT=$((FAIL_COUNT + 1))
        echo "[FAIL] ${name}"
        echo "        expected: no unknown/malformed/truncated lines"
        echo "        actual:   decode diagnostic found in a code-only fixture"
        return
    fi

    reasm_bin="${dis_path%.mazm}.mzb"
    if ! "$MAZM_EXE" "$dis_path" >/dev/null 2>&1 || [ ! -f "$reasm_bin" ]; then
        FAIL_COUNT=$((FAIL_COUNT + 1))
        echo "[FAIL] ${name}"
        echo "        expected: mzdis's own output reassembles cleanly"
        echo "        actual:   mazm failed to reassemble mzdis output"
        return
    fi

    if cmp -s "$bin_path" "$reasm_bin"; then
        echo "[PASS] ${name}"
    else
        FAIL_COUNT=$((FAIL_COUNT + 1))
        echo "[FAIL] ${name}"
        echo "        expected: reassembled .mzb byte-identical to original"
        echo "        actual:   content differs"
    fi
}

# Reserved-opcode resync + round-trip (AC6481, AC7278): two reserved bytes decode
# as DATA $XX / unknown opcode (D-DATA: DB is not a mazm keyword and would break
# round-trip; DATA $XX reassembles to the same byte), advancing exactly one byte
# each, decoding resumes correctly afterward, and mzdis's own output reassembles
# back to the original .mzb byte-for-byte.
run_mzdis_reserved_test() {
    name="mzdis_reserved_resync"
    TOTAL=$((TOTAL + 1))
    asm_path="${TEST_RUN_DIR}/test_mzdis_reserved.mazm"
    cp "${ASM_DIR}/test_mzdis_reserved.mazm" "$asm_path"
    bin_path="${asm_path%.mazm}.mzb"

    if ! "$MAZM_EXE" "$asm_path" >/dev/null 2>&1 || [ ! -f "$bin_path" ]; then
        FAIL_COUNT=$((FAIL_COUNT + 1))
        echo "[FAIL] ${name}"
        echo "        expected: fixture assembles cleanly"
        echo "        actual:   mazm failed to produce a .mzb"
        return
    fi

    dis_path="${TEST_RUN_DIR}/test_mzdis_reserved.dis.mazm"
    # `|| dis_exit=$?` keeps a nonzero exit from tripping set -e; a bare
    # `cmd; dis_exit=$?` would kill the whole script before the capture.
    dis_exit=0
    "$MZDIS_EXE" -o "$dis_path" "$bin_path" || dis_exit=$?

    if [ "$dis_exit" -ne 0 ] \
        || ! grep -qE 'DATA \$37.*unknown opcode' "$dis_path" \
        || ! grep -qE 'DATA \$38.*unknown opcode' "$dis_path" \
        || ! grep -qE '^\s+NOP\b' "$dis_path" \
        || grep -qi 'TRUNCATED' "$dis_path"; then
        FAIL_COUNT=$((FAIL_COUNT + 1))
        echo "[FAIL] ${name}"
        echo "        expected: DATA \$37/\$38 unknown-opcode lines, NOP decodes correctly after, exit 0"
        echo "        actual:   exit ${dis_exit}; see ${dis_path}"
        return
    fi

    reasm_bin="${dis_path%.mazm}.mzb"
    if ! "$MAZM_EXE" "$dis_path" >/dev/null 2>&1 || [ ! -f "$reasm_bin" ]; then
        FAIL_COUNT=$((FAIL_COUNT + 1))
        echo "[FAIL] ${name}"
        echo "        expected: mzdis's own output reassembles cleanly"
        echo "        actual:   mazm failed to reassemble mzdis output"
        return
    fi

    if cmp -s "$bin_path" "$reasm_bin"; then
        echo "[PASS] ${name}"
    else
        FAIL_COUNT=$((FAIL_COUNT + 1))
        echo "[FAIL] ${name}"
        echo "        expected: reassembled .mzb byte-identical to original"
        echo "        actual:   content differs"
    fi
}

# Symbolic round trip (AC7275, AC7276): a code-only fixture whose in-image
# 32-bit CALL/JMP/Jcc targets (forward and backward) all qualify for label
# synthesis. Asserts symbolization actually fired (fn_/loc_ declaration lines
# AND symbolic operands present -- not a silent fallback to literals), then
# reassembles mzdis's own output and diffs the resulting .mzb against the
# original byte-for-byte, same as run_mzdis_roundtrip_test.
run_mzdis_rt_symbolic_test() {
    name="mzdis_rt_symbolic"
    TOTAL=$((TOTAL + 1))
    asm_path="${TEST_RUN_DIR}/test_mzdis_rt_symbolic.mazm"
    cp "${ASM_DIR}/test_mzdis_rt_symbolic.mazm" "$asm_path"
    bin_path="${asm_path%.mazm}.mzb"

    if ! "$MAZM_EXE" "$asm_path" >/dev/null 2>&1 || [ ! -f "$bin_path" ]; then
        FAIL_COUNT=$((FAIL_COUNT + 1))
        echo "[FAIL] ${name}"
        echo "        expected: fixture assembles cleanly"
        echo "        actual:   mazm failed to produce a .mzb"
        return
    fi

    dis_path="${TEST_RUN_DIR}/test_mzdis_rt_symbolic.dis.mazm"
    if ! "$MZDIS_EXE" -o "$dis_path" "$bin_path"; then
        FAIL_COUNT=$((FAIL_COUNT + 1))
        echo "[FAIL] ${name}"
        echo "        expected: mzdis exits 0 with clean decode"
        echo "        actual:   mzdis exited nonzero"
        return
    fi
    if grep -qiE 'unknown opcode|malformed|TRUNCATED' "$dis_path"; then
        FAIL_COUNT=$((FAIL_COUNT + 1))
        echo "[FAIL] ${name}"
        echo "        expected: no unknown/malformed/truncated lines (code-only fixture)"
        echo "        actual:   decode diagnostic found"
        return
    fi

    if ! grep -qE '^fn_[0-9a-f]+:' "$dis_path" \
        || ! grep -qE '^loc_[0-9a-f]+:' "$dis_path" \
        || ! grep -qE 'CALL fn_[0-9a-f]+' "$dis_path" \
        || ! grep -qE '(JMP|JNZ) loc_[0-9a-f]+' "$dis_path"; then
        FAIL_COUNT=$((FAIL_COUNT + 1))
        echo "[FAIL] ${name}"
        echo "        expected: synthesized fn_/loc_ declarations AND symbolic operands (symbolization fired)"
        echo "        actual:   missing one or more; see ${dis_path}"
        return
    fi

    reasm_bin="${dis_path%.mazm}.mzb"
    if ! "$MAZM_EXE" "$dis_path" >/dev/null 2>&1 || [ ! -f "$reasm_bin" ]; then
        FAIL_COUNT=$((FAIL_COUNT + 1))
        echo "[FAIL] ${name}"
        echo "        expected: mzdis's own output reassembles cleanly"
        echo "        actual:   mazm failed to reassemble mzdis output"
        return
    fi

    if cmp -s "$bin_path" "$reasm_bin"; then
        echo "[PASS] ${name}"
    else
        FAIL_COUNT=$((FAIL_COUNT + 1))
        echo "[FAIL] ${name}"
        echo "        expected: reassembled .mzb byte-identical to original"
        echo "        actual:   content differs"
    fi
}

# Truncated tail (AC6482): chop the assembled fixture mid-immediate so the final
# instruction's declared bytes run past EOF. mzdis must still emit everything decoded
# before the cut, a trailing "; TRUNCATED ..." line, and exit 1.
run_mzdis_truncated_test() {
    name="mzdis_truncated_tail"
    TOTAL=$((TOTAL + 1))
    asm_path="${TEST_RUN_DIR}/test_mzdis_truncate_src.mazm"
    cp "${ASM_DIR}/test_mzdis_truncate_src.mazm" "$asm_path"
    bin_path="${asm_path%.mazm}.mzb"

    if ! "$MAZM_EXE" "$asm_path" >/dev/null 2>&1 || [ ! -f "$bin_path" ]; then
        FAIL_COUNT=$((FAIL_COUNT + 1))
        echo "[FAIL] ${name}"
        echo "        expected: fixture assembles cleanly"
        echo "        actual:   mazm failed to produce a .mzb"
        return
    fi

    # HALT(1) + CLR R0(2) + CP \$12345678 R0 (opcode+param+4-byte imm = 6) = 9 real
    # bytes; keep only the first 8, cutting the immediate 2 bytes short.
    trunc_path="${TEST_RUN_DIR}/test_mzdis_truncate.mzb"
    dd if="$bin_path" of="$trunc_path" bs=1 count=8 >/dev/null 2>&1

    out_file="${TEST_RUN_DIR}/test_mzdis_truncate.out"
    dis_exit=0
    "$MZDIS_EXE" "$trunc_path" >"$out_file" || dis_exit=$?

    if [ "$dis_exit" -eq 1 ] \
        && grep -qE '^\s+HALT\b' "$out_file" \
        && grep -qE '^\s+CLR R0\b' "$out_file" \
        && grep -qi 'TRUNCATED' "$out_file"; then
        echo "[PASS] ${name}"
    else
        FAIL_COUNT=$((FAIL_COUNT + 1))
        echo "[FAIL] ${name}"
        echo "        expected: partial output (HALT, CLR R0) + TRUNCATED diagnostic, exit 1"
        echo "        actual:   exit ${dis_exit}; see ${out_file}"
    fi
}

# .mzo rejection (AC6480): exit 1, a diagnostic naming the file, and no stdout output.
run_mzdis_mzo_reject_test() {
    name="mzdis_mzo_reject"
    TOTAL=$((TOTAL + 1))
    mzo_path="${TEST_RUN_DIR}/link_a.mzo"
    if [ ! -f "$mzo_path" ]; then
        FAIL_COUNT=$((FAIL_COUNT + 1))
        echo "[FAIL] ${name}"
        echo "        expected: link_a.mzo present from the link tests"
        echo "        actual:   missing (link tests must run first)"
        return
    fi

    out_file="${TEST_RUN_DIR}/test_mzdis_mzo.out"
    err_file="${TEST_RUN_DIR}/test_mzdis_mzo.err"
    dis_exit=0
    "$MZDIS_EXE" "$mzo_path" >"$out_file" 2>"$err_file" || dis_exit=$?

    if [ "$dis_exit" -eq 1 ] \
        && [ ! -s "$out_file" ] \
        && grep -qF '.mzo relocatable object' "$err_file" \
        && grep -qF "$(basename "$mzo_path")" "$err_file"; then
        echo "[PASS] ${name}"
    else
        FAIL_COUNT=$((FAIL_COUNT + 1))
        echo "[FAIL] ${name}"
        echo "        expected: exit 1, no stdout, stderr names the .mzo file"
        echo "        actual:   exit ${dis_exit}; see ${out_file} / ${err_file}"
    fi
}

# .mzx segment routing (AC6479): CODE decodes as instructions at vaddr (with an ENTRY
# annotation), RODATA renders as DATA lines, never passed through the instruction
# decoder.
run_mzdis_mzx_test() {
    name="mzdis_mzx_segments"
    TOTAL=$((TOTAL + 1))
    mzx_path="${TEST_RUN_DIR}/link.mzx"
    if [ ! -f "$mzx_path" ]; then
        FAIL_COUNT=$((FAIL_COUNT + 1))
        echo "[FAIL] ${name}"
        echo "        expected: link.mzx present from the link tests"
        echo "        actual:   missing (link tests must run first)"
        return
    fi

    out_file="${TEST_RUN_DIR}/test_mzdis_mzx.out"
    dis_exit=0
    "$MZDIS_EXE" "$mzx_path" >"$out_file" || dis_exit=$?

    if [ "$dis_exit" -eq 0 ] \
        && grep -qE '^\s+CALL\b.*ENTRY' "$out_file" \
        && grep -qE '^\s+RET\b' "$out_file" \
        && grep -q 'RODATA' "$out_file" \
        && grep -qF 'DATA $4C $69 $6E $6B $65 $64 $21' "$out_file"; then
        echo "[PASS] ${name}"
    else
        FAIL_COUNT=$((FAIL_COUNT + 1))
        echo "[FAIL] ${name}"
        echo "        expected: CODE decoded with ENTRY annotation, RODATA rendered as DATA \$4C ... (\"Linked!\")"
        echo "        actual:   exit ${dis_exit}; see ${out_file}"
    fi
}

run_mzdis_roundtrip_test
run_mzdis_rt_symbolic_test
run_mzdis_reserved_test
run_mzdis_truncated_test
run_mzdis_mzo_reject_test
run_mzdis_mzx_test

PASS_COUNT=$((TOTAL - FAIL_COUNT))
echo ""
echo "${PASS_COUNT} passed, ${FAIL_COUNT} failed (${TOTAL} total)"

if [ "$FAIL_COUNT" -gt 0 ]; then
    exit 1
fi
exit 0
