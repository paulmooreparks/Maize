#!/usr/bin/env bash
# Build mazm and install a stable copy into ~/bin (Linux/WSL/macOS).
# Counterpart of install-mazm.ps1; wired to the default build task via
# .vscode/tasks.json. Never prompts.

set -euo pipefail

PRESET="${1:-linux-debug}"
INSTALL_DIR="${2:-$HOME/bin}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build/$PRESET"

if ! command -v cmake >/dev/null 2>&1; then
    echo "error: cmake not found on PATH" >&2
    exit 2
fi

if [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
    echo "Configuring preset '$PRESET'..."
    cmake --preset "$PRESET"
fi

echo "Building mazm ($PRESET)..."
cmake --build "$BUILD_DIR" --target mazm

mkdir -p "$INSTALL_DIR"
cp "$BUILD_DIR/mazm" "$INSTALL_DIR/mazm"
echo "Installed $BUILD_DIR/mazm -> $INSTALL_DIR/mazm"

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

echo "mazm installed and smoke-checked (stdin diagnostics probe passed)."
