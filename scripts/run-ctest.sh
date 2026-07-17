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
MAIZE=$(resolve_exe "${BUILD_DIR}/maize") || {   # maize-225/230: SDL-free console build (no WSLg window)
    echo "run-ctest.sh: maize (console build) not found in ${BUILD_DIR}; run scripts/run-tests.sh first." >&2; exit 2; }
MZLD=$(resolve_exe "${BUILD_DIR}/mzld") || {
    echo "run-ctest.sh: mzld not found in ${BUILD_DIR}; run scripts/run-tests.sh first." >&2; exit 2; }

# maize-221: non-interactive stdin for every test child, so the console VM's
# framebuffer-takeover trap (interactive-tty only) never fires on the headless
# doom self-checks regardless of how this script is launched. See run-tests.sh.
exec 0</dev/null

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
run_ctest "ldzfold"
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
# maize-216 large-n bulk memory: memcpy/memmove/memset at/over BULK_SYSCALL_THRESHOLD
# route to the host via SYS $F4 (sys_bulk_copy, memmove-safe) / $F5 (sys_bulk_set).
# str.c only exercises the sub-threshold inline word loop; this drives the syscall
# path (aligned/unaligned, both overlap directions, threshold boundary, n==0) and
# self-checks byte-for-byte. One "bulkmem PASS".
run_ctest "bulkmem"
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

# maize-213 palette-blit syscall (SYS $F3): a self-checking fixture proving the
# blit is bit-identical (dst[i] == lut[src[i]], RV == npixels) AND deny-by-default
# secure (oversized npixels -> -EINVAL, a dst/src base+len wrap -> -EFAULT, each
# with no guest write and no crash). Prints a single "palette-blit: PASS" line.
run_ctest "palette_blit_selfcheck"
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
# maize-179 ftruncate over the confined hostfs (shrink/extend/EINVAL/EROFS).
run_hostfs_truncate

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

run_console_selfcheck

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

run_doom_transition

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

run_doom_input

# maize-138 multi-file compile/link: the primary-gate cross-object fixture, the
# negative link-rejection case, and the two multi-source usage-error paths.
run_multi_ctest "multifile" "multifile_main.c multifile_lib.c"
run_multi_link_reject_test
run_multi_usage_test "multifile_no_out" "needs an output path" \
    "${CTEST_DIR}/multifile_main.c" "${CTEST_DIR}/multifile_lib.c"
run_multi_usage_test "multifile_emit_reject" "only when compiling a single" \
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

run_launcher_defaults

# maize-24 keystone (Piece 3): quesOS single-tasking exec/reap. Builds the two
# borrowed static guest printers (os/quesos/demo_child*.c) through the ordinary
# cc-maize.sh pipeline (stock .mzx at base 0x2000), links quesOS itself at its
# non-default base via os/quesos/build-quesos.sh, then runs quesOS as a directly-
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
    builder="${REPO_ROOT}/os/quesos/build-quesos.sh"
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
    if ! sh "$builder" --preset "$PRESET" -o "$quesos" >>"$log" 2>&1 || [ ! -f "$quesos" ]; then
        echo "[FAIL] ${name}: quesOS link failed"; cat "$log" >&2
        FAIL_COUNT=$((FAIL_COUNT + 1)); return
    fi

    nat=$(host_to_native "$progs")
    # MSYS2_ARG_CONV_EXCL keeps the /progs guest paths from being rewritten to Windows
    # paths on the MinGW leg (same reason doom-render excludes /ro); harmless elsewhere.
    set +e
    actual=$(MSYS2_ARG_CONV_EXCL='/progs' "$MAIZE" --no-root --mount "${nat}=/progs:ro" \
        "$quesos" /progs/child1.mzx /progs/child2.mzx 2>/dev/null | grep -v '^$')
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

run_quesos_selfcheck

# maize-93 process ladder: the multi-process quesOS acceptance fixtures. Each is a C
# program compiled by the ordinary cc-maize.sh pipeline (stock .mzx) and run UNDER
# quesOS, which exercises fork (eager copy on Sv48), execve, waitpid/zombies, pipes +
# dup2 + per-process fd tables, and the preemptive round-robin timer scheduler. quesOS
# is linked once; each scenario runs its launcher off the worklist (exec/pipeline
# targets are built into /progs but not on the worklist). The gate is the fixture's own
# self-checked PASS marker in the transcript. Wrapped in `timeout` so a scheduler or
# blocking-semantics regression that livelocks is a failure, not a hung suite.
run_quesos_ac_fixtures() {
    builder="${REPO_ROOT}/os/quesos/build-quesos.sh"
    progs="${WORK_DIR}/quesos-ac"
    quesos="${WORK_DIR}/quesos-ac.mzx"
    log="${WORK_DIR}/quesos-ac.log"
    rm -rf "$progs"; mkdir -p "$progs"

    if ! sh "$builder" --preset "$PRESET" -o "$quesos" >"$log" 2>&1 || [ ! -f "$quesos" ]; then
        echo "[FAIL] quesos_ac: quesOS link failed"; cat "$log" >&2
        TOTAL=$((TOTAL + 1)); FAIL_COUNT=$((FAIL_COUNT + 1)); return
    fi
    for src in fork_isolation fork_multi exec_launch exec_target pipe_roundtrip \
               pipe_bigwrite pipeline producer filter consumer stress20 preempt \
               blocked console_echo \
               sig_handler sig_default sig_chld sig_pgroup \
               sig_kill sig_exec_launch sig_exec_target \
               fb_register fb_reject fb_fork_cleanup fb_exec_launch fb_exec_target \
               fb_exit_cleanup; do
        if ! "$CC_MAIZE" --preset "$PRESET" -o "${progs}/${src}.mzx" \
                "${REPO_ROOT}/os/quesos/${src}.c" >>"$log" 2>&1; then
            echo "[FAIL] quesos_ac: ${src}.c compile failed"; cat "$log" >&2
            TOTAL=$((TOTAL + 1)); FAIL_COUNT=$((FAIL_COUNT + 1)); return
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
}

run_quesos_ac_fixtures

# maize-94 wave-1 kernel plumbing: quesOS forwards the native hostfs file/dir subset
# (decision 8941), owns a per-process cwd + relative-path resolution (decision 8940), and
# forwards the console termios calls (OQ 8951 operator ruling) so oksh can enter raw mode.
# fs_forward + cwd_resolve run under a writable /rw mount (alongside :ro /progs for the
# binary); termios_raw runs under --console-dump (which binds the grid console's termios).
# Each is a self-checked PASS marker; `timeout` guards a blocking-semantics regression.
run_quesos94_fixtures() {
    builder="${REPO_ROOT}/os/quesos/build-quesos.sh"
    progs="${WORK_DIR}/quesos94"
    rw="${WORK_DIR}/quesos94-rw"
    bin="${WORK_DIR}/quesos94-bin"
    quesos="${WORK_DIR}/quesos94.mzx"
    log="${WORK_DIR}/quesos94.log"
    rm -rf "$progs" "$rw" "$bin"; mkdir -p "$progs" "$rw" "$bin"

    if ! sh "$builder" --preset "$PRESET" -o "$quesos" >"$log" 2>&1 || [ ! -f "$quesos" ]; then
        echo "[FAIL] quesos94: quesOS link failed"; cat "$log" >&2
        TOTAL=$((TOTAL + 1)); FAIL_COUNT=$((FAIL_COUNT + 1)); return
    fi
    for src in fs_forward cwd_resolve termios_raw libc_proc execvp_run bin_echoer setjmp_launch; do
        if ! "$CC_MAIZE" --preset "$PRESET" -o "${progs}/${src}.mzx" \
                "${REPO_ROOT}/os/quesos/${src}.c" >>"$log" 2>&1; then
            echo "[FAIL] quesos94: ${src}.c compile failed"; cat "$log" >&2
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

run_quesos94_fixtures

# maize-94 wave-1 userland: the VENDORED sbase binaries (userland/oksh + userland/sbase
# submodules, built by userland/build-userland.sh through the same cc-maize.sh pipeline),
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
# (quesOS worklist entries take no args). build-userland.sh needs cp/find (and, once the
# oksh overlay lands, patch); the Windows MSYS leg installs patch+diffutils via ci.yml.
run_userland94_fixtures() {
    builder="${REPO_ROOT}/os/quesos/build-quesos.sh"
    ubuild="${REPO_ROOT}/userland/build-userland.sh"
    progs="${WORK_DIR}/ul94-progs"
    bindir="${WORK_DIR}/ul94-bin"
    rwdir="${WORK_DIR}/ul94-rw"
    quesos="${WORK_DIR}/ul94-quesos.mzx"
    log="${WORK_DIR}/ul94.log"
    rm -rf "$progs" "$bindir" "$rwdir"; mkdir -p "$progs" "$bindir" "$rwdir"

    # build-userland.sh stages a scratch checkout with cp -a + find; skip loudly on a host
    # lacking them rather than reporting a spurious failure (never silently pass, though:
    # a SKIP is visible in the CI log).
    if ! command -v find >/dev/null 2>&1 || ! command -v cp >/dev/null 2>&1; then
        echo "[SKIP] userland94: cp/find unavailable (cannot stage the sbase scratch tree)"
        return
    fi

    if ! sh "$builder" --preset "$PRESET" -o "$quesos" >"$log" 2>&1 || [ ! -f "$quesos" ]; then
        echo "[FAIL] userland94: quesOS link failed"; cat "$log" >&2
        TOTAL=$((TOTAL + 1)); FAIL_COUNT=$((FAIL_COUNT + 1)); return
    fi
    # Build the shipped wave-1 /bin set through the userland harness (the vendored sbase
    # plus the oksh shell). oksh is the wave's central deliverable (ACs 8929-8934).
    if ! sh "$ubuild" --preset "$PRESET" --out "$bindir" \
            true false echo cat pwd printf cp mv rm ls oksh >>"$log" 2>&1; then
        echo "[FAIL] userland94: build-userland.sh failed to build the wave-1 sbase + oksh set"
        cat "$log" >&2
        TOTAL=$((TOTAL + 1)); FAIL_COUNT=$((FAIL_COUNT + 1)); return
    fi
    # bin_echoer is the execvp-fallback resolution target (decision 9084), placed in /bin.
    if ! "$CC_MAIZE" --preset "$PRESET" -o "${bindir}/bin_echoer.mzx" \
            "${REPO_ROOT}/os/quesos/bin_echoer.c" >>"$log" 2>&1; then
        echo "[FAIL] userland94: bin_echoer.c compile failed"; cat "$log" >&2
        TOTAL=$((TOTAL + 1)); FAIL_COUNT=$((FAIL_COUNT + 1)); return
    fi
    # The launcher drivers are quesOS worklist entries (compiled like any fixture):
    # sbase_launch (echo|cat pipeline), printf_launch, and the cp/mv/rm fs launchers
    # (each seeds a file on the /rw mount, execve's its util, and verifies the result).
    for _drv in sbase_launch printf_launch cp_launch mv_launch rm_launch ls_launch \
                oksh_shell execvp_ext oksh_interactive; do
        if ! "$CC_MAIZE" --preset "$PRESET" -o "${progs}/${_drv}.mzx" \
                "${REPO_ROOT}/os/quesos/${_drv}.c" >>"$log" 2>&1; then
            echo "[FAIL] userland94: ${_drv}.c compile failed"; cat "$log" >&2
            TOTAL=$((TOTAL + 1)); FAIL_COUNT=$((FAIL_COUNT + 1)); return
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
    ul94_run() {
        MSYS2_ARG_CONV_EXCL='/progs;/bin;/rw' timeout 90 "$MAIZE" --no-root \
            --mount "${pnat}=/progs:ro" --mount "${bnat}=/bin:ro" \
            --mount "${rnat}=/rw:rw" \
            "$quesos" "$1" 2>/dev/null
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
    # pty_oksh_check.py forks `maize <quesos> /bin/oksh.mzx` (the DEFAULT input path, no
    # --input flag: exactly the operator's invocation) into a pseudo terminal, waits for the
    # prompt, types "pwd" + an echo + "exit" as keystrokes, and asserts the shell echoed and
    # executed them. This is the acceptance bar the piped/-c fixtures missed twice: with the
    # default-path console input fixed (demand-driven con_data read) an interactive shell now
    # works from a real terminal. Linux pty variant only (CI-safe, stdlib pty); the Windows
    # ConPTY equivalent is operator/local. Skips loudly if python3 is unavailable.
    if command -v python3 >/dev/null 2>&1; then
        TOTAL=$((TOTAL + 1))
        set +e
        out=$(python3 "${REPO_ROOT}/scripts/pty_oksh_check.py" \
            "$MAIZE" "$quesos" "$bindir" "$rwdir" 2>&1)
        set -e
        if printf '%s\n' "$out" | grep -qF "pty-oksh: PASS"; then
            echo "[PASS] userland94_oksh_keystrokes (real pty, default input path)"
        else
            echo "[FAIL] userland94_oksh_keystrokes"; printf '%s\n' "$out" | sed 's/^/          | /'
            FAIL_COUNT=$((FAIL_COUNT + 1))
        fi
    else
        echo "[SKIP] userland94_oksh_keystrokes (python3 unavailable for the pty driver)"
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
}

run_userland94_fixtures

echo "-----------------------------------------------------------------------"
if [ "$FAIL_COUNT" -eq 0 ]; then
    echo "C toolchain: ${TOTAL} passed, 0 failed."
    exit 0
else
    echo "C toolchain: $((TOTAL - FAIL_COUNT)) passed, ${FAIL_COUNT} failed."
    exit 1
fi
