#!/bin/sh
# cc-maize.sh: the SINGLE canonical C compile-and-run driver for the Maize C
# toolchain (maize-96). This is the one place the whole C pipeline is defined:
#
#   tr -d '\r'                (defense-in-depth CRLF strip; no-op on a clean LF tree)
#     -> cpp -E -P -nostdinc -I toolchain/rt   (expand #include; cproc-qbe has none)
#     -> cproc-qbe            (C11 -> QBE IL)
#     -> normalize            (drop the `extern` linkage annotation; lower `neg`)
#     -> qbe -t maize         (QBE IL -> mazm body, with SECTION/GLOBAL/ALIGN/DREF)
#     -> mazm -c              (body.mazm -> body.mzo relocatable object)
#     -> mzld                 (crt0 syscall errno string ctype stdio stdlib body ->
#                              <base>.mzx; entry _start; W^X, per-section alignment)
#     -> maize                (load_mzx sets RP=_start; execute; propagate guest exit)
#
# BOTH consumers of the C pipeline call this driver, so CI (scripts/run-ctest.sh,
# via `-o <path>`) exercises the EXACT pipeline the operator acceptance-tests
# with (the ~/bin/mzcc forwarder execs this file). The normalize sed, the cpp
# flags, the RT object set, and the mzld link order therefore live in EXACTLY ONE
# place: here. Drift between CI and the operator's tool is structurally impossible.
#
# `extern` normalization: the pinned cproc (d1c53dd) tags EVERY external-symbol
# operand `extern` (calls AND loads/adds of file-scope globals), a linkage
# annotation the pinned qbe (4420727) predates and rejects. In the Maize linked-
# image model every `$sym` resolves through mzld regardless, so the annotation
# carries no meaning and is stripped deterministically. `neg X` is the unary op the
# pinned qbe's parser predates; `sub 0, X` is the identity lowering (same class /
# semantics).
#
# Modes (three orthogonal axes: RUN via -r/--run, EMIT via --emit, OUT via -o):
#   cc-maize.sh [--preset <name>] <file.c>
#       DEFAULT: compile + link to <base>.mzx beside the source; do NOT run (exit 0).
#   cc-maize.sh [--preset <name>] -r|--run <file.c>
#       compile + run, propagating the guest exit code; runs from scratch, leaving NO
#       persistent .mzx unless -o is also given.
#   cc-maize.sh [--preset <name>] --emit <file.c>
#       also drop <base>.mazm (the qbe body) beside the source. --emit governs ONLY
#       the .mazm; the .mzx still follows the produce rule (default/-o/-r) above.
#   cc-maize.sh [--preset <name>] -o <path> <file.c>
#       write the linked image to <path> (suppresses the beside-source default); add
#       -r to produce AND run.
#   cc-maize.sh [--preset <name>] -o <path> <a.c> <b.c> [<c.c> ...]
#       MULTI-SOURCE (maize-138): compile each C source to its own object and link them
#       together with the runtime into one .mzx at <path>. -o (or -r) is REQUIRED with
#       two or more sources: there is no single source to sit a beside-source default
#       next to. --emit is single-source only and is rejected in multi-source mode.
#   cc-maize.sh [--preset <name>] -o <path> --sources <listfile>
#       read the C source list from <listfile> (one path per line; blank lines and
#       lines beginning with # are skipped), then compile + link as the multi-source
#       case above. Positionals and --sources may be combined.
#   cc-maize.sh --build
#       (re)build the vendored cproc/qbe toolchain, then exit.
#   --compile-only is retained as a back-compat no-op alias of the new no-run default.
#
# `*.mzx` is globally gitignored, so the beside-source produce never dirties the tree.
# `*.mazm` is gitignored only under ctest/ (.gitignore line 65); an --emit .mazm dropped
# beside a non-ctest/ C source is an explicit, opt-in debug artifact the user cleans up.
#
# A source given as a Windows path (C:\... or C:/...) is translated with wslpath when
# present (WSL), else cygpath (Git Bash / MSYS2, maize-257), so the tool works from a
# Windows shell forwarder as well as from a WSL/Linux shell.
#
# cproc is strict C11: declare any libc-style function you call (e.g.
# `int puts(const char *);`); the declaration only satisfies the front-end, the
# symbol still resolves to the runtime's definition in mazm's shared label table.
#
# Exit codes: the guest's exit code under -r/--run; 0 on any produce-only path
# (default, --emit, -o without -r); 2 for a usage or environment/setup failure; a
# nonzero pipeline-stage failure is reported with context and propagated.
set -eu

# --- Paths resolved relative to THIS script, not the caller's CWD ----------------
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "${SCRIPT_DIR}/.." && pwd)
RT_DIR="${REPO_ROOT}/toolchain/rt"
QBE_DIR="${REPO_ROOT}/toolchain/qbe"
CPROC_DIR="${REPO_ROOT}/toolchain/cproc"

die() { echo "cc-maize.sh: $*" >&2; exit 2; }

# Translate a Windows-style path (C:\... or C:/...) to a POSIX path this shell
# understands: wslpath when present (WSL bash), else cygpath -u (Git Bash / MSYS2,
# maize-257, neither of which ships wslpath).
win_to_posix() {
    if command -v wslpath >/dev/null 2>&1; then
        wslpath "$1"
    else
        cygpath -u "$1"
    fi
}

# maize-257: translate a POSIX path this shell understands into the form the
# NATIVE Windows tool binaries (mazm/qbe/cproc-qbe/mzld/maize, all built for the
# windows-llvm-mingw-* preset) need when passed as a COMMAND-LINE ARGUMENT (never
# needed for stdin/stdout/file redirection: the shell opens those handles itself,
# bypassing argv entirely). Bash/MSYS auto-converts POSIX-looking argv strings to
# native windows exes IN AN INTERACTIVE OR ALREADY-MSYS-ROOTED shell, but that
# conversion is NOT reliable when bash itself is a freshly-spawned child of a
# non-MSYS parent (exactly mzcc.cmd's own shape: a .cmd batch file execs bash.exe,
# which execs this script): verified empirically that the identical `mazm -c
# /tmp/x/crt0.mazm` argv fails with "filesystem error: in canonical" (mazm.exe
# receiving the raw, unconverted POSIX string) when bash is spawned from cmd.exe,
# while the SAME command succeeds when bash is already the caller's shell. Rather
# than depend on that ambient, context-dependent auto-conversion, every path handed
# to a native .exe as an argument is explicitly converted here, mirroring
# run-ctest.sh's host_to_native. A no-op (prints its argument unchanged) on
# Linux/macOS, where mazm/mzld/maize/qbe/cproc-qbe are native ELF/Mach-O binaries
# that need no translation.
native_path() {
    case "$(uname -s)" in
        MINGW*|MSYS*|CYGWIN*)
            if command -v cygpath >/dev/null 2>&1; then
                cygpath -w "$1"
            elif command -v wslpath >/dev/null 2>&1; then
                wslpath -w "$1"
            else
                printf '%s' "$1"
            fi
            ;;
        *) printf '%s' "$1" ;;
    esac
}

# native_path() applied to each word of a space-separated path list (RT_OBJS /
# USER_OBJS), for the mzld link line's object-file arguments. Assumes no embedded
# spaces in any individual path, the same assumption RT_OBJS/USER_OBJS already make
# via their own unquoted word-splitting.
native_path_list() {
    _npl_out=""
    for _npl_p in $1; do
        _npl_out="${_npl_out} $(native_path "$_npl_p")"
    done
    printf '%s' "$_npl_out"
}

# --- Default build preset, mirroring run-ctest.sh's platform switch ---------------
UNAME=$(uname -s)
case "$UNAME" in
    Linux)  DEFAULT_PRESET='linux-debug' ;;
    Darwin) DEFAULT_PRESET='macos-debug' ;;
    MINGW*|MSYS*|CYGWIN*) DEFAULT_PRESET='windows-llvm-mingw-debug' ;;
    *) die "unsupported platform: ${UNAME}" ;;
esac

# --- maize-263: throttle only, NO native mirror (decision D15, reverses D3). The
#     mirror's value is BATCH workloads (run-tests.sh / run-ctest.sh), where the
#     one-time full-tree rsync is amortized over the whole suite. A STANDALONE
#     cc-maize.sh call (the interactive `mzcc foo.c` path) compiles ONE small source,
#     so the ~1.1s full-tree rsync scan makes it SLOWER than running in place (Test
#     report #2910 measured ~1.5s mirrored vs ~1.2s in-place) -- the opposite of the
#     spec's promise to keep interactive use fast. Suite-internal cc-maize.sh calls
#     already run inside the mirrored parent (the recursion guard makes their own
#     mirror check a no-op), so a wrapper here would only ever fire standalone, where
#     it only hurts. So: keep the niceness throttle, drop the mirror. Interactive
#     single compiles stay in place; the 9P cost of one small compile is negligible. -
. "${SCRIPT_DIR}/lib/harness-env.sh"
maize_apply_throttle

# --- Parse arguments (flags accepted in any position) ----------------------------
# Three orthogonal axes govern the compile path (RUN / EMIT / OUT); `build` is the
# one special mode. See the Modes block above for the authoritative matrix.
PRESET="$DEFAULT_PRESET"
MODE="compile"      # compile | build
RUN=0               # -r / --run
EMIT=0              # --emit
DEV=0               # --dev (append the mzdev device-access shim to the link, maize-121)
EXTRA_CPPDEFS=""    # -D <name>[=val] cpp-define passthrough (maize-153); defaults empty so
                    # every existing caller's cpp command is byte-identical. Threaded into
                    # compile_tu's cpp line after the fixed -D '__attribute__(x)='.
OUT=""
POS_SRCS=""         # positional C sources, newline-separated, in command-line order
SRCFILES=""         # --sources listfiles, newline-separated
USED_SOURCES=0      # set when --sources appears (forces the multi-source path, maize-138)
while [ $# -gt 0 ]; do
    case "$1" in
        --build) MODE="build"; shift ;;
        -r|--run) RUN=1; shift ;;
        --emit) EMIT=1; shift ;;
        --dev) DEV=1; shift ;;
        -D)  EXTRA_CPPDEFS="${EXTRA_CPPDEFS} -D $2"; shift 2 ;;   # -D <name>[=val] (maize-153)
        -D*) EXTRA_CPPDEFS="${EXTRA_CPPDEFS} $1";    shift ;;     # -D<name>[=val] (glued form)
        --compile-only) RUN=0; shift ;;   # back-compat: no-op alias of new default (D3)
        -o) OUT="${2:-}"; shift 2 ;;
        -o*) OUT="${1#-o}"; shift ;;
        --preset) PRESET="${2:-}"; shift 2 ;;
        --preset=*) PRESET="${1#--preset=}"; shift ;;
        --sources) SRCFILES="${SRCFILES}${2:-}
"; USED_SOURCES=1; shift 2 ;;
        --sources=*) SRCFILES="${SRCFILES}${1#--sources=}
"; USED_SOURCES=1; shift ;;
        --) shift; while [ $# -gt 0 ]; do POS_SRCS="${POS_SRCS}$1
"; shift; done ;;
        -*) die "unknown option: $1" ;;
        *) POS_SRCS="${POS_SRCS}$1
"; shift ;;
    esac
done

# --- Toolchain build passthrough --------------------------------------------------
if [ "$MODE" = "build" ]; then
    exec "${SCRIPT_DIR}/build-toolchain.sh"
fi

# --- Collect the C source list (positionals + any --sources listfiles) -----------
# One source is the single-source path (unchanged). Two or more sources, OR any use
# of --sources, take the multi-source path (maize-138). A source given as a Windows
# path (C:\... or C:/...) is translated with wslpath per source, so the tool works
# from a Windows shell forwarder as well as from a WSL/Linux shell.
SRC_LIST=""         # resolved source paths, newline-separated, in link order
SRC_COUNT=0
add_source() {
    _s="$1"
    case "$_s" in
        *:\\*|[A-Za-z]:/*) _s=$(win_to_posix "$_s") ;;
    esac
    [ -f "$_s" ] || die "no such file: $_s"
    SRC_LIST="${SRC_LIST}${_s}
"
    SRC_COUNT=$((SRC_COUNT + 1))
}

# Positionals first (command-line order). A while-read fed from a here-doc (not a
# pipe) runs in THIS shell, so add_source's counter/list updates persist.
while IFS= read -r _line; do
    [ -n "$_line" ] || continue
    add_source "$_line"
done <<EOF
${POS_SRCS}
EOF

# Then each --sources listfile, appending its non-blank, non-# lines in file order.
while IFS= read -r _sf; do
    [ -n "$_sf" ] || continue
    case "$_sf" in
        *:\\*|[A-Za-z]:/*) _sf=$(win_to_posix "$_sf") ;;
    esac
    [ -f "$_sf" ] || die "no such --sources listfile: $_sf"
    while IFS= read -r _entry || [ -n "$_entry" ]; do
        _entry=$(printf '%s' "$_entry" | tr -d '\r')
        case "$_entry" in
            ''|\#*) continue ;;
        esac
        add_source "$_entry"
    done < "$_sf"
done <<EOF
${SRCFILES}
EOF

[ "$SRC_COUNT" -gt 0 ] || die "usage: cc-maize.sh [--preset <name>] [-r|--run] [--emit] [-o <path>] <file.c>
       cc-maize.sh [--preset <name>] [-r|--run] -o <path> <a.c> <b.c> [<c.c> ...]
       cc-maize.sh [--preset <name>] [-r|--run] -o <path> --sources <listfile>
       cc-maize.sh --build"

# Single source with no --sources keeps the existing single-source path exactly;
# two or more sources (or any --sources use) take the multi-source path.
MULTI=0
if [ "$SRC_COUNT" -ge 2 ] || [ "$USED_SOURCES" -eq 1 ]; then MULTI=1; fi

# Multi-source preconditions, checked early (before the toolchain is even resolved)
# so a usage mistake fails fast. With several sources there is no single source to
# sit a beside-source default next to, so an explicit output is required; --emit
# drops one qbe body beside its single source and does not generalize to many.
if [ "$MULTI" -eq 1 ]; then
    if [ "$EMIT" -eq 1 ]; then
        die "--emit works only when compiling a single .c file (it drops that file's
       qbe body beside the source). Drop --emit for a multi-file build:
       cc-maize.sh [--preset <name>] -o <out.mzx> <a.c> <b.c> [<c.c> ...]"
    fi
    if [ -z "$OUT" ] && [ "$RUN" -eq 0 ]; then
        die "a multi-file build needs an output path: pass -o <out.mzx> to write the
       linked image (add -r to also run it), or -r alone to build and run it:
       cc-maize.sh [--preset <name>] -o <out.mzx> <a.c> <b.c> [<c.c> ...]
       cc-maize.sh [--preset <name>] -r --sources <listfile>"
    fi
fi

BUILD_DIR="${REPO_ROOT}/build/${PRESET}"

# Resolve an executable path, tolerating a .exe suffix on Windows.
resolve_exe() {
    if [ -x "$1" ] || [ -f "$1" ]; then echo "$1"; return 0; fi
    if [ -x "$1.exe" ] || [ -f "$1.exe" ]; then echo "$1.exe"; return 0; fi
    return 1
}

CPROC_QBE=$(resolve_exe "${CPROC_DIR}/cproc-qbe") \
    || die "cproc-qbe not found; run 'cc-maize.sh --build' (cproc/qbe)."
QBE=$(resolve_exe "${QBE_DIR}/obj/qbe") \
    || die "qbe not found; run 'cc-maize.sh --build' (cproc/qbe)."
MAZM=$(resolve_exe "${BUILD_DIR}/mazm") \
    || die "mazm not found in ${BUILD_DIR}; run scripts/install-mazm.sh (or run-tests.sh) first."
MAIZE=$(resolve_exe "${BUILD_DIR}/maize") \
    || die "maize not found in ${BUILD_DIR}; run scripts/install-mazm.sh (or run-tests.sh) first."
MZLD=$(resolve_exe "${BUILD_DIR}/mzld") \
    || die "mzld not found in ${BUILD_DIR}; run scripts/install-mazm.sh (or run-tests.sh) first."

# System preprocessor for #include expansion (maize-74). cproc-qbe's own front-end
# does not implement #include, so a source (or the errno.c runtime) that includes
# toolchain/rt/syscall.h is preprocessed by the system cc/gcc first. Mirror
# build-toolchain.sh's compiler pick.
CPP="${CC:-}"
if [ -z "$CPP" ]; then
    if command -v cc >/dev/null 2>&1; then
        CPP=cc
    elif command -v gcc >/dev/null 2>&1; then
        CPP=gcc
    else
        # maize-257: Git Bash ships neither cc nor gcc. Fall back to the vendored
        # llvm-mingw clang (the same compiler build-toolchain.sh's native branch
        # builds cproc-qbe/qbe with) for the -E preprocess step only.
        _vendored_clang="${REPO_ROOT}/.toolchains/llvm-mingw/bin/x86_64-w64-mingw32-clang.exe"
        [ -f "$_vendored_clang" ] && CPP="$_vendored_clang"
    fi
fi
command -v "$CPP" >/dev/null 2>&1 \
    || die "no C preprocessor (cc/gcc) found for #include expansion. On Windows, run scripts/bootstrap-toolchain.ps1 to fetch the vendored llvm-mingw clang."

# Everything intermediate goes in a scratch dir so the source tree never sees .mzo
# clutter, even with --emit (only the body .mazm and the linked .mzx are copied out).
WORK=$(mktemp -d)
# Clean the scratch dir on exit WHILE PRESERVING the exit status. In dash (the usual
# /bin/sh), the EXIT trap's last command status becomes the script's exit status, so a
# plain `rm` would clobber a guest's nonzero exit (e.g. main returning 42) back to 0.
# Capture $? first and re-exit with it so the guest exit code always propagates.
cleanup() { _rc=$?; rm -rf "$WORK"; exit "$_rc"; }
trap cleanup EXIT

# Compile one C translation unit through the full segmented pipeline to a .mzo:
#   tr -d '\r' -> cpp -E -P -nostdinc -I toolchain/rt -> cproc-qbe -> normalize
#     -> qbe -t maize -> mazm -c
# $1 = source path, $2 = tag for intermediates. Echoes the emitted <tag>.body.mzo on
# success; on failure prints stage context to stderr and returns 1. Shared by the
# body compile and the errno.c runtime compile so both take the identical path.
compile_tu() {
    _src="$1"
    _tag="$2"
    _lf="${WORK}/${_tag}.lf.c"
    _pp="${WORK}/${_tag}.pp.c"
    _ssa="${WORK}/${_tag}.ssa"
    _norm="${WORK}/${_tag}.norm.ssa"
    _body="${WORK}/${_tag}.body.mazm"
    _mzo="${WORK}/${_tag}.body.mzo"

    # Defense in depth for CRLF checkouts (belt-and-suspenders with .gitattributes
    # eol=lf): cproc is strict C11 and treats a bare CR as a stray token. A clean LF
    # checkout makes this a no-op. (maize-62)
    tr -d '\r' < "$_src" > "$_lf"

    # Expand #include / include guards with the system preprocessor. -nostdinc keeps
    # the search to toolchain/rt (freestanding: no system headers); -P drops GNU line
    # markers (cproc-qbe tolerates them, so -P is just for cleanliness). The ORIGINAL
    # source directory is added AFTER toolchain/rt so a source can include a sibling
    # header (e.g. a demo's own term_core.h) while RT headers still resolve from RT_DIR;
    # existing fixtures include only RT headers, so their resolution is unchanged. The
    # -I is the source's real directory (the .lf.c copy lives in the scratch dir, which
    # cpp searches implicitly for the current file).
    #
    # -D '__attribute__(x)=' neutralizes GNU attributes before cproc-qbe (maize-149).
    # __attribute__ is a compiler keyword, not a preprocessor keyword, so cpp treats it
    # as an ordinary identifier and the one-arg function-like redefine is legal: the
    # double-paren form __attribute__((packed)) expands as __attribute__( (packed) ) ->
    # empty (the inner commas of e.g. ((packed, aligned(4))) are paren-protected). DOOM's
    # doomtype.h puts packed in the TRAILING declarator position (} PACKEDATTR name;),
    # which the pinned cproc rejects (it honors packed only in the leading position), so
    # the strip is what unblocks DOOM's 47 core WAD-struct TUs. It is run-safe: every
    # PACKEDATTR struct in the pinned doomgeneric is padding-free under natural alignment
    # (per-member offsetof identical packed-vs-natural; verified in AC-4), so the layout
    # DOOM reads/writes is byte-identical with or without packed. No in-tree TU compiled
    # through this driver uses any GNU attribute, so the strip is a no-op for the existing
    # RT/ctest corpus and cannot regress hello.mzb.
    # ${EXTRA_CPPDEFS} carries any caller -D defines (maize-153); it is empty for every
    # existing caller, so this cpp line is byte-identical unless a -D was passed (DOOM's
    # -D DOOMGENERIC_RESX=320 -D DOOMGENERIC_RESY=200 geometry override). Intentionally
    # unquoted so the accumulated `-D name=val` tokens word-split into separate args.
    #
    # -U WIN32/_WIN32/_WIN64/__MINGW32__/... neutralizes host-OS-identity macros a
    # mingw-target $CPP predefines (maize-257: the vendored llvm-mingw clang used as
    # the native-Windows CPP fallback targets x86_64-w64-mingw32, so it predefines
    # _WIN32 unlike a Linux-targeting cc/gcc). The Maize guest is a freestanding
    # target with no host OS identity, so a borrowed source's `#ifdef _WIN32` must see
    # the same "no host OS" view regardless of which system compiler performs this -E
    # pass. Undefining a macro that was never defined (every non-mingw $CPP) is a
    # silent no-op, so this is byte-identical on Linux/macOS/MSYS2-gcc. Concretely: DOOM's
    # doomtype.h picks its `boolean` typedef via `#ifdef _WIN32` (skip stdbool.h,
    # hand-roll `typedef enum { false = 0, true = 1, ... }`) vs the portable branch
    # (`#include <stdbool.h>`, resolved by toolchain/rt/stdbool.h under -nostdinc,
    # which sets `__bool_true_false_are_defined` and takes the `unsigned int` branch);
    # cproc reserves `false`/`true` as keywords (tokens.h TFALSE/TTRUE), so the
    # hand-rolled enum's enumerators are a syntax error to cproc-qbe. Undefining these
    # restores the portable branch, matching what every non-mingw $CPP already does.
    # native_path(): $CPP is a native Windows binary too on MINGW/MSYS/CYGWIN (either
    # the vendored llvm-mingw clang fallback or MSYS2's own mingw-w64 gcc; maize-257,
    # see native_path's definition above), so its path arguments need the same
    # conversion as mazm/qbe/mzld/maize's. No-op (native_path returns its argument
    # unchanged) on Linux/macOS, where $CPP is a native ELF/Mach-O binary.
    if ! "$CPP" -E -P -nostdinc -D '__attribute__(x)=' \
        -U WIN32 -U WIN64 -U _WIN32 -U _WIN64 \
        -U __WIN32 -U __WIN32__ -U __WIN64 -U __WIN64__ \
        -U __MINGW32__ -U __MINGW64__ \
        ${EXTRA_CPPDEFS} -I "$(native_path "$RT_DIR")" -I "$(native_path "$(dirname -- "$_src")")" "$(native_path "$_lf")" > "$_pp" 2>"${WORK}/${_tag}.cpp.log"; then
        echo "cc-maize.sh: cpp failed for ${_tag}" >&2; cat "${WORK}/${_tag}.cpp.log" >&2; return 1
    fi
    if ! "$CPROC_QBE" < "$_pp" > "$_ssa" 2>"${WORK}/${_tag}.cproc.log"; then
        echo "cc-maize.sh: cproc-qbe failed for ${_tag}" >&2; cat "${WORK}/${_tag}.cproc.log" >&2; return 1
    fi
    # Normalize two IL-version-skew points between the pinned cproc and pinned qbe
    # (see the header comment): strip `extern $` and lower `neg` to `sub 0, `.
    # THIS IS THE ONE NORMALIZE SED IN THE TREE (maize-96).
    sed -e 's/extern \$/$/g' \
        -e 's/\(=[wl]\) neg /\1 sub 0, /' "$_ssa" > "$_norm"
    # native_path(): mazm/qbe are native Windows binaries under the windows-llvm-mingw
    # preset (maize-257; see the native_path definition above for why this cannot rely
    # on ambient MSYS argv auto-conversion). No-op on Linux/macOS.
    if ! "$QBE" -t maize "$(native_path "$_norm")" > "$_body" 2>"${WORK}/${_tag}.qbe.log"; then
        echo "cc-maize.sh: qbe -t maize failed for ${_tag}" >&2; cat "${WORK}/${_tag}.qbe.log" >&2; return 1
    fi
    if ! "$MAZM" -c "$(native_path "$_body")" >"${WORK}/${_tag}.mazm.log" 2>&1 || [ ! -f "$_mzo" ]; then
        echo "cc-maize.sh: mazm -c failed for ${_tag}" >&2; cat "${WORK}/${_tag}.mazm.log" >&2; return 1
    fi
    echo "$_mzo"
    return 0
}

# Assemble the freestanding asm runtime as relocatable objects (maize-77 decision
# 7168): crt0/syscall each become a .mzo. mazm -c writes <input>.mzo beside its
# input, so the sources are copied into WORK first (keeping toolchain/rt clean).
# puts.mazm was retired in maize-76 (decision 7345): puts now lives in C (stdio.c).
# maize-94 adds setjmp.mazm (setjmp/longjmp/sigsetjmp/siglongjmp: machine-dependent
# register save/restore over the Maize calling convention, the borrowed-shell enabler).
# It joins the always-linked RT asm set; images that never reference the symbols simply
# carry a few unused bytes (mzld resolves by name, so link order is irrelevant).
RT_OBJS=""
for rt in crt0 syscall setjmp; do
    cp "${RT_DIR}/${rt}.mazm" "${WORK}/${rt}.mazm"
    if ! "$MAZM" -c "$(native_path "${WORK}/${rt}.mazm")" >"${WORK}/${rt}.mazm.log" 2>&1 || [ ! -f "${WORK}/${rt}.mzo" ]; then
        echo "cc-maize.sh: failed to assemble runtime object ${rt}.mazm:" >&2
        cat "${WORK}/${rt}.mazm.log" >&2
        exit 2
    fi
    RT_OBJS="${RT_OBJS} ${WORK}/${rt}.mzo"
done

# maize-121: the OPT-IN device-access shim. --dev appends toolchain/rt/mzdev.mzo (the
# hand-written IN/OUT stubs over the frozen framebuffer/keyboard ports) to the link so a
# guest-C program can drive the devices. This is the ONLY object added; the default C link
# (no --dev) is byte-for-byte unchanged, so every existing ctest fixture is unaffected.
if [ "$DEV" -eq 1 ]; then
    cp "${RT_DIR}/mzdev.mazm" "${WORK}/mzdev.mazm"
    if ! "$MAZM" -c "$(native_path "${WORK}/mzdev.mazm")" >"${WORK}/mzdev.mazm.log" 2>&1 || [ ! -f "${WORK}/mzdev.mzo" ]; then
        echo "cc-maize.sh: failed to assemble device shim mzdev.mazm:" >&2
        cat "${WORK}/mzdev.mazm.log" >&2
        exit 2
    fi
    RT_OBJS="${RT_OBJS} ${WORK}/mzdev.mzo"
fi

# The C runtime modules (maize-74 errno; maize-76 string/ctype/stdio/stdlib; maize-120
# dirent; maize-148 strings/math/unistd; maize-140 termios) are C sources, not asm. Each is
# compiled through the SAME segmented C pipeline and its object added to the RT set, so
# every C image links the freestanding libc slice (errno + syscall wrappers incl.
# open/close/lseek/fstat + remove/mkdir, string, strings' case-insensitive compares, ctype,
# math's fabs, stdio's unbuffered core + printf/sscanf + file-backed FILE* layer, stdlib's
# exit/abort/malloc family + sbrk + system, unistd's usleep, dirent's opendir/readdir/
# closedir, termios' tcgetattr/tcsetattr/cfmakeraw over the window console, time's
# monotonic-seconds time() over SYS $F0, maize-172) alongside crt0/syscall.
# Single-source per maize-96: this is the ONLY place the RT set is enumerated.
for rt in errno string strings ctype math stdio stdlib unistd dirent termios time; do
    RT_MZO=$(compile_tu "${RT_DIR}/${rt}.c" "rt_${rt}") \
        || die "failed to compile C runtime object ${rt}.c"
    RT_OBJS="${RT_OBJS} ${RT_MZO}"
done

# --- Multi-source path (>= 2 sources, or any --sources use) ----------------------
# Compile every user source to its own .mzo with an INDEX-prefixed tag (u0_<base>,
# u1_<base>, ...) so two sources that share a basename do not collide their WORK
# intermediates or objects, and no user tag collides with the RT tags (rt_*, crt0,
# syscall, mzdev). Then link the RT set plus all user objects into one image. mzld
# resolves cross-object calls and shared globals through its global symbol table, so
# link order does not affect resolution; RT stays first to preserve section layout.
if [ "$MULTI" -eq 1 ]; then
    USER_OBJS=""
    _i=0
    while IFS= read -r _usrc; do
        [ -n "$_usrc" ] || continue
        _ubase=$(basename "${_usrc%.c}")
        _uobj=$(compile_tu "$_usrc" "u${_i}_${_ubase}") || exit 1
        USER_OBJS="${USER_OBJS} ${_uobj}"
        _i=$((_i + 1))
    done <<EOF
${SRC_LIST}
EOF

    MZX="${WORK}/prog.mzx"
    # native_path()/native_path_list(): mzld/maize are native Windows binaries under
    # the windows-llvm-mingw preset (maize-257; see native_path's definition above).
    if ! "$MZLD" -o "$(native_path "$MZX")" $(native_path_list "$RT_OBJS") $(native_path_list "$USER_OBJS") >"${WORK}/prog.mzld.log" 2>&1 || [ ! -f "$MZX" ]; then
        echo "cc-maize.sh: mzld failed linking the multi-source image" >&2
        cat "${WORK}/prog.mzld.log" >&2; exit 1
    fi

    # Produce BEFORE run (D6 ordering) so the artifact lands even on a nonzero guest
    # exit. No beside-source default and no --emit in multi-source mode.
    if [ -n "$OUT" ]; then
        out_dir=$(dirname "$OUT"); [ -d "$out_dir" ] || mkdir -p "$out_dir"
        cp "$MZX" "$OUT"
    fi

    if [ "$RUN" -eq 1 ]; then
        set +e
        "$MAIZE" "$(native_path "$MZX")"
        rc=$?
        set -e
        exit "$rc"
    fi
    exit 0
fi

# --- Single-source path (unchanged: byte-identical link, full existing contract) --
IFS= read -r SRC <<EOF
${SRC_LIST}
EOF

# Compile the user body.
base=$(basename "${SRC%.c}")
BODY_MZO=$(compile_tu "$SRC" "$base") || exit 1

# Link crt0 syscall errno string ctype stdio stdlib body -> <base>.mzx (default entry
# _start; W^X + per-section alignment via mzld). RT_OBJS already carries the
# crt0 syscall + C-runtime order; the body goes last. native_path()/native_path_list():
# mzld is a native Windows binary under the windows-llvm-mingw preset (maize-257; see
# native_path's definition above).
MZX="${WORK}/${base}.mzx"
if ! "$MZLD" -o "$(native_path "$MZX")" $(native_path_list "$RT_OBJS") "$(native_path "$BODY_MZO")" >"${WORK}/${base}.mzld.log" 2>&1 || [ ! -f "$MZX" ]; then
    echo "cc-maize.sh: mzld failed for ${base}" >&2; cat "${WORK}/${base}.mzld.log" >&2; exit 1
fi

# Ordering (load-bearing, D6): emit + produce happen BEFORE the run step, so both
# artifacts land even when the guest exits nonzero and the EXIT trap re-exits with
# that status.

# --- Emit the qbe body beside the source, when asked (independent of run/produce) --
if [ "$EMIT" -eq 1 ]; then
    dst=$(dirname "$SRC")
    cp "${WORK}/${base}.body.mazm" "${dst}/${base}.mazm"
    echo "cc-maize.sh: emitted ${dst}/${base}.mazm (qbe body)" >&2
fi

# --- Produce the linked image: -o wins; else beside-source unless running from scratch
if [ -n "$OUT" ]; then
    out_dir=$(dirname "$OUT"); [ -d "$out_dir" ] || mkdir -p "$out_dir"
    cp "$MZX" "$OUT"
elif [ "$RUN" -eq 0 ]; then
    dst=$(dirname "$SRC")
    cp "$MZX" "${dst}/${base}.mzx"
    echo "cc-maize.sh: produced ${dst}/${base}.mzx" >&2
fi

# --- Run + propagate the guest exit code only when asked --------------------------
# `exec` cannot be used (the EXIT trap must still clean WORK). Capture the guest status
# and `exit` with it: that sets $? for the EXIT trap, and cleanup() re-exits with that
# captured status, so the guest exit code propagates unchanged.
if [ "$RUN" -eq 1 ]; then
    set +e
    "$MAIZE" "$(native_path "$MZX")"
    rc=$?
    set -e
    exit "$rc"
fi
exit 0
