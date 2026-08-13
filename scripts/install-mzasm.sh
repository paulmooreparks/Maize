#!/usr/bin/env bash
# Build the Maize v2 binaries (mzvm, mzvmg, mzasm) and install stable copies into
# ~/bin (Linux/WSL/macOS).
# Counterpart of install-mzasm.ps1; wired to the default build task via
# .vscode/tasks.json, which runs this script on every press so the binaries it just
# built are the ones on PATH (maize-454). Never prompts.
#
# maize-454: the installed set is the v2 machine and the v2 assembler only. mzld, mzdis
# and mzcc keep their names under maize-422 D-1 but have not been ported yet
# (maize-423/424/425/426), so installing today's v1 builds of them would put tools on
# PATH that cannot read a v2 object; each comes back here as its parity card lands.
#
# maize-450: the --with-c-toolchain option is gone with the v1 build. It named mzcc plus
# the mazm, maize and mzld its resolver requires, and none of those targets exists in this
# tree's CMakeLists any more. v1 is archived: its sources are still here to port from, and
# it still builds, installs and tests on the `v1` branch.
#
# usage: install-mzasm.sh [preset] [install-dir]

set -euo pipefail

PRESET=""
INSTALL_DIR=""

for arg in "$@"; do
    case "$arg" in
        -*)
            echo "error: unknown option '$arg'" >&2
            echo "usage: install-mzasm.sh [preset] [install-dir]" >&2
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

# Set MAIZE_DISPLAY from whether system SDL2 dev files are present, rather than failing
# the task on a server host that has none. No binary links SDL2 today (maize-450 archived
# v1's maizeg; mzvmg's display device is maize-456), so neither branch changes what gets
# built; the option is kept wired so the machine is ready when the port lands.
# MAIZE_DISPLAY is passed EXPLICITLY either way: a bare configure would inherit a
# stale MAIZE_DISPLAY=ON from a prior CMakeCache and then hard-fail find_package(SDL2)
# once system SDL2 went missing. (On Windows install-mzasm.ps1 instead auto-fetches a
# vendored SDL2; on Linux/WSL, SDL2 comes from the system package manager.)
if command -v sdl2-config >/dev/null 2>&1 || pkg-config --exists sdl2 2>/dev/null; then
    display_args=(-DMAIZE_DISPLAY=ON)
    echo "SDL2 found; configuring with MAIZE_DISPLAY=ON. Nothing links it yet (maize-456)."
else
    display_args=(-DMAIZE_DISPLAY=OFF)
    echo "note: SDL2 dev files not found; configuring with MAIZE_DISPLAY=OFF. Nothing links SDL2 yet, so this build is unaffected; install libsdl2-dev before the display device lands (maize-456)." >&2
fi

# Always reconfigure (idempotent) so the display cache var is applied even to a build
# directory first configured without it.
echo "Configuring preset '$PRESET'..."
cmake --preset "$PRESET" "${display_args[@]}"

# maize-418: mzvm is the console-subsystem Maize v2 machine (terminal I/O); mzvmg is the
# graphical one, whose display device has not landed yet (maize-456), so today it is a
# name-reserving twin of mzvm. maize-422 (D-1): mzasm is the v2 assembler.
install_tools=(mzvm mzvmg mzasm)
build_targets=("${install_tools[@]}")
copy_tools=("${install_tools[@]}")

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

# maize-450: the C cross-toolchain refresh (cproc/qbe plus mzcc) used to run here behind
# --with-c-toolchain. It built v1 guest code and is archived with the rest of v1.

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
