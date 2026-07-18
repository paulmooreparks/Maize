#!/bin/sh
# prune-native-mirrors.sh (maize-263): remove stale WSL-native mirror directories.
#
# maize_native_mirror_run (scripts/lib/harness-env.sh) keeps one native mirror per
# source-worktree path under ${MAIZE_NATIVE_MIRROR_ROOT:-$HOME/.cache/maize/mirrors},
# each carrying a .mirror-source file naming the worktree it mirrors. Agent worktrees
# churn (created and removed per card), so those mirrors accumulate. This script
# removes a mirror whose source worktree no longer exists on disk.
#
# It is NOT wired into any other script; the operator (or a future card) decides when
# to run or cron it.
#
# Usage: scripts/prune-native-mirrors.sh [--dry-run] [--all]
#   --dry-run  report what WOULD be removed, remove nothing.
#   --all      remove every mirror dir regardless of source liveness (nuke-and-rebuild).
#
# Exit codes: 0 always on a normal run (nothing to prune is not an error).
set -eu

DRY_RUN=0
ALL=0
while [ $# -gt 0 ]; do
    case "$1" in
        --dry-run) DRY_RUN=1; shift ;;
        --all) ALL=1; shift ;;
        -h|--help)
            echo "usage: prune-native-mirrors.sh [--dry-run] [--all]"
            exit 0
            ;;
        *) echo "prune-native-mirrors.sh: unknown argument: $1" >&2; exit 2 ;;
    esac
done

MIRROR_ROOT="${MAIZE_NATIVE_MIRROR_ROOT:-$HOME/.cache/maize/mirrors}"
if [ ! -d "$MIRROR_ROOT" ]; then
    echo "no mirror root at ${MIRROR_ROOT}; nothing to prune."
    exit 0
fi

removed=0
kept=0
for d in "$MIRROR_ROOT"/*/; do
    [ -d "$d" ] || continue
    d=${d%/}

    if [ "$ALL" -eq 1 ]; then
        reason="--all"
        orphan=1
    else
        src=""
        if [ -f "${d}/.mirror-source" ]; then
            src=$(cat "${d}/.mirror-source")
        fi
        if [ -n "$src" ] && [ -e "$src" ]; then
            orphan=0
        else
            orphan=1
            if [ -z "$src" ]; then
                reason="no .mirror-source"
            else
                reason="source gone: ${src}"
            fi
        fi
    fi

    if [ "$orphan" -eq 0 ]; then
        kept=$((kept + 1))
        continue
    fi

    if [ "$DRY_RUN" -eq 1 ]; then
        echo "would remove ${d} (${reason})"
    else
        rm -rf "$d"
        echo "removed ${d} (${reason})"
    fi
    removed=$((removed + 1))
done

if [ "$DRY_RUN" -eq 1 ]; then
    echo "prune-native-mirrors.sh: ${removed} mirror(s) would be removed, ${kept} kept (dry run)."
else
    echo "prune-native-mirrors.sh: ${removed} mirror(s) removed, ${kept} kept."
fi
exit 0
