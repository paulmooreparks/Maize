#!/usr/bin/env bash
# Build the Maize toolchain (maize, mazm, mzld, mzdis) and install stable copies into ~/bin (Linux/WSL/macOS).
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
display_args=()
if command -v sdl2-config >/dev/null 2>&1 || pkg-config --exists sdl2 2>/dev/null; then
    display_args=(-DMAIZE_DISPLAY=ON)
    echo "SDL2 found; building maize with the --display window backend."
else
    echo "note: SDL2 dev files not found; building headless (no --display window)." >&2
fi

# Always reconfigure (idempotent) so the display cache var is applied even to a build
# directory first configured without it.
echo "Configuring preset '$PRESET'..."
cmake --preset "$PRESET" "${display_args[@]}"

echo "Building maize, mazm, mzld, mzdis ($PRESET)..."
cmake --build "$BUILD_DIR" --target maize mazm mzld mzdis

mkdir -p "$INSTALL_DIR"
for tool in maize mazm mzld mzdis; do
    cp "$BUILD_DIR/$tool" "$INSTALL_DIR/$tool"
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

echo "maize, mazm, mzld, mzdis installed and smoke-checked."
