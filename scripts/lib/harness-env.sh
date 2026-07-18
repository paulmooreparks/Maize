# shellcheck shell=sh
# harness-env.sh (maize-263): shared helpers for the test/build harness scripts.
#
# This file is SOURCED, never executed directly (it defines functions and sets no
# top-level state). The five POSIX-sh harness entry points (run-tests.sh,
# run-ctest.sh, build-toolchain.sh, cc-maize.sh, userland/build-userland.sh) source
# it and call:
#
#   maize_apply_throttle         once, near the top, to renice/ionice the whole run
#   maize_native_mirror_run ...  once, near the top (BEFORE argument parsing consumes
#                                "$@"), to re-root the run on WSL-native storage
#   maize_bounded_jobs           at each real parallel-build site, to cap ninja/make
#   maize_is_ci                  to skip the cap + niceness under CI
#
# The three problems this addresses (maize-263 diagnosis): a repo living on the
# Windows drive makes every WSL file operation cross the 9P bridge (and get
# Defender-scanned); ninja/make run with unbounded parallelism on every core; and
# each fresh agent worktree cold-rebuilds the toolchain. See the maize-263 spec.
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
    if ! rsync -a --delete \
        --exclude='/build' --exclude='/build-wsl' \
        --exclude='/.toolchains' \
        --exclude='.claude' \
        --exclude='.git' \
        "${_repo_root}/" "${_mirror_dir}/"; then
        echo "WARNING: native-mirror rsync failed; continuing in-place on ${_repo_root} (slow under WSL's 9P bridge)." >&2
        return 0
    fi

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
