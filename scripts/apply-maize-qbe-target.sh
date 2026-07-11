#!/bin/sh
# Overlay the Maize QBE target onto the pinned qbe submodule (maize-62, decision 6637).
#
# The qbe submodule stays pinned at its exact upstream commit (maize-61 decisions
# 6409/6610, preserving the auditable-upstream property). The Maize target source
# lives in the Maize repo under toolchain/qbe-maize/ and is overlaid here:
#
#   1. copy the target sources into toolchain/qbe/maize/
#   2. apply a minimal registration patch to qbe's main.c / all.h / Makefile
#      (target table + `-t maize` dispatch + data-emitter hook + obj list)
#
# Both steps are idempotent, so a fresh checkout and a re-run behave identically.
# Documented fallback if the overlay/patch is ever fragile on a platform: repoint
# the submodule at a Maize-controlled qbe fork carrying the target (decision 6637).
#
# Exit codes:
#   0 - overlay present (copied + patched, or already applied)
#   2 - environment/setup failure (submodule not initialized)
#   1 - the registration patch does not apply to this qbe checkout

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "${SCRIPT_DIR}/.." && pwd)
QBE_DIR="${REPO_ROOT}/toolchain/qbe"
SRC_DIR="${REPO_ROOT}/toolchain/qbe-maize"
PATCH="${SRC_DIR}/qbe-registration.patch"

if [ ! -f "${QBE_DIR}/Makefile" ]; then
    echo "apply-maize-qbe-target.sh: qbe submodule not initialized." >&2
    echo "  run: git submodule update --init --recursive" >&2
    exit 2
fi

# 1. Overlay the target sources.
mkdir -p "${QBE_DIR}/maize"
for f in all.h targ.c abi.c isel.c emit.c data.c; do
    cp "${SRC_DIR}/${f}" "${QBE_DIR}/maize/${f}"
done

# 2. Apply the registration patch, robustly and idempotently.
# Restore the submodule's TRACKED files to their pinned state first, so the patch
# always applies to a pristine checkout. This keeps the overlay robust to a CHANGED
# registration patch: a checkout still carrying an OLDER version of the patch (e.g.
# a local clone patched before maize-102 grew the patch with the hlt hunks) would
# otherwise fail BOTH the reverse-check (new hunks absent) AND the forward-check
# (old hunks already present) and abort. Only tracked files are reset; the untracked
# maize/ overlay (copied above) and obj/ build artifacts are preserved. A fresh
# checkout has nothing to restore, so this is a no-op there (matching CI).
git -C "${QBE_DIR}" checkout -- . 2>/dev/null || true
if git -C "${QBE_DIR}" apply --check "${PATCH}" >/dev/null 2>&1; then
    git -C "${QBE_DIR}" apply "${PATCH}"
    echo "apply-maize-qbe-target.sh: registration patch applied (pinned checkout restored first)."
else
    echo "apply-maize-qbe-target.sh: registration patch does not apply to this qbe checkout." >&2
    echo "  the submodule may not be pinned at the expected commit; see toolchain/qbe-maize/README.md." >&2
    exit 1
fi

echo "Maize QBE target overlaid onto ${QBE_DIR}."
