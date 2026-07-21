#!/bin/sh
# build-userland.sh (maize-94): compile the vendored wave-1 userland to .mzx binaries.
#
# The vendoring model (VENDORING.md): each project is a PRISTINE pinned submodule; every
# Maize-local change lives in a numbered userland/patches/<project>/ overlay. This script
# stages a scratch checkout of each submodule, applies its patch series in order, then
# compiles each program through scripts/cc-maize.sh (the single canonical C pipeline)
# using the program's userland/sources/<project>/<name>.list source list. Output is one
# <name>.mzx per program in the chosen --out dir (the wave-1 /bin set). Never edits the
# submodule in place, so a re-pin stays clean.
#
# Usage: userland/build-userland.sh [--preset <name>] --out <dir> [prog ...]
#   With no prog names, builds the full wave-1 set. `prog` is an sbase util name or
#   `oksh`. The sbase scratch also carries userland/include on the cpp path (the regex.h
#   shim util.h pulls in).
#
# Exit codes: 0 all built; 1 a build failed; 2 usage / setup failure.
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "${SCRIPT_DIR}/.." && pwd)
CC_MAIZE="${REPO_ROOT}/scripts/cc-maize.sh"
INCLUDE_DIR="${SCRIPT_DIR}/include"

die() { echo "build-userland.sh: $*" >&2; exit 2; }

# Fail LOUDLY if a build claims success but the produced image is missing, empty, or not
# a real .mzx (first 4 bytes must be the "MZX" magic + version 0x01). cc-maize.sh already
# fails nonzero on a pipeline error, but this catches a silently-truncated or empty output
# before it reaches quesOS as an unloadable image (the failure mode is a cryptic
# "[quesos] not a loadable .mzx image" much later, off in a test harness).
verify_mzx() {
    _f="$1"
    if [ ! -f "$_f" ]; then echo "build-userland.sh: MISSING output ${_f}" >&2; return 1; fi
    _sz=$(wc -c < "$_f" 2>/dev/null | tr -d ' ')
    if [ -z "$_sz" ] || [ "$_sz" -lt 24 ]; then
        echo "build-userland.sh: output ${_f} too small (${_sz:-0} bytes, need >= 24-byte header)" >&2
        return 1
    fi
    _magic=$(dd if="$_f" bs=1 count=3 2>/dev/null)
    if [ "$_magic" != "MZX" ]; then
        echo "build-userland.sh: output ${_f} is not a .mzx image (bad magic)" >&2
        return 1
    fi
    return 0
}

# --- maize-263: WSL-native mirror + throttle, BEFORE arg parsing consumes "$@" so
#     --out and the program list reach the mirrored child intact. --out is a
#     caller-supplied path resolved against the unchanged PWD (no cd), so the .mzx
#     outputs still land where the caller asked. The staged-source cache (C2) below
#     removes the per-run submodule re-copy; the loop itself is serial (no -j). ----
. "${REPO_ROOT}/scripts/lib/harness-env.sh"
maize_apply_throttle
# Precompute submodule SHAs host-side before re-exec (D14): the C2 stage-cache key
# needs sbase/oksh HEADs, and git does not resolve inside the git-less mirror.
maize_precompute_submodule_keys "$REPO_ROOT"
maize_native_mirror_run "$REPO_ROOT" "$SCRIPT_DIR" "$(basename "$0")" -- "$@"

USERLAND_STAGE_CACHE_ROOT="${MAIZE_USERLAND_STAGE_CACHE:-$HOME/.cache/maize/userland-stage}"

UNAME=$(uname -s)
case "$UNAME" in
    Linux)  PRESET='linux-release' ;;
    Darwin) PRESET='macos-debug' ;;
    MINGW*|MSYS*|CYGWIN*) PRESET='windows-llvm-mingw-release' ;;
    *) die "unsupported platform: ${UNAME}" ;;
esac

OUT=""
PROGS=""
while [ $# -gt 0 ]; do
    case "$1" in
        --preset) PRESET="${2:-}"; shift 2 ;;
        --preset=*) PRESET="${1#--preset=}"; shift ;;
        --out) OUT="${2:-}"; shift 2 ;;
        --out=*) OUT="${1#--out=}"; shift ;;
        -*) die "unknown option: $1" ;;
        *) PROGS="${PROGS} $1"; shift ;;
    esac
done
[ -n "$OUT" ] || die "an output dir is required: --out <dir>"
mkdir -p "$OUT"

# Wave-1 default set when no programs are named. ed is re-scoped out of wave 1 by
# operator ruling (OQ 9081): it hard-requires POSIX regex (regcomp/regexec, filed as
# maize-243), so the wave ships the other 10 sbase utils + oksh and AC 8935 lands at
# 10/11 with ed explicitly deferred. The operator-confirmed list is OQ 8949.
SBASE_WAVE1="true false echo printf pwd cat cp mv rm ls"

# Wave-2 default set (maize-292): every sbase tool confirmed by an actual build AND
# a real quesOS smoke run (not static inspection alone) to compile, link, and behave
# correctly against the current guest RT, plus the patched `kill`. The spec's own
# Group-A/Group-B triage (18 + 28 tools) turned out to be a spec-time estimate, not a
# build/run-time fact: empirical verification of every listed tool found 15 real gaps
# the spec did not anticipate, each one out of THIS card's scope per decision 9695
# (the stdin fix is the only RT change this card makes):
#   dd (strtoumax), env/nohup (_exit not visible via <unistd.h>, only <syscall.h>),
#   pathchk (_POSIX_PATH_MAX), tail (llabs), test (faccessat/AT_FDCWD, the same *at()
#   family already excluded elsewhere in this card's spec section 7) -- 6 of Group A;
#   sort (strtod), split (SSIZE_MAX), strings/wc (bsearch, transitively via the
#   isXXXrune classifier files), uudecode (chmod) -- 4 of Group B; tr fails separately
#   on a pre-existing qbe-maize backend limit (aggregate/struct arguments are not
#   supported), unrelated to any libc gap. expand/unexpand BUILD and LINK clean but
#   fail their smoke run: both call parselist(), whose
#   `estrtonum(p, 1, MIN(LLONG_MAX, SIZE_MAX))` trips a genuine, pre-existing
#   cproc/qbe backend defect (MIN(LLONG_MAX, SIZE_MAX) evaluates to -1 instead of
#   LLONG_MAX, a 64-bit signed/unsigned ternary mis-promotion, reproduced minimally
#   and confirmed to affect other in-tree call sites of the same macro pairing too),
#   so their default (no -t) invocation exits 1 instead of passing text through.
#   uuencode ALSO builds and links clean but crashes the whole VM ("unhandled
#   interrupt: vector 8", a page fault quesOS has no user-mode recovery for yet) on
#   even the most trivial invocation, reproduced as the SOLE check in its own
#   single-tool smoke fixture (ruling out a fixture-shape artifact); the exact root
#   cause (uuencode.c's own fstat/fread interaction, or another pinned-backend
#   codegen defect) is not chased down here. None of these 15 ship; each is a
#   candidate for its own future card (see maize-292's card comments), not a defect
#   in this list. kill needed its own patch (0007-kill-sig0-existence.patch) for a
#   related but distinct reason: kill() is declared only in <unistd.h> here, but
#   kill.c (like every POSIX C library) expects <signal.h> to declare it; the patch
#   adds the include rather than widening a shared RT header.
SBASE_WAVE2="basename cal cksum dirname logname mkdir printenv sleep sponge tee unlink yes cmp cols comm cut fold head join md5sum paste rev sha1sum sha224sum sha256sum sha384sum sha512sum sha512-224sum sha512-256sum tsort uniq kill"

if [ -z "$PROGS" ]; then PROGS="${SBASE_WAVE1} ${SBASE_WAVE2} oksh"; fi

# maize-263 C2: content-addressed staged-source cache key for a project. sha256 over
# the pinned submodule SHA + the whole applied patch series + the shim-header set
# (decision D8): any re-pin, patch edit, or shim change rolls the key. Echoes "" when
# no sha256 tool is available (caller then always stages fresh). Stdout only carries
# the key here; the caller captures it.
stage_cache_key() {
    _k_proj="$1"
    # Submodule SHA comes from the host-side precompute (D14): git does not resolve
    # inside the git-less mirror. Fall back to a source-side git read only when the env
    # is unset (an in-place, non-mirrored run that skipped precompute).
    case "$_k_proj" in
        sbase) _k_sha="${MAIZE_KEY_SBASE:-}" ;;
        oksh)  _k_sha="${MAIZE_KEY_OKSH:-}" ;;
        *)     _k_sha="" ;;
    esac
    [ -n "$_k_sha" ] || _k_sha=$(maize_pinned_sha "$REPO_ROOT" "userland/${_k_proj}")
    [ -n "$_k_sha" ] || _k_sha=no-head
    {
        printf '%s\n' "$_k_sha"
        for _kp in "${SCRIPT_DIR}/patches/${_k_proj}"/*.patch; do
            [ -e "$_kp" ] && cat "$_kp"
        done
        for _kh in "${INCLUDE_DIR}"/*.h; do
            [ -e "$_kh" ] && cat "$_kh"
        done
    } | maize_sha256
}

# Stage a pristine submodule into a scratch dir and apply its patch series in order.
# Idempotent per run (scratch is rebuilt each invocation). maize-263 C2: when the
# post-patch/post-shim staged tree for this project's exact inputs is already cached
# (a .complete marker guards against a killed-mid-populate cache), restore it with a
# single native-to-native rsync and skip the cp -a / patch-apply / find-exec-copy
# steps entirely. MAIZE_NO_USERLAND_STAGE_CACHE=1 forces a fresh stage. Echoes ONLY
# the stage path on stdout; all cache chatter goes to stderr (the caller captures
# stdout as the return value).
stage_project() {
    _proj="$1"
    _sub="${SCRIPT_DIR}/${_proj}"
    _stage="${WORK}/${_proj}"
    [ -d "$_sub" ] || die "submodule not initialized: ${_sub} (git submodule update --init)"

    _key=$(stage_cache_key "$_proj") || _key=""
    _cache_dir=""
    if [ -n "$_key" ]; then
        _cache_dir="${USERLAND_STAGE_CACHE_ROOT}/${_proj}/${_key}"
    fi

    if [ "${MAIZE_NO_USERLAND_STAGE_CACHE:-}" != "1" ] && [ -n "$_cache_dir" ] \
        && [ -f "${_cache_dir}/.complete" ] && command -v rsync >/dev/null 2>&1; then
        echo "build-userland.sh: userland-stage cache hit for ${_proj} ($(printf '%.8s' "$_key"))." >&2
        rm -rf "$_stage"
        mkdir -p "$_stage"
        # Exclude the marker so the restored stage stays a clean source tree.
        rsync -a --delete --exclude='/.complete' "${_cache_dir}/" "${_stage}/"
        echo "$_stage"
        return 0
    fi

    rm -rf "$_stage"
    cp -a "$_sub" "$_stage"
    if [ -d "${SCRIPT_DIR}/patches/${_proj}" ]; then
        for _p in "${SCRIPT_DIR}/patches/${_proj}"/*.patch; do
            [ -e "$_p" ] || continue
            if ! (cd "$_stage" && patch -p1 --forward --silent < "$_p"); then
                die "patch failed for ${_proj}: ${_p}"
            fi
        done
    fi
    # Copy the Maize-local shim headers (e.g. the regex.h util.h pulls in) into EVERY
    # directory of the scratch checkout, so cc-maize.sh's per-source `-I <source dir>`
    # resolves the angle-bracket include no matter which subdir a source lives in (sbase
    # sources sit in both the root and libutil/). No RT-slice change, no -I passthrough.
    if [ -d "$INCLUDE_DIR" ]; then
        for _h in "$INCLUDE_DIR"/*.h; do
            [ -e "$_h" ] || continue
            find "$_stage" -type d -exec cp "$_h" {} \;
        done
    fi

    # Populate the cache (marker written LAST) for the next run/worktree with the same
    # inputs. Best-effort: a cache-write failure removes the partial dir and continues.
    if [ "${MAIZE_NO_USERLAND_STAGE_CACHE:-}" != "1" ] && [ -n "$_cache_dir" ] \
        && command -v rsync >/dev/null 2>&1; then
        rm -rf "$_cache_dir"
        if mkdir -p "$_cache_dir" && rsync -a --delete "${_stage}/" "${_cache_dir}/"; then
            : > "${_cache_dir}/.complete"
        else
            echo "build-userland.sh: WARNING: could not populate userland-stage cache for ${_proj}; continuing." >&2
            rm -rf "$_cache_dir" 2>/dev/null || true
        fi
    fi

    echo "$_stage"
}

WORK=$(mktemp -d)
cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT

SBASE_STAGE=$(stage_project sbase)
OKSH_STAGE=""

# Build the vendored oksh shell. Like build_sbase_util, but oksh has a single source
# list (sources/oksh/oksh.list) and carries a Maize-local overlay of hand-authored
# shim headers plus one stub source (patches/oksh/include/ -> the scratch root),
# replacing oksh's autoconf output: pconfig.h pins every HAVE_* to Maize's libc, and
# grp/pwd/paths/sys shims + maize_stubs.c satisfy the borrowed sources. EMACS is
# defined on the cpp line (mirroring upstream's -DEMACS cflag) so config.h's
# "define EMACS or VI" guard passes with the emacs-mode line editor in scope
# (decision 8945). Nothing is hand-edited in the submodule checkout (AC 8929).
build_oksh() {
    _list="${SCRIPT_DIR}/sources/oksh/oksh.list"
    [ -f "$_list" ] || die "no sources list for oksh: ${_list}"
    _overlay="${SCRIPT_DIR}/patches/oksh/include"
    if [ -d "$_overlay" ]; then
        # Recursive overlay copy preserves the sys/ subdir (sys/param.h et al) that the
        # flat shim-header copy in stage_project cannot place.
        cp -a "${_overlay}/." "${OKSH_STAGE}/"
    fi
    _srcs=""
    while IFS= read -r _line || [ -n "$_line" ]; do
        _line=$(printf '%s' "$_line" | tr -d '\r')
        case "$_line" in ''|\#*) continue ;; esac
        _srcs="${_srcs} ${OKSH_STAGE}/${_line}"
    done < "$_list"
    [ -n "$_srcs" ] || die "empty sources list: ${_list}"
    # -D EMACS: satisfy config.h's "define EMACS or VI" guard with the emacs-mode line
    #   editor in scope (decision 8945), mirroring upstream's -DEMACS cflag.
    # -D volatile=: neutralize the `volatile` keyword before cproc-qbe, which cannot yet
    #   lower a volatile store (it rejects the `volatile sig_atomic_t` trap flags and the
    #   setjmp-survival `volatile int` locals oksh uses pervasively). This mirrors
    #   cc-maize.sh's own `-D '__attribute__(x)='` keyword-neutralizing idiom and is a
    #   build-config choice, not a per-file source edit. It is SAFE on Maize: no RT source
    #   relies on volatile (verified: zero `volatile` in toolchain/rt/*.c), wave-1 delivers
    #   no asynchronous signals so the trap-flag volatiles are moot (decision 8947), and
    #   qbe-maize spills locals to the stack (which longjmp does not clobber) rather than
    #   caching them across the setjmp call. The exit-status ACs (8933) verify empirically
    #   that command status survives oksh's longjmp-based error unwind. Recorded as a
    #   decision on the card.
    # shellcheck disable=SC2086
    if ! "$CC_MAIZE" --preset "$PRESET" -D EMACS -D "volatile=" -o "${OUT}/oksh.mzx" $_srcs; then
        echo "build-userland.sh: FAILED building oksh" >&2
        return 1
    fi
    verify_mzx "${OUT}/oksh.mzx" || return 1
    echo "built ${OUT}/oksh.mzx"
}

build_sbase_util() {
    _name="$1"
    _list="${SCRIPT_DIR}/sources/sbase/${_name}.list"
    [ -f "$_list" ] || die "no sources list for sbase/${_name}: ${_list}"
    # Prefix each list entry with the scratch sbase dir; skip blank/# lines.
    _srcs=""
    while IFS= read -r _line || [ -n "$_line" ]; do
        _line=$(printf '%s' "$_line" | tr -d '\r')
        case "$_line" in ''|\#*) continue ;; esac
        _srcs="${_srcs} ${SBASE_STAGE}/${_line}"
    done < "$_list"
    [ -n "$_srcs" ] || die "empty sources list: ${_list}"
    # cc-maize.sh -I's each source's own directory (the scratch sbase dir), which carries
    # sbase's own headers plus the copied shim headers, so angle-bracket includes resolve.
    # shellcheck disable=SC2086
    if ! "$CC_MAIZE" --preset "$PRESET" -o "${OUT}/${_name}.mzx" $_srcs; then
        echo "build-userland.sh: FAILED building sbase/${_name}" >&2
        return 1
    fi
    verify_mzx "${OUT}/${_name}.mzx" || return 1
    echo "built ${OUT}/${_name}.mzx"
}

rc=0
for prog in $PROGS; do
    if [ "$prog" = "oksh" ]; then
        [ -n "$OKSH_STAGE" ] || OKSH_STAGE=$(stage_project oksh)
        build_oksh || rc=1
        continue
    fi
    build_sbase_util "$prog" || rc=1
done
exit "$rc"
