# shellcheck shell=sh
# harness-env.sh (maize-263): shared helpers for the test/build harness scripts.
#
# This file is SOURCED, never executed directly (it defines functions and sets no
# top-level state). Six POSIX-sh harness entry points source it, and they split into
# two groups by whether they produce tools or only consume them:
#
#   Producers (run-tests.sh, build-toolchain.sh) throttle AND mirror. run-tests.sh
#   cmakes and builds its own tree; build-toolchain.sh make/ninjas qbe and cproc.
#   Both hand the results back via maize_sync_back_artifacts, so re-rooting them onto
#   WSL-native storage pays for itself, and the mirror rsync excludes /build precisely
#   because each of them builds a fresh one inside the mirror.
#
#   run-ctest.sh also throttles AND mirrors, but it is a HYBRID rather than a
#   producer: it runs no cmake of its own, and its only build action is a conditional
#   build-toolchain.sh call when qbe or cproc-qbe are absent. It still needs
#   build/<preset>/mazm for the guest suite, and the /build exclusion above hides that
#   from the mirror the same way it hid it from the two consumers before maize-265, so
#   a direct /mnt-rooted run-ctest.sh invocation carries the same exposure that card
#   removed from build-userland.sh and build-demos.sh. Tracked separately as maize-398.
#
#   Consumers (cc-maize.sh, userland/build-userland.sh, demos/build-demos.sh)
#   throttle only. They build no tools at all, they read build/<preset>/{mazm,mzld,
#   maize} and toolchain/{qbe,cproc}, and that same /build exclusion means a mirror
#   never carries the build tree they need. So they run in place on the real tree.
#   maize-263 D15 drew that line for cc-maize.sh and maize-265 drew it for the other
#   two, which had mirrored and then died on "mazm not found" at their first compile.
#
#   maize_apply_throttle         once, near the top, to renice/ionice the whole run
#   maize_native_mirror_run ...  producers + the run-ctest.sh hybrid, once, near the
#                                top (BEFORE argument parsing consumes "$@"), to
#                                re-root the run on WSL-native storage
#   maize_bounded_jobs           at each real parallel-build site, to cap ninja/make
#   maize_is_ci                  to skip the cap + niceness under CI
#
#   A third group sources this file for maize_require_file alone and neither throttles
#   nor mirrors: the maize-313 evidence scripts (stdin-wake-audit.sh,
#   stdin-wake-check.sh, idle-cost-check.sh, negctl/maize-313/run-negative-controls.sh).
#   They are short-lived measurement and audit runs rather than builds, so re-rooting
#   them would move the very tree they are auditing and throttling would perturb the
#   idle-cost numbers they exist to take.
#
# The three problems this addresses (maize-263 diagnosis): a repo living on the
# Windows drive makes every WSL file operation cross the 9P bridge (and get
# Defender-scanned); ninja/make run with unbounded parallelism on every core; and
# each fresh agent worktree cold-rebuilds the toolchain. See the maize-263 spec.
#
# maize-304 note: a fourth problem was diagnosed here: on Windows/MSYS git-bash, TWO
# independent heavy builds (e.g. two agent-worktree `run-ctest.sh` invocations, each
# compiling dozens of quesOS fixtures through the fork-heavy cc-maize.sh pipeline)
# running at the same time can exhaust the host's MSYS fork-emulation resource pool
# (`dofork ... Resource temporarily unavailable`), stalling one of them for 35+ minutes
# with no bound. An earlier maize-304 cycle added a machine-wide mkdir-based build gate
# to this file to serialize around exactly those sections; the operator decided to drop
# it (maize-304 comment #3133) and keep only the fail-fast `timeout` wrap that lives in
# run-ctest.sh's `cc_maize_compile_bounded`. There is no cross-process lock here: heavy
# Test-stage runs are expected to be run one at a time by orchestration discipline, and
# the timeout wrap bounds the damage if two ever overlap anyway, turning a silent
# unbounded stall into a fast, diagnosed failure instead.
#
# All functions are written to be safe under `set -eu`, which every calling script
# enables: no bare command whose failure should not abort the caller is left
# unguarded, and every conditional uses if/then (a set -e-exempt context) rather
# than a bare `cmd && ...` list.

# maize_is_ci: true (exit 0) when running under a CI runner. GitHub Actions sets
# both CI and GITHUB_ACTIONS; either suffices. Used only in `if` conditions.
maize_is_ci() {
    [ -n "${CI:-}" ] || [ -n "${GITHUB_ACTIONS:-}" ]
}

# maize_require_file <path> [<path>...]
#   Return 0 when every named path exists as a regular file and is readable. Otherwise
#   echo the FIRST path that is not, on stdout, and return 1.
#
#   maize-313: this exists because grep(1) reports three outcomes and most callers read
#   only two. It exits 0 on a match, 1 on no match and 2 when it cannot read the file, so
#   `if grep -q PATTERN file; then fail; else pass; fi` prints a pass for a file that was
#   renamed or deleted. A grep in a pipeline is worse still, since the pipeline carries
#   the LAST command's status and the missing file's 2 is discarded outright. The same
#   shape reached the tree six times on one branch, each time in an audit whose whole
#   purpose was to notice that a named file had stopped saying something.
#
#   So the guard is separated from the check: ask this function whether the evidence is
#   readable, and only then run the matcher whose two remaining outcomes are the real
#   answer. Callers report the unreadable case in their own vocabulary, because a build
#   harness wants to abort where an audit wants to record that the check did not run and
#   says nothing either way. Echoing the offending path rather than a message is what
#   lets them do that.
#
#   A directory fails the -f test deliberately: every caller here names a source file or
#   a report, and a directory sitting at that path is a defect rather than evidence.
maize_require_file() {
    for _rf_path in "$@"; do
        if [ ! -f "$_rf_path" ] || [ ! -r "$_rf_path" ]; then
            printf '%s\n' "$_rf_path"
            return 1
        fi
    done
    return 0
}

# maize_host_to_native <path>
#   Echo <path> in the form the built `maize` expects on the HOST side of a --mount
#   grant. Under MSYS/MinGW/Cygwin the built maize is a native Windows exe, so a POSIX
#   /c/... path must become a Windows C:\... path (cygpath -w); on every other platform
#   the path is echoed unchanged. The GUEST side of the grant is never passed through
#   here: it stays a *nix path, and on MSYS it is protected from argv translation by
#   MSYS2_ARG_CONV_EXCL at the call sites that need it.
#
#   maize-442: this body used to live only in run-ctest.sh, so the four other harness
#   scripts that build --mount arguments had no conversion at all. Nine stdin_wake legs
#   failed on windows-llvm-mingw-debug for that reason, and two more call sites carried
#   the same defect dormantly. MSYS's automatic argv translation cannot cover for a
#   missing call here: the --mount value is a compound HOST=GUEST:MODE string, and the
#   embedded colon makes the heuristic leave the whole argument untranslated. The body
#   lives in one place now so a fix cannot land in one copy and miss another.
maize_host_to_native() {
    case "$(uname -s)" in
        MINGW*|MSYS*|CYGWIN*) cygpath -w "$1" ;;
        *) printf '%s' "$1" ;;
    esac
}

# maize_nproc: echo the logical core count. nproc on Linux/WSL, sysctl on macOS,
# empty when neither is available (the caller then falls back to a fixed default).
maize_nproc() {
    if command -v nproc >/dev/null 2>&1; then
        nproc
    elif command -v sysctl >/dev/null 2>&1; then
        sysctl -n hw.ncpu 2>/dev/null
    else
        echo ""
    fi
}

# maize_bounded_jobs: echo the capped build-job count, max(2, nproc - 2), leaving at
# least two cores free for the operator's foreground work. Falls back to 4 when the
# core count cannot be determined.
maize_bounded_jobs() {
    _n=$(maize_nproc)
    case "$_n" in
        ''|*[!0-9]*) echo 4; return 0 ;;
    esac
    _j=$((_n - 2))
    if [ "$_j" -lt 2 ]; then
        _j=2
    fi
    echo "$_j"
}

# maize_apply_throttle: lower the CURRENT process's CPU and IO priority so every
# child (make/ninja/cpp/cproc/qbe/mazm/maize) inherits it via fork. One call at the
# top of a script covers the whole run. Best-effort: a missing renice/ionice is
# skipped silently (not every platform ships ionice). No-op under CI or when
# MAIZE_SKIP_NICE=1 is set.
maize_apply_throttle() {
    if maize_is_ci; then
        return 0
    fi
    if [ "${MAIZE_SKIP_NICE:-}" = "1" ]; then
        return 0
    fi
    if command -v renice >/dev/null 2>&1; then
        renice -n 10 -p $$ >/dev/null 2>&1 || true
    fi
    if command -v ionice >/dev/null 2>&1; then
        ionice -c 3 -p $$ >/dev/null 2>&1 || true
    fi
    return 0
}

# maize_sha256: read stdin, echo its lowercase hex sha256 (no trailing filename).
# Returns 1 when no sha256 tool is available, so a caller computing a cache key can
# treat an empty key as "caching unavailable" and fall back to always building.
maize_sha256() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum | cut -d' ' -f1
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 | cut -d' ' -f1
    else
        return 1
    fi
}

# maize_pinned_sha <repo_root> <submodule_relpath>
#   Echo the submodule's PINNED commit as recorded in the superproject tree
#   (git rev-parse HEAD:<relpath>), or "" if it cannot be read. Two properties matter:
#     - It reads the pin from the SUPERPROJECT tree, so it works even when the
#       submodule is not checked out (no gitlink needed).
#     - It tries native `git` first, then falls back to Windows `git.exe` (with a
#       wslpath-translated path). Card agents run in LINKED worktrees created by
#       Windows git, whose top-level .git gitdir is an ABSOLUTE Windows path
#       (C:/...): native WSL git cannot resolve that chain, but git.exe can. Without
#       this fallback every submodule SHA silently degrades to a fallback label (the
#       finding this fix pass addresses), so a re-pin would not roll the cache key.
#   MUST be called on the SOURCE side (a real repo/worktree), not inside the git-less
#   mirror; the precompute below runs it there and passes the result via env.
maize_pinned_sha() {
    _psr="$1"
    _psrel="$2"
    _pssha=$(git -C "$_psr" rev-parse "HEAD:${_psrel}" 2>/dev/null) || _pssha=""
    if [ -z "$_pssha" ] && command -v git.exe >/dev/null 2>&1 && command -v wslpath >/dev/null 2>&1; then
        _pswin=$(wslpath -w "$_psr" 2>/dev/null) || _pswin=""
        if [ -n "$_pswin" ]; then
            _pssha=$(git.exe -C "$_pswin" rev-parse "HEAD:${_psrel}" 2>/dev/null | tr -d '\r') || _pssha=""
        fi
    fi
    printf '%s' "$_pssha"
}

# maize_precompute_submodule_keys <repo_root>
#   Compute each vendored submodule's pinned commit on the SOURCE side and export it
#   as MAIZE_KEY_<NAME>, so the mirrored child (a plain, git-less file tree per D14)
#   reads the SHA from the environment instead of running git against a broken
#   in-mirror gitlink. Idempotent: an already-set value (inherited by the mirrored
#   child, or a deliberate operator override) is kept, never recomputed, so the child
#   never runs a doomed in-mirror git call. Safe to call from any entry point before
#   maize_native_mirror_run. Absent/unreadable submodules yield an empty key (there is
#   nothing to build from them anyway; the cache-key label fallback then applies).
maize_precompute_submodule_keys() {
    _rr="$1"
    : "${MAIZE_KEY_QBE:=$(maize_pinned_sha "$_rr" toolchain/qbe)}"
    : "${MAIZE_KEY_CPROC:=$(maize_pinned_sha "$_rr" toolchain/cproc)}"
    : "${MAIZE_KEY_SBASE:=$(maize_pinned_sha "$_rr" userland/sbase)}"
    : "${MAIZE_KEY_OKSH:=$(maize_pinned_sha "$_rr" userland/oksh)}"
    export MAIZE_KEY_QBE MAIZE_KEY_CPROC MAIZE_KEY_SBASE MAIZE_KEY_OKSH
}

# maize_sync_back_artifacts <mirror_dir> <repo_root>
#   After a mirrored run, copy the small, named allowlist of conventionally-located
#   binaries the mirror produced back to their in-tree locations under repo_root, so
#   any follow-on command that expects build/<preset>/<tool> or toolchain/{qbe,cproc}
#   at the conventional path still finds it (maize-263 decision D12). Copies ONLY the
#   allowlist; leaves every other scratch (test-run/, ctest-run/, ...) mirror-only.
#
#   Best-effort per file, but returns nonzero if ANY copy failed, so its guarded
#   caller (`|| echo WARNING`) can surface a partial sync-back. It is ALWAYS invoked
#   in a `|| ...` context, which suppresses `set -e` inside it, so a failing cp here
#   continues to the next rather than aborting the whole run mid-sync.
maize_sync_back_artifacts() {
    _mirror_dir="$1"
    _repo_root="$2"
    _fail=0

    # build/<preset>/{maize,maizeg,mazm,mzld,mzdis}[.exe] for every preset the run
    # actually built. maizeg is included per D12/OQ 9388: CMakeLists.txt builds it
    # unconditionally (MAIZE_DISPLAY gates only SDL2 linkage), and the operator
    # always wants a display-capable build on hand.
    if [ -d "${_mirror_dir}/build" ]; then
        for _preset_dir in "${_mirror_dir}/build"/*/; do
            [ -d "$_preset_dir" ] || continue
            _preset=$(basename "$_preset_dir")
            _dest="${_repo_root}/build/${_preset}"
            for _b in maize maizeg mazm mzld mzdis; do
                for _cand in "${_preset_dir}${_b}" "${_preset_dir}${_b}.exe"; do
                    if [ -f "$_cand" ]; then
                        mkdir -p "$_dest" || { _fail=1; continue; }
                        cp -p "$_cand" "${_dest}/$(basename "$_cand")" || _fail=1
                    fi
                done
            done
        done
    fi

    # toolchain/qbe/obj/qbe[.exe] and toolchain/cproc/{cproc,cproc-qbe}[.exe], so a
    # later non-mirrored invocation (MAIZE_NO_NATIVE_MIRROR=1, or a tool reading these
    # paths directly) still finds them populated in-tree.
    for _rel in toolchain/qbe/obj/qbe toolchain/cproc/cproc toolchain/cproc/cproc-qbe; do
        for _cand in "${_mirror_dir}/${_rel}" "${_mirror_dir}/${_rel}.exe"; do
            if [ -f "$_cand" ]; then
                _destdir="${_repo_root}/$(dirname "$_rel")"
                mkdir -p "$_destdir" || { _fail=1; continue; }
                cp -p "$_cand" "${_destdir}/$(basename "$_cand")" || _fail=1
            fi
        done
    done

    return "$_fail"
}

# maize_native_mirror_run <repo_root> <script_dir> <script_basename> -- "$@"
#   When the calling script's repo lives under /mnt/ (the WSL 9P-bridge signature),
#   rsync a WSL-native mirror of the source tree keyed by sha1(repo_root) and re-run
#   a fresh copy of the SAME script from inside that mirror as a FOREGROUND child
#   (not exec: the sync-back below must run after the child exits, on success OR
#   failure), then sync the named artifact allowlist back and propagate the child's
#   exit code. On a mirror it never returns (it exits with the child's code); when
#   mirroring does not apply or a precondition fails it RETURNS so the caller runs
#   in-place exactly as before (decision D5, loud-and-unconditional fallback).
#
#   MUST be called BEFORE the caller parses/consumes "$@", so the original argument
#   vector reaches the mirrored child intact (a single-source cc-maize.sh invocation,
#   a --preset override, a --out dir all ride "$@").
maize_native_mirror_run() {
    _repo_root="$1"
    _script_dir="$2"
    _script_basename="$3"
    shift 3
    if [ "${1:-}" = "--" ]; then
        shift
    fi
    # "$@" is now exactly the original argument vector for the child.

    # Not applicable: repo is not on the 9P bridge. In-place is normal and correct
    # here (native Linux, native macOS, CI runners), so return silently.
    case "$_repo_root" in
        /mnt/*) : ;;
        *) return 0 ;;
    esac

    # Already inside a mirrored run (an outer script re-rooted us and exported the
    # flag). Do not re-mirror or re-run; return so the in-mirror work proceeds.
    if [ "${MAIZE_NATIVE_MIRROR_ACTIVE:-}" = "1" ]; then
        return 0
    fi

    # Explicit operator opt-out. Loud (D5): the operator forcing in-place under /mnt/
    # is eating the 9P tax on purpose and should see why the run is slow.
    if [ "${MAIZE_NO_NATIVE_MIRROR:-}" = "1" ]; then
        echo "WARNING: MAIZE_NO_NATIVE_MIRROR=1 set; running in-place on ${_repo_root} (slow under WSL's 9P bridge; unset it to restore the native mirror)." >&2
        return 0
    fi

    if ! command -v rsync >/dev/null 2>&1; then
        echo "WARNING: native mirror wanted (repo under /mnt/) but rsync is not on PATH; continuing in-place on ${_repo_root} (slow under WSL's 9P bridge)." >&2
        return 0
    fi

    if command -v sha1sum >/dev/null 2>&1; then
        _key=$(printf '%s' "$_repo_root" | sha1sum | cut -c1-16)
    else
        _key=$(printf '%s' "$_repo_root" | cksum | cut -d' ' -f1)
    fi
    _mirror_root="${MAIZE_NATIVE_MIRROR_ROOT:-$HOME/.cache/maize/mirrors}"
    _mirror_dir="${_mirror_root}/${_key}"

    if ! mkdir -p "$_mirror_dir"; then
        echo "WARNING: could not create native-mirror dir ${_mirror_dir}; continuing in-place on ${_repo_root}." >&2
        return 0
    fi

    # Exclude list (decision D11, revised by D14), hardcoded (it cannot be derived
    # from .gitignore alone: .claude/worktrees rides .git/info/exclude). .git is now
    # EXCLUDED (D14 reverses D4's inclusion): every card agent runs in a LINKED git
    # worktree whose .git is a pointer FILE into the main repo's .git/worktrees/<name>,
    # and the submodule gitlinks resolve through .git/modules storage that lives
    # OUTSIDE the mirrored tree, so a mirrored .git is a BROKEN pointer that makes
    # `git -C <mirror>/toolchain/qbe ...` fail. Instead the mirror is a plain, git-less
    # file tree: submodule SHAs are precomputed host-side (maize_precompute_submodule_
    # keys) and passed via MAIZE_KEY_* env, and apply-maize-qbe-target.sh's `git apply`
    # runs repo-less against the plain files. The unanchored `.git` match also drops
    # each submodule's gitlink file. `.gitignore`/`.gitmodules` (different names) stay.
    # maize-382: bracket the rsync with a wall-clock report. This phase runs BEFORE
    # the calling harness's own per-fixture timing starts, so without this line its
    # cost silently folds into whatever the first fixture happens to be. The
    # nested-call no-op path above (MAIZE_NATIVE_MIRROR_ACTIVE already set) returns
    # long before here and correctly prints nothing, because there is no sync to time.
    _mirror_t0=$(date +%s)
    if ! rsync -a --delete \
        --exclude='/build' --exclude='/build-wsl' \
        --exclude='/.toolchains' \
        --exclude='.claude' \
        --exclude='.git' \
        "${_repo_root}/" "${_mirror_dir}/"; then
        _mirror_t1=$(date +%s)
        echo "native mirror sync: $((_mirror_t1 - _mirror_t0))s (FAILED)" >&2
        echo "WARNING: native-mirror rsync failed; continuing in-place on ${_repo_root} (slow under WSL's 9P bridge)." >&2
        return 0
    fi
    _mirror_t1=$(date +%s)
    echo "native mirror sync: $((_mirror_t1 - _mirror_t0))s" >&2

    # Written AFTER rsync: --delete would otherwise remove this dest-only file. Read
    # by scripts/prune-native-mirrors.sh to detect orphaned mirrors.
    printf '%s\n' "$_repo_root" > "${_mirror_dir}/.mirror-source" 2>/dev/null || true

    _child="${_mirror_dir}${_script_dir#$_repo_root}/${_script_basename}"
    if [ ! -f "$_child" ]; then
        echo "WARNING: mirrored script ${_child} not found after rsync; continuing in-place on ${_repo_root}." >&2
        return 0
    fi

    export MAIZE_NATIVE_MIRROR_ACTIVE=1

    # set -e safety (D12/OQ 9387): the child MUST be the condition of an `if`, never
    # a bare statement. Under `set -e` a plain failing command aborts the whole
    # process at its line, so a bare `child; rc=$?; sync_back; exit "$rc"` would never
    # reach the sync-back/exit tail on a failing child (the routine case during
    # iteration, and exactly when synced-back binaries matter most for debugging).
    if "$_child" "$@"; then
        _rc=0
    else
        _rc=$?
    fi

    # Guarded, not bare (D12): an unguarded failing sync-back would itself trip
    # `set -e` here and abort BEFORE `exit "$_rc"`, discarding the child's real code.
    maize_sync_back_artifacts "$_mirror_dir" "$_repo_root" \
        || echo "WARNING: artifact sync-back failed; build/toolchain binaries under ${_repo_root} may be stale." >&2

    exit "$_rc"
}
