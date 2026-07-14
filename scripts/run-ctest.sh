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

    mzx="${WORK_DIR}/${name}.mzx"
    if ! "$CC_MAIZE" --preset "$PRESET" -o "$mzx" "$src" \
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
run_driver_run_mode_test() {
    name="driver_run"
    TOTAL=$((TOTAL + 1))

    stray="${CTEST_DIR}/exitcode.mzx"
    rm -f "$stray"

    set +e
    "$CC_MAIZE" --preset "$PRESET" -r "${CTEST_DIR}/exitcode.c" >/dev/null 2>"${WORK_DIR}/dr.err"
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
    printf '%s\n' "${CTEST_DIR}/multifile_main.c" > "$listfile"
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
# maize-136 spilled-operand regression: >11 simultaneously-live values force QBE to
# spill to frame slots, and a loop rotating sixteen loop-carried values drives the
# block-edge slot->slot Ocopy (the PUSH/POP register borrow). Pre-fix the emitter
# die()d on any spilled operand; post-fix it emits the reload / spill-store / slot
# copy paths. Overlay-only fix in qbe-maize/emit.c. Self-checks against 1541762618.
run_ctest "spill"
# maize-143 CAddr nonzero-offset regression: forms and uses &global_array[K],
# &s.field, "lit"+K, and a &global[K] carried across a fused-branch loop, each with
# a checked result folded into "caddroff: PASS". Pre-fix the emitter die()d on any
# nonzero-offset CAddr con; post-fix isel routes it through a register and emitcopy
# lowers it as CP <label> ; LEA $<off> (flag-neutral). Overlay-only in qbe-maize.
run_ctest "caddroff"
# maize-143 flag-safety gate (QBE-IR level; see run_qbe_flag above): the LEA offset
# lowering must be flag-neutral so a $sym+K materialization landing between a fused
# CMP and its Jcc cannot corrupt the branch. Fails if LEA is swapped for ADD/SUB.
run_qbe_flag
# maize-137 float/double codegen: a self-checking fixture exercising float and
# double arithmetic (+ - * /), all six comparisons in both widths (ordered and
# NaN/unordered), signed int<->float and float<->double conversions (unsigned
# int<->float is out of scope), inline float/double constants, and
# passing/returning float and double across a call boundary. Each sub-result is
# checked (value or exact IEEE bits) so a wrong FP encoding fails the gate.
run_ctest "fp"
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
# maize-146 freestanding headers: fixed-width types + limit/constant macros + bool,
# and (precautionary) the inttypes PRI* format macros over the Maize printf.
run_ctest "stdint"
# maize-147 RT headers round 2 for DOOM: includes every new header (strings/math/
# assert/unistd/sys/types/sys/stat), asserts the SEEK_*/EISDIR/S_IF* macro values and
# the off_t/ssize_t/mode_t widths, proves the struct stat byte-ABI (sizeof 144;
# nlink@16/mode@24/size@48 via runtime pointer subtraction), and parses each new decl
# via sizeof(&fn) with NO link dependency (bodies are maize-148). One "rthdrs2: PASS".
run_ctest "rthdrs2"
# maize-149 GNU-attribute strip: a DOOM mapsidedef_t-shaped struct using the
# TRAILING __attribute__((packed)) position (which the pinned cproc rejects)
# compiles through the driver's cpp-step strip, and its sizeof/offsetof asserts
# (sizeof==30, char[8] blocks at 4/12/20, trailing short at 28) prove the natural
# layout is byte-identical to the packed on-disk WAD layout, so the strip is
# run-safe. Prints a single "packed: PASS" line.
run_ctest "packed"
# maize-100 atexit registry: two handlers registered A-then-B run at exit in LIFO
# order (B, then A) after "main done", proving both that exit() runs the registry
# and the ordering, plus the indirect-call-through-a-runtime-indexed-fnptr-array path.
run_ctest "atexit"
# maize-142 stdlib numeric conversions: atoi/abs/labs/strtol in one self-checking
# fixture (base 10/16/0-autodetect, overflow clamp + ERANGE, endptr/no-conversion,
# invalid-base EINVAL, and the bare-"0x"/"0"-no-digit corners). One "strtol PASS".
run_ctest "strtol"
# maize-141 monotonic ms clock (SYS $F0): a self-checking fixture asserting the
# clock is non-decreasing at fine grain, advances under a bounded busy-spin, and
# reports a plausible (nonzero, < 60 s) delta. Prints a single "clock: PASS" line.
run_ctest "clock"
# maize-98 varargs / stdarg ABI: a self-checking fixture exercising the register
# save area, va_arg over mixed scalar classes, the register->overflow boundary,
# and va_copy. Prints a single PASS line.
run_ctest "varargs"
# maize-99 variadic printf over the stdarg ABI: direct-emit correctness for every
# conversion (%d %i %u %x %X %c %s %p %%, %ld/%lu/%lx, width + zero-pad, INT_MIN /
# LONG_MIN) matched byte-for-byte, plus an snprintf return/truncation self-check
# and a >256-byte line proving chunked flush. Ends in a single "selfcheck PASS".
run_ctest "printf"
# maize-144 RT libc gaps for the DOOM boot: printf/sprintf PRECISION (%.Nd min-digits
# incl. the DOOM STCFN%.3d lump shape, %.Ns string truncation, %8.3d width+precision,
# %.0d-of-0 empty, and the untouched %05d path) plus strdup / getenv / qsort / atof,
# all checked silently with inline-computed expected values. One "libcgaps PASS".
run_ctest "libcgaps"
# maize-148 RT libc round 3 for the DOOM Phase A link: strcasecmp/strncasecmp (tolower),
# fabs via a sign-bit mask (incl. -0.0 -> +0.0 by bit pattern), the sscanf scanf core
# (%d/%x/%f/%s/%c/width/suppress with checked counts + values, a partial match), system
# (-1/0), usleep (no-op), and the remove/mkdir link-only stubs (execute smoke, no value
# assertion; real filesystem ACs are on maize-151). One "libcgaps3 PASS".
run_ctest "libcgaps3"
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
# maize-111 CLI-rework self-checks: the new no-run default (produce beside source) and
# the driver -r run-and-propagate path.
run_default_produce_test
run_driver_run_mode_test

# maize-114 hostfs acceptance scenarios (cat + ls on both hosts, ..-escape and
# symlink-escape EACCES/ENOENT, :ro write EROFS).
run_hostfs_cat
run_hostfs_ls
run_hostfs_stat
run_hostfs_escape
run_hostfs_rofs
# maize-120 FILE* stdio + dirent layer over the hostfs stubs.
run_hostfs_stdio
# maize-151 path-mutating syscalls (mkdir/unlink/rename) over the confined hostfs.
run_hostfs_savefs
run_hostfs_savefs_neg

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

run_terminal_selfcheck

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

run_doom_link

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

run_doom_selfcheck

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

run_doom_render

# maize-138 multi-file compile/link: the primary-gate cross-object fixture, the
# negative link-rejection case, and the two multi-source usage-error paths.
run_multi_ctest "multifile" "multifile_main.c multifile_lib.c"
run_multi_link_reject_test
run_multi_usage_test "multifile_no_out" "needs an output path" \
    "${CTEST_DIR}/multifile_main.c" "${CTEST_DIR}/multifile_lib.c"
run_multi_usage_test "multifile_emit_reject" "only when compiling a single" \
    --emit -o "${WORK_DIR}/multifile_emit_reject.mzx" \
    "${CTEST_DIR}/multifile_main.c" "${CTEST_DIR}/multifile_lib.c"

echo "-----------------------------------------------------------------------"
if [ "$FAIL_COUNT" -eq 0 ]; then
    echo "C toolchain: ${TOTAL} passed, 0 failed."
    exit 0
else
    echo "C toolchain: $((TOTAL - FAIL_COUNT)) passed, ${FAIL_COUNT} failed."
    exit 1
fi
