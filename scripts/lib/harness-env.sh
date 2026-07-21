# shellcheck shell=sh
# harness-env.sh (maize-263): shared helpers for the test/build harness scripts.
#
# This file is SOURCED, never executed directly (it defines functions and sets no
# top-level state). The six POSIX-sh harness entry points (run-tests.sh,
# run-ctest.sh, build-toolchain.sh, cc-maize.sh, userland/build-userland.sh,
# demos/build-demos.sh) source it and call:
#
#   maize_apply_throttle         once, near the top, to renice/ionice the whole run
#   maize_native_mirror_run ...  once, near the top (BEFORE argument parsing consumes
#                                "$@"), to re-root the run on WSL-native storage
#   maize_bounded_jobs           at each real parallel-build site, to cap ninja/make
#   maize_is_ci                  to skip the cap + niceness under CI
#   maize_gate_acquire <name>    around a heavy, fork-dense section that must not run
#   maize_gate_release           concurrently with the SAME section in another
#                                invocation on the same host (maize-304); mkdir-based,
#                                machine-wide, with bounded wait + stale-lock recovery
#
# The three problems this addresses (maize-263 diagnosis): a repo living on the
# Windows drive makes every WSL file operation cross the 9P bridge (and get
# Defender-scanned); ninja/make run with unbounded parallelism on every core; and
# each fresh agent worktree cold-rebuilds the toolchain. See the maize-263 spec.
#
# maize-304 adds a fourth, narrower problem: on Windows/MSYS git-bash, TWO independent
# heavy builds (e.g. two agent-worktree `run-ctest.sh` invocations, each compiling
# dozens of quesOS fixtures through the fork-heavy cc-maize.sh pipeline) running at the
# same time can exhaust the host's MSYS fork-emulation resource pool (`dofork ...
# Resource temporarily unavailable`), stalling one of them for 35+ minutes with no
# bound. `maize_gate_acquire`/`maize_gate_release` give callers a machine-wide mutex to
# serialize around exactly those sections; see the maize-304 spec for the ground-truth
# read that led here.
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

# --- maize-304: machine-wide build gate (mkdir lock, bounded wait, stale recovery) ---
#
# maize_lock_root: echo the lock root directory, honoring MAIZE_LOCK_DIR. Deliberately
# NOT under the repo/worktree (each agent worktree is its own checkout under
# .claude/worktrees/*): the whole point is serializing ACROSS independently-cloned
# worktrees on the same host, so the root must be machine-wide, keyed by nothing
# repo-specific.
maize_lock_root() {
    printf '%s' "${MAIZE_LOCK_DIR:-${TMPDIR:-/tmp}/maize-locks}"
}

# maize_gate_acquire <name>
#   Acquire a machine-wide, named mutex before entering a heavy, fork-dense section
#   (e.g. a quesOS-fixture compile loop) that must not run concurrently with the SAME
#   section in another `run-ctest.sh` invocation on this host (maize-304: two such
#   invocations can exhaust the Windows/MSYS git-bash fork-emulation resource pool at
#   once, `dofork ... Resource temporarily unavailable`, stalling one 35+ minutes with
#   no bound).
#
#   Mechanism: `mkdir` is the lock primitive (atomic on both POSIX and MSYS). `flock`
#   was checked and rejected for this host: this repo's actual harness shell is MSYS
#   git-bash (x86_64-pc-msys) and it does not ship a `flock` binary (confirmed via
#   `command -v flock` at cycle-3 implement time), so a `flock`-based rewrite would
#   trade one bug class for an unconditional "the gate silently does not exist on the
#   platform this card exists for" regression. `mkdir` remains the right primitive.
#
#   The winner records its PID and acquire-time UNIX timestamp inside the lock
#   directory (the "holder" file) so a contending waiter can tell a LIVE holder from a
#   STALE one, left behind by a holder that was SIGKILL'd (or crashed) before it
#   reached its release path:
#     - holder PID no longer running (`kill -0` fails)              -> stale, reclaim now
#     - holder PID alive but older than MAIZE_GATE_STALE_SECONDS     -> stale, reclaim now
#     - lock dir present with no holder file past a short grace window (a crash between
#       `mkdir` and writing the holder file) -> stale, reclaim now
#   A live, fresh holder is waited out with a short poll interval and periodic ("every
#   30s") progress output, so a long wait reads as a wait, not a second silent stall.
#   The total wait is bounded by MAIZE_GATE_WAIT_SECONDS (default 300); exceeding it
#   fails fast with a clear diagnostic (return 1) rather than blocking forever, so a
#   genuinely wedged holder cannot deadlock every other invocation on the host.
#
#   Cycle-2 fix KEPT: the holder-identity write is FORK-FREE for the part that matters
#   (the PID). `printf` and `$$` are shell builtins, so the PID lands on disk with
#   nothing able to fork/stall between `mkdir` succeeding and that write completing.
#   The PREVIOUS (cycle-1) code forked `date` for the timestamp BEFORE the first write
#   ever hit disk; on a fork-starved host (exactly the condition maize-304 exists to
#   handle) that fork could itself stall past the grace window below, so a waiter would
#   reclaim a lock whose holder had not yet gotten around to proving it was there. The
#   timestamp (which does need a `date +%s` fork; POSIX sh has no fork-free clock) is
#   written in a second, separate pass, after the PID is already visible.
#
#   Cycle-2 fix REMOVED (cycle-3 simplification, replacing it rather than patching it
#   again): cycle-2 also fenced the decide-and-evict step behind its OWN atomic mkdir
#   (a `.reclaiming` marker), to stop two waiters both judging a lock stale in the same
#   iteration from double-entering the critical section. That fence introduced a NEW
#   bug (found in cycle-3 code review): its fall-through `continue` paths (both "lost
#   the `.reclaiming` mkdir" and "won it, reclaimed, done") skipped the poll-sleep and
#   the `_gate_elapsed` bump at the bottom of the loop entirely. A `.reclaiming` marker
#   orphaned by a process killed between its own mkdir and rmdir became permanent: every
#   future waiter lost that inner mkdir race forever and looped back to the top with NO
#   sleep and NO elapsed increment, a genuine unbounded 100%-CPU livelock, worse than
#   the stall this card exists to fix. The fence is gone. Nothing replaces it, because
#   nothing needs to: `mkdir "$_gate_dir"` at the top of the loop is ALREADY the single
#   atomic arbiter this gate needs. If two waiters both judge the lock stale in the same
#   instant and both `rm -rf` it, that is harmless: `rm -rf` on an already-removed (or
#   about-to-be-removed) directory is a no-op either way, and NEITHER waiter has entered
#   the critical section yet. The very next `mkdir "$_gate_dir"` (back at the top of
#   EACH waiter's own loop) is what actually decides the one winner, exactly as it does
#   for a fresh, uncontested lock; a loser just sees `mkdir` fail again and waits another
#   poll interval, same as any other contended iteration. A stale-reclaim iteration is
#   now a plain iteration: it always falls through to the shared wait-budget check, the
#   shared progress print, and the shared `sleep` + `_gate_elapsed` bump, so it is
#   provably bounded by MAIZE_GATE_WAIT_SECONDS exactly like every other path through
#   this loop, with no separate code path that can skip the bump.
#
#   On success, installs an EXIT/INT/TERM trap that force-releases the lock even if the
#   caller is killed or errors out before calling maize_gate_release itself (belt and
#   suspenders: this is also exactly what lets THIS invocation clean up after ITSELF
#   if it is later the one that gets SIGKILL'd, so the NEXT invocation reclaims a fresh
#   holder record rather than an ambiguous one). Callers MUST still call
#   maize_gate_release on every normal exit path from the guarded section (including
#   early `return`s on a failure inside it): the trap is the backstop for an abnormal
#   exit, not a substitute for releasing promptly on a normal one, since a lock held
#   until the WHOLE SCRIPT exits blocks every other section (and every other host
#   invocation waiting on the same name) far longer than the guarded section itself
#   runs.
#
#   Returns 0 with the lock held, or 1 (after the diagnostic) when the wait budget is
#   exceeded. Not reentrant: a second maize_gate_acquire for the SAME name from the
#   SAME process before releasing the first would deadlock against itself exactly
#   like any other non-reentrant mutex; callers should not nest gates of the same name.
maize_gate_acquire() {
    _gate_name="$1"
    _gate_root=$(maize_lock_root)
    _gate_dir="${_gate_root}/${_gate_name}.lock"
    _gate_wait="${MAIZE_GATE_WAIT_SECONDS:-300}"
    _gate_stale="${MAIZE_GATE_STALE_SECONDS:-1800}"
    _gate_poll=2
    _gate_elapsed=0
    _gate_last_progress=0

    mkdir -p "$_gate_root" 2>/dev/null || true

    while true; do
        if mkdir "$_gate_dir" 2>/dev/null; then
            # Fork-free PID write FIRST (see the maize_gate_acquire doc comment above):
            # printf and $$ are shell builtins, so nothing can fork/stall between mkdir
            # granting the lock and the holder file proving it. The timestamp (which
            # does need a `date` fork) is appended in a second write right after; the
            # presence-based grace window below only cares that the PID landed fast.
            printf '%s\n' "$$" > "${_gate_dir}/holder" 2>/dev/null || true
            printf '%s %s\n' "$$" "$(date +%s)" > "${_gate_dir}/holder" 2>/dev/null || true
            MAIZE_GATE_HELD_DIR="$_gate_dir"
            trap 'maize_gate_release' EXIT INT TERM
            return 0
        fi

        _holder_pid=""
        _holder_time=""
        if [ -f "${_gate_dir}/holder" ]; then
            _holder_pid=$(cut -d' ' -f1 "${_gate_dir}/holder" 2>/dev/null) || _holder_pid=""
            # `-s` (POSIX cut: suppress lines containing no delimiter) is required
            # here, not optional: the fork-free write above lands the PID-only line
            # FIRST, then a second write appends " <timestamp>". During the (normally
            # sub-millisecond) window between those two writes, this file has ONE
            # field and NO space. Plain `cut -d' ' -f2` on a delimiter-less line
            # passes the WHOLE line through unchanged (POSIX-mandated, confirmed on
            # this host), so without `-s` this would read back the PID itself as
            # "_holder_time", and the age arithmetic below (`now - holder_time`) would
            # then compute a huge bogus age against a live holder and misreport it
            # stale. Caught while writing this cycle's self-test (test 4: a live,
            # delayed-write holder was getting reclaimed); `-s` makes an in-flight
            # PID-only line read back as an empty (not-yet-aged) timestamp instead.
            _holder_time=$(cut -s -d' ' -f2 "${_gate_dir}/holder" 2>/dev/null) || _holder_time=""
        fi

        _stale=0
        if [ -n "$_holder_pid" ]; then
            if ! kill -0 "$_holder_pid" 2>/dev/null; then
                _stale=1
            else
                # Only trust _holder_time for arithmetic once it is confirmed to be a
                # plain non-negative integer (mirrors maize_bounded_jobs's own
                # non-numeric-input guard above): a corrupted/partial holder-file write
                # (e.g. a crash mid-write) must not reach `$((...))` with a non-numeric
                # operand, which is a hard arithmetic error under `set -eu`. Treat it the
                # same as "no timestamp recorded" (a live, un-aged holder; not reclaimed
                # on age, only on the pid-liveness check above).
                case "$_holder_time" in
                    ''|*[!0-9]*) : ;;
                    *)
                        _now=$(date +%s)
                        _age=$((_now - _holder_time))
                        if [ "$_age" -gt "$_gate_stale" ]; then
                            _stale=1
                        fi
                        ;;
                esac
            fi
        elif [ "$_gate_elapsed" -ge 5 ]; then
            # Lock dir exists but no holder file appeared within a 5s grace window:
            # the winner crashed between mkdir and the holder-file write. Reclaim
            # rather than wait out the full budget on a lock nobody will ever finish
            # writing.
            _stale=1
        fi

        if [ "$_stale" -eq 1 ]; then
            # No fence needed (cycle-3: see the doc comment above for why the cycle-2
            # `.reclaiming` fence was removed, not repaired). `rm -rf` here is
            # best-effort and unconditional: if another waiter beats us to it, or has
            # already removed it, this is a harmless no-op either way, and the real
            # arbiter is the `mkdir "$_gate_dir"` retry at the top of the NEXT loop
            # iteration, not this step. Deliberately falls through to the shared
            # wait-budget check / progress print / sleep below rather than an early
            # `continue`, so this path is bounded exactly like every other iteration.
            echo "run-ctest.sh: reclaiming stale ${_gate_name} lock (${_gate_dir}, holder pid ${_holder_pid:-unknown} gone or older than ${_gate_stale}s)" >&2
            rm -rf "$_gate_dir" 2>/dev/null || true
        fi

        if [ "$_gate_elapsed" -ge "$_gate_wait" ]; then
            echo "run-ctest.sh: [FAIL] could not acquire the ${_gate_name} build gate after ${_gate_wait}s; another concurrent heavy build (pid ${_holder_pid:-unknown}) may be stuck." >&2
            return 1
        fi

        if [ $((_gate_elapsed - _gate_last_progress)) -ge 30 ]; then
            echo "run-ctest.sh: waiting for the ${_gate_name} build gate (${_gate_elapsed}s elapsed, holder pid ${_holder_pid:-unknown})..." >&2
            _gate_last_progress="$_gate_elapsed"
        fi

        sleep "$_gate_poll"
        _gate_elapsed=$((_gate_elapsed + _gate_poll))
    done
}

# maize_gate_release: release the lock most recently acquired by maize_gate_acquire in
# THIS process and clear the EXIT/INT/TERM trap it installed. Idempotent (a second call
# with no held lock is a silent no-op), so it is safe to call both explicitly at the
# end of a guarded section AND let the EXIT trap fire harmlessly afterward.
maize_gate_release() {
    if [ -n "${MAIZE_GATE_HELD_DIR:-}" ]; then
        rm -rf "${MAIZE_GATE_HELD_DIR}" 2>/dev/null || true
        MAIZE_GATE_HELD_DIR=""
    fi
    trap - EXIT INT TERM
}
