#!/usr/bin/env bash
# Build the Maize toolchain (maize, maizeg, mazm, mzld, mzdis) and install stable copies into ~/bin (Linux/WSL/macOS).
# Counterpart of install-mazm.ps1; wired to the default build task via
# .vscode/tasks.json. Never prompts.

set -euo pipefail

PRESET="${1:-linux-release}"
INSTALL_DIR="${2:-$HOME/bin}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build/$PRESET"

if ! command -v cmake >/dev/null 2>&1; then
    echo "error: cmake not found on PATH" >&2
    exit 2
fi

# Enable the maize SDL2 window backend (--display) when system SDL2 dev files are
# present; otherwise build headless rather than failing the task on a server host.
# MAIZE_DISPLAY is passed EXPLICITLY either way: a bare configure would inherit a
# stale MAIZE_DISPLAY=ON from a prior CMakeCache and then hard-fail find_package(SDL2)
# once system SDL2 went missing. (On Windows install-mazm.ps1 instead auto-fetches a
# vendored SDL2; on Linux/WSL, SDL2 comes from the system package manager.)
if command -v sdl2-config >/dev/null 2>&1 || pkg-config --exists sdl2 2>/dev/null; then
    display_args=(-DMAIZE_DISPLAY=ON)
    echo "SDL2 found; building maizeg with the --display window backend."
else
    display_args=(-DMAIZE_DISPLAY=OFF)
    echo "note: SDL2 dev files not found; building headless (no --display window). Install libsdl2-dev to enable it." >&2
fi

# Always reconfigure (idempotent) so the display cache var is applied even to a build
# directory first configured without it.
echo "Configuring preset '$PRESET'..."
cmake --preset "$PRESET" "${display_args[@]}"

echo "Building maize, maizeg, mazm, mzld, mzdis, mzcc ($PRESET)..."
cmake --build "$BUILD_DIR" --target maize maizeg mazm mzld mzdis mzcc

mkdir -p "$INSTALL_DIR"
# maize-217/230: `maize` is the console-subsystem VM (terminal I/O); `maizeg` is the graphical
# one (SDL window). Both are installed; console programs run under maize, the screen under maizeg.
# maize-278: mzcc is the compiled C guest-build driver (the native cc-maize.sh replacement).
for tool in maize maizeg mazm mzld mzdis mzcc; do
    cp "$BUILD_DIR/$tool" "$INSTALL_DIR/$tool"
    # cp preserves the source artifact's mtime, so an up-to-date incremental
    # reinstall would leave an old timestamp on the installed copy and look
    # stale. Stamp it to now so a completed install always shows fresh (maize-366).
    touch "$INSTALL_DIR/$tool"
    echo "Installed $BUILD_DIR/$tool -> $INSTALL_DIR/$tool"
done

case ":$PATH:" in
    *":$INSTALL_DIR:"*) ;;
    *) echo "note: $INSTALL_DIR is not on PATH; add it to your shell profile." ;;
esac

# Deliberately-broken stdin probe: proves the installed binary supports the
# editor's --stdin diagnostics path, independent of repo .mazm state.
set +e
probe_out=$(printf 'STRING "x\n' | "$INSTALL_DIR/mazm" --check --stdin --base-path /tmp --source-name mazm-install-probe 2>&1)
probe_rc=$?
set -e

if [ "$probe_rc" -ne 1 ] || ! printf '%s' "$probe_out" | grep -q 'mazm-install-probe:1: error:'; then
    echo "error: installed mazm failed the --stdin probe smoke test (exit $probe_rc)" >&2
    exit 1
fi

# mzld smoke: no inputs prints the usage line to stderr and exits 1.
set +e
ld_out=$("$INSTALL_DIR/mzld" 2>&1)
ld_rc=$?
set -e

if [ "$ld_rc" -ne 1 ] || ! printf '%s' "$ld_out" | grep -q 'usage: mzld'; then
    echo "error: installed mzld failed the usage smoke test (exit $ld_rc)" >&2
    exit 1
fi

# --- C cross-toolchain refresh (cproc/qbe + Maize target) -------------------------
# Keeps `mzcc` current after every build. Non-fatal: the native tools above are
# installed and smoke-checked, so a toolchain hiccup (e.g. no network for the
# submodule fetch) only warns rather than failing the build task.
set +e
"$SCRIPT_DIR/refresh-c-toolchain.sh"
tc_rc=$?
set -e
if [ "$tc_rc" -ne 0 ]; then
    echo "warning: C cross-toolchain refresh failed (exit $tc_rc); native tools are installed. Retry with scripts/refresh-c-toolchain.sh." >&2
fi

# Resolve the git revision the tree was built from, for a visible provenance
# stamp in the summary line. git describe --always --dirty yields the nearest
# tag (or abbreviated hash) plus a -dirty suffix when the tree has uncommitted
# changes, in one call. Bracket in set +e / set -e (matching the smoke-check
# idiom above) so a missing git binary or a non-repo checkout degrades to
# "unknown" rather than aborting under set -euo pipefail (maize-366).
set +e
revision="$(git -C "$REPO_ROOT" describe --always --dirty 2>/dev/null)"
set -e
if [ -z "$revision" ]; then
    revision="unknown"
fi

echo "Installed maize, maizeg, mazm, mzld, mzdis, mzcc to $INSTALL_DIR (built from $revision)."
