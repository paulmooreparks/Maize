#!/bin/sh
# Apply the Maize cproc source-patch overlay onto the pinned cproc submodule
# (maize-297), mirroring apply-maize-qbe-target.sh's overlay mechanism.
#
# The cproc submodule stays pinned at its exact upstream commit
# (toolchain/VENDORING.md), preserving the auditable-upstream property. Small,
# targeted correctness fixes to the vendored frontend land as patch files
# under toolchain/cproc-patches/, applied here at build time rather than
# committed into the submodule.
#
# Currently carries:
#   0001-typecommonreal-unsigned-llong.patch - typecommonreal()'s
#     TYPELLONG arm of the equal-width unsigned-vs-signed usual-arithmetic-
#     conversions switch returned the SIGNED type; C11 6.3.1.8 requires the
#     unsigned counterpart (the sibling TYPEINT/TYPELONG arms already do
#     this correctly). See maize-297.
#
# Each patch is applied idempotently: already-applied and cleanly-appliable
# are the two fast paths a fresh checkout (including CI, which reinitializes
# the submodule every run) always takes. The stale-patch recovery branch
# mirrors apply-maize-qbe-target.sh's: only reached by a persisted clone
# carrying an older/different patch revision, and only then does it restore
# the submodule's tracked files to the pinned commit before re-applying.
#
# Exit codes:
#   0 - all patches present (applied, or already applied)
#   2 - environment/setup failure (submodule not initialized)
#   1 - a patch does not apply to this cproc checkout

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "${SCRIPT_DIR}/.." && pwd)
CPROC_DIR="${REPO_ROOT}/toolchain/cproc"
PATCH_DIR="${REPO_ROOT}/toolchain/cproc-patches"

if [ ! -f "${CPROC_DIR}/Makefile" ]; then
    echo "apply-cproc-patches.sh: cproc submodule not initialized." >&2
    echo "  run: git submodule update --init --recursive" >&2
    exit 2
fi

if [ ! -d "${PATCH_DIR}" ]; then
    echo "apply-cproc-patches.sh: no ${PATCH_DIR}; nothing to apply."
    exit 0
fi

for PATCH in "${PATCH_DIR}"/*.patch; do
    [ -e "${PATCH}" ] || continue
    NAME=$(basename "${PATCH}")
    if git -C "${CPROC_DIR}" apply --reverse --check "${PATCH}" >/dev/null 2>&1; then
        echo "apply-cproc-patches.sh: ${NAME} already applied."
    elif git -C "${CPROC_DIR}" apply --check "${PATCH}" >/dev/null 2>&1; then
        git -C "${CPROC_DIR}" apply "${PATCH}"
        echo "apply-cproc-patches.sh: ${NAME} applied."
    else
        echo "apply-cproc-patches.sh: stale/older patch detected for ${NAME}; restoring pinned cproc checkout and re-applying all patches." >&2
        git -C "${CPROC_DIR}" checkout -- . 2>/dev/null || true
        for P2 in "${PATCH_DIR}"/*.patch; do
            [ -e "${P2}" ] || continue
            if git -C "${CPROC_DIR}" apply --check "${P2}" >/dev/null 2>&1; then
                git -C "${CPROC_DIR}" apply "${P2}"
                echo "apply-cproc-patches.sh: $(basename "${P2}") applied (after restoring pinned checkout)."
            else
                echo "apply-cproc-patches.sh: $(basename "${P2}") does not apply to this cproc checkout." >&2
                echo "  the submodule may not be pinned at the expected commit; see toolchain/VENDORING.md." >&2
                exit 1
            fi
        done
        break
    fi
done

echo "Maize cproc patch overlay applied onto ${CPROC_DIR}."
