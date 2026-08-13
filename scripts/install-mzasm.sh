#!/usr/bin/env bash
# Build the Maize v2 binaries (mzvm, mzvmg, mzasm) and install stable copies into
# ~/bin (Linux/WSL/macOS).
# Counterpart of install-mzasm.ps1; wired to the default build task via
# .vscode/tasks.json, which runs this script on every press so the binaries it just
# built are the ones on PATH (maize-454). Never prompts.
#
# maize-454: the installed set is the v2 machine and the v2 assembler only. The frozen
# v1 binaries (maize, maizeg, mazm) are no longer built or copied. mzld, mzdis and mzcc
# keep their names under maize-422 D-1 but have not been ported yet
# (maize-423/424/425/426), so installing today's v1 builds of them would put tools on
# PATH that cannot read a v2 object; each comes back here as its parity card lands. The
# v1 C pipeline (mzcc plus the cproc/qbe cross-toolchain) is behind --with-c-toolchain,
# opt-in, because it is sometimes a real wait and has no business in a loop pressed
# dozens of times a day.
#
# usage: install-mzasm.sh [preset] [install-dir] [--with-c-toolchain]

set -euo pipefail

PRESET=""
INSTALL_DIR=""
WITH_C_TOOLCHAIN=0

for arg in "$@"; do
    case "$arg" in
        --with-c-toolchain) WITH_C_TOOLCHAIN=1 ;;
        -*)
            echo "error: unknown option '$arg'" >&2
            echo "usage: install-mzasm.sh [preset] [install-dir] [--with-c-toolchain]" >&2
            exit 2
            ;;
        *)
            if [ -z "$PRESET" ]; then
                PRESET="$arg"
            elif [ -z "$INSTALL_DIR" ]; then
                INSTALL_DIR="$arg"
            else
                echo "error: unexpected argument '$arg'" >&2
                exit 2
            fi
            ;;
    esac
done

PRESET="${PRESET:-linux-release}"
INSTALL_DIR="${INSTALL_DIR:-$HOME/bin}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build/$PRESET"

if ! command -v cmake >/dev/null 2>&1; then
    echo "error: cmake not found on PATH" >&2
    exit 2
fi

# Enable the mzvmg SDL2 window backend (--display) when system SDL2 dev files are
# present; otherwise build headless rather than failing the task on a server host.
# MAIZE_DISPLAY is passed EXPLICITLY either way: a bare configure would inherit a
# stale MAIZE_DISPLAY=ON from a prior CMakeCache and then hard-fail find_package(SDL2)
# once system SDL2 went missing. (On Windows install-mzasm.ps1 instead auto-fetches a
# vendored SDL2; on Linux/WSL, SDL2 comes from the system package manager.)
if command -v sdl2-config >/dev/null 2>&1 || pkg-config --exists sdl2 2>/dev/null; then
    display_args=(-DMAIZE_DISPLAY=ON)
    echo "SDL2 found; building mzvmg with the --display window backend."
else
    display_args=(-DMAIZE_DISPLAY=OFF)
    echo "note: SDL2 dev files not found; building headless (no --display window). Install libsdl2-dev to enable it." >&2
fi

# Always reconfigure (idempotent) so the display cache var is applied even to a build
# directory first configured without it.
echo "Configuring preset '$PRESET'..."
cmake --preset "$PRESET" "${display_args[@]}"

# maize-418: mzvm is the console-subsystem Maize v2 machine (terminal I/O); mzvmg is the
# graphical one (SDL window). maize-422 (D-1): mzasm is the v2 assembler.
# --with-c-toolchain adds the v1 C pipeline; mzcc spawns mazm and mzld out of the BUILD
# directory (mzcc.c resolves them under MAIZE_ROOT/build/<preset>), so those two are
# built for it without being installed onto PATH.
install_tools=(mzvm mzvmg mzasm)
build_targets=("${install_tools[@]}")
copy_tools=("${install_tools[@]}")
if [ "$WITH_C_TOOLCHAIN" -eq 1 ]; then
    build_targets+=(mzcc mazm mzld)
    # mzcc is the C pipeline's entry point, so it travels with the toolchain rather than
    # with the v2 machine. mazm and mzld stay in the build directory, unexported.
    copy_tools+=(mzcc)
fi

echo "Building ${build_targets[*]} ($PRESET)..."
cmake --build "$BUILD_DIR" --target "${build_targets[@]}"

mkdir -p "$INSTALL_DIR"
for tool in "${copy_tools[@]}"; do
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

# Deliberately-broken stdin probe: proves the installed mzasm supports the
# editor's --stdin diagnostics path, independent of repo .mzasm state.
set +e
probe_out=$(printf 'no_such_instruction\n' | "$INSTALL_DIR/mzasm" --check --stdin --base-path /tmp --source-name mzasm-install-probe 2>&1)
probe_rc=$?
set -e

if [ "$probe_rc" -ne 1 ] || ! printf '%s' "$probe_out" | grep -q 'mzasm-install-probe:1: error:'; then
    echo "error: installed mzasm failed the --stdin probe smoke test (exit $probe_rc)" >&2
    exit 1
fi

# mzvm smoke: no image argument prints the usage line to stderr and exits 2.
set +e
vm_out=$("$INSTALL_DIR/mzvm" 2>&1)
vm_rc=$?
set -e

if [ "$vm_rc" -ne 2 ] || ! printf '%s' "$vm_out" | grep -q 'usage: mzvm'; then
    echo "error: installed mzvm failed the usage smoke test (exit $vm_rc)" >&2
    exit 1
fi

# --- C cross-toolchain refresh (cproc/qbe + Maize target) -------------------------
# maize-454: opt-in via --with-c-toolchain, since Ctrl+Shift+B now runs this script on
# every press and the refresh is a real wait whenever it is not a cache hit. Non-fatal
# when it does run: the v2 tools above are installed and smoke-checked, so a toolchain
# hiccup (e.g. no network for the submodule fetch) only warns.
if [ "$WITH_C_TOOLCHAIN" -eq 1 ]; then
    set +e
    "$SCRIPT_DIR/refresh-c-toolchain.sh"
    tc_rc=$?
    set -e
    if [ "$tc_rc" -ne 0 ]; then
        echo "warning: C cross-toolchain refresh failed (exit $tc_rc); native tools are installed. Retry with scripts/refresh-c-toolchain.sh." >&2
    fi
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

echo "Installed ${copy_tools[*]} to $INSTALL_DIR (built from $revision)."
