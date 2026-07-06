#!/bin/sh
# Fetch and verify the pinned llvm-mingw toolchain for building Maize on Windows.
#
# On genuine Linux/macOS this is a harmless no-op: only the Windows llvm-mingw path
# needs this archive (Linux/macOS build with the system compiler). On a Windows
# Git-Bash / MSYS shell it performs the same fetch-verify-extract as the PowerShell
# script.
#
# No admin rights, no PATH mutation, no installer: the script only writes inside the
# repo's own .toolchains/ directory. Idempotent; pass --force to re-fetch.
#
# This toolchain has no Microsoft Visual C++ Redistributable runtime dependency.

set -eu

# --- Pinned constants (see resolved open question 5943 on card maize-32) ---------
VERSION='20260616'
ASSET='llvm-mingw-20260616-ucrt-x86_64.zip'
URL="https://github.com/mstorsjo/llvm-mingw/releases/download/${VERSION}/${ASSET}"
SHA256='b9b68a4d276e16fa25802aaba458e4638f64b3884c290aaccdc2d87083b6ca35'

FORCE=0
for arg in "$@"; do
    case "$arg" in
        --force) FORCE=1 ;;
        *) echo "Unknown argument: $arg" >&2; exit 2 ;;
    esac
done

# --- Genuine Linux/macOS: no fetch needed ---------------------------------------
case "$(uname -s)" in
    Linux|Darwin)
        echo "bootstrap-toolchain.sh: no toolchain fetch needed on $(uname -s)."
        echo "Build with the system compiler:"
        echo "  cmake --preset linux-debug   # or macos-debug"
        exit 0
        ;;
esac

# --- Windows (MINGW*/MSYS*/CYGWIN*): perform the fetch ---------------------------

# Paths resolved relative to THIS script, not the caller's CWD.
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "${SCRIPT_DIR}/.." && pwd)
DEST="${REPO_ROOT}/.toolchains/llvm-mingw"
STAMP="${DEST}/.bootstrap-version"
CLANGXX="${DEST}/bin/x86_64-w64-mingw32-clang++.exe"
CLANGC="${DEST}/bin/x86_64-w64-mingw32-clang.exe"

# --- Idempotency check -----------------------------------------------------------
if [ "$FORCE" -eq 0 ] && [ -f "$STAMP" ] && [ -f "$CLANGXX" ]; then
    existing=$(tr -d ' \t\r\n' < "$STAMP")
    if [ "$existing" = "$VERSION" ]; then
        echo "llvm-mingw ${VERSION} already up to date at ${DEST}"
        echo "  C compiler:   ${CLANGC}"
        echo "  C++ compiler: ${CLANGXX}"
        exit 0
    fi
fi

# --- Remove any stale/partial destination ----------------------------------------
if [ -e "$DEST" ]; then
    echo "Removing existing ${DEST} ..."
    rm -rf "$DEST"
fi

# --- Download to a temp file -----------------------------------------------------
TMPDIR_BASE="${TMPDIR:-/tmp}"
TMPZIP="${TMPDIR_BASE}/maize-${ASSET}"
rm -f "$TMPZIP"

echo "Downloading ${URL} ..."
if command -v curl >/dev/null 2>&1; then
    curl -fSL --retry 3 -o "$TMPZIP" "$URL"
elif command -v wget >/dev/null 2>&1; then
    wget -O "$TMPZIP" "$URL"
else
    echo "Neither curl nor wget is available; cannot download the toolchain." >&2
    exit 1
fi

# --- Verify SHA256 before extracting ---------------------------------------------
echo "Verifying SHA256 ..."
if command -v sha256sum >/dev/null 2>&1; then
    actual=$(sha256sum "$TMPZIP" | cut -d' ' -f1)
elif command -v shasum >/dev/null 2>&1; then
    actual=$(shasum -a 256 "$TMPZIP" | cut -d' ' -f1)
else
    rm -f "$TMPZIP"
    echo "Neither sha256sum nor shasum is available; cannot verify the download." >&2
    exit 1
fi

# Case-insensitive hex comparison.
expected_lc=$(printf '%s' "$SHA256" | tr 'A-Z' 'a-z')
actual_lc=$(printf '%s' "$actual" | tr 'A-Z' 'a-z')
if [ "$actual_lc" != "$expected_lc" ]; then
    rm -f "$TMPZIP"
    echo "Checksum mismatch for ${ASSET}" >&2
    echo "  expected: ${SHA256}" >&2
    echo "  actual:   ${actual}" >&2
    echo "Refusing to extract unverified content." >&2
    exit 1
fi
echo "  OK: ${actual}"

# --- Extract, stripping the archive's top-level directory ------------------------
if ! command -v unzip >/dev/null 2>&1; then
    rm -f "$TMPZIP"
    echo "unzip is not available; cannot extract the toolchain." >&2
    exit 1
fi

TMPEXTRACT="${TMPDIR_BASE}/maize-extract-$$"
rm -rf "$TMPEXTRACT"
mkdir -p "$TMPEXTRACT"
echo "Extracting ..."
unzip -q "$TMPZIP" -d "$TMPEXTRACT"

# The archive contains a single top-level directory (llvm-mingw-<tag>-ucrt-x86_64/).
# Strip it so bin/, lib/, etc. land directly under $DEST.
inner="$TMPEXTRACT"
count=$(find "$TMPEXTRACT" -mindepth 1 -maxdepth 1 | wc -l)
if [ "$count" -eq 1 ]; then
    only=$(find "$TMPEXTRACT" -mindepth 1 -maxdepth 1)
    if [ -d "$only" ]; then
        inner="$only"
    fi
fi

mkdir -p "$(dirname -- "$DEST")"
mv "$inner" "$DEST"

rm -rf "$TMPEXTRACT"
rm -f "$TMPZIP"

# --- Stamp the version -----------------------------------------------------------
printf '%s' "$VERSION" > "$STAMP"

if [ ! -f "$CLANGXX" ]; then
    echo "Extraction completed but ${CLANGXX} is missing; the archive layout may have changed." >&2
    exit 1
fi

echo ""
echo "llvm-mingw ${VERSION} installed at ${DEST}"
echo "  C compiler:   ${CLANGC}"
echo "  C++ compiler: ${CLANGXX}"
echo ""
echo "Next: cmake --preset windows-llvm-mingw-debug"
exit 0
