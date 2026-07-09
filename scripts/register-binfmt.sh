#!/usr/bin/env bash
# Register (or unregister) the Maize runner as Linux binfmt_misc interpreters so
# Maize images run directly from the shell or file manager:
#
#     ./hello.mzb        instead of   maize hello.mzb
#     ./hello.mzx        instead of   maize hello.mzx
#
# maize-67. This is DOCUMENTED, user-run glue. It is deliberately NOT wired into
# the build or the install task; it mutates per-kernel OS state and must be run
# (and reversed) by a human who understands what it changes.
#
# Two binfmt_misc entries are created:
#
#   maize-mzx : matched by header MAGIC ("MZX" 0x01 = bytes 4D 5A 58 01) at
#               offset 0. Magic is authoritative, exactly as the maize loader
#               itself dispatches (src/maize.cpp load_mzx), so ANY .mzx image
#               runs directly even without a .mzx extension, and even with no
#               extension at all, as long as it carries the magic and +x.
#
#   maize-mzb : matched by the .mzb EXTENSION. Flat .mzb images have no header
#               magic (the file begins with the first instruction), so the
#               extension is the only stable key the kernel can match on.
#
# binfmt_misc invokes the interpreter as:  maize <full-path-to-image> [args...]
# so the image path lands in maize's argv[1], which is exactly what maize wants.
#
# Requires root (writes /proc/sys/fs/binfmt_misc/register). binfmt_misc is
# per-kernel and, under WSL, per-instance: these entries are isolated from
# Windows Explorer file associations. Use register-assoc.ps1 for Windows.
#
# Usage:
#   sudo ./scripts/register-binfmt.sh register [--interp /path/to/maize]
#   sudo ./scripts/register-binfmt.sh unregister
#        ./scripts/register-binfmt.sh status
#
# The interpreter path is resolved from --interp, then $MAIZE_BIN, then `maize`
# on PATH. It must be an absolute path to an executable file.

set -euo pipefail

BINFMT_DIR="/proc/sys/fs/binfmt_misc"
MZX_NAME="maize-mzx"
MZB_NAME="maize-mzb"
# "MZX" + version 0x01 (see MZX_MAGIC0..2 / MZX_VERSION in src/maize_obj.h).
MZX_MAGIC='\x4d\x5a\x58\x01'

INTERP=""

die() { echo "register-binfmt: $*" >&2; exit 1; }

require_root() {
    if [ "$(id -u)" -ne 0 ]; then
        die "must run as root (writes $BINFMT_DIR). Re-run with sudo."
    fi
}

ensure_mounted() {
    if [ ! -e "$BINFMT_DIR/register" ]; then
        # The binfmt_misc filesystem is not mounted yet; mount it.
        if ! mount -t binfmt_misc none "$BINFMT_DIR" 2>/dev/null; then
            die "binfmt_misc is not mounted and could not be mounted at $BINFMT_DIR. \
Enable CONFIG_BINFMT_MISC / load the binfmt_misc module and retry."
        fi
    fi
}

resolve_interp() {
    local cand="$INTERP"
    if [ -z "$cand" ]; then cand="${MAIZE_BIN:-}"; fi
    if [ -z "$cand" ]; then
        if command -v maize >/dev/null 2>&1; then
            cand="$(command -v maize)"
        fi
    fi
    [ -n "$cand" ] || die "could not find the 'maize' interpreter. Pass --interp /path/to/maize, set \$MAIZE_BIN, or put maize on PATH."
    # binfmt_misc requires an absolute interpreter path.
    cand="$(readlink -f "$cand" 2>/dev/null || true)"
    [ -n "$cand" ] || die "could not resolve an absolute path to the maize interpreter."
    [ -x "$cand" ] || die "interpreter '$cand' is not an executable file."
    printf '%s' "$cand"
}

unregister_one() {
    local name="$1"
    if [ -e "$BINFMT_DIR/$name" ]; then
        echo -1 > "$BINFMT_DIR/$name"
        echo "unregistered $name"
    else
        echo "$name not registered (nothing to do)"
    fi
}

register_one() {
    # register_one <name> <register-spec>
    local name="$1" spec="$2"
    # Idempotent: drop any stale entry first so re-running always lands the
    # current interpreter path.
    if [ -e "$BINFMT_DIR/$name" ]; then
        echo -1 > "$BINFMT_DIR/$name"
    fi
    printf '%s' "$spec" > "$BINFMT_DIR/register"
    echo "registered $name"
}

do_register() {
    require_root
    ensure_mounted
    local interp
    interp="$(resolve_interp)"
    echo "interpreter: $interp"
    # Field layout (delimiter ':'):  :name:type:offset:magic:mask:interpreter:flags
    # .mzx by magic at offset 0, exact match (empty mask).
    register_one "$MZX_NAME" ":$MZX_NAME:M:0:$MZX_MAGIC::$interp:"
    # .mzb by extension (flat images have no magic). Extension is given without
    # the leading dot.
    register_one "$MZB_NAME" ":$MZB_NAME:E::mzb::$interp:"
    echo "done. Directly-run images now execute via: $interp <image>"
    echo "note: make an image executable first, e.g. chmod +x hello.mzb"
}

do_unregister() {
    require_root
    if [ ! -e "$BINFMT_DIR/register" ]; then
        echo "binfmt_misc not mounted; nothing registered."
        return 0
    fi
    unregister_one "$MZX_NAME"
    unregister_one "$MZB_NAME"
    echo "done. Machine restored."
}

do_status() {
    if [ ! -e "$BINFMT_DIR/register" ]; then
        echo "binfmt_misc not mounted; no Maize handlers registered."
        return 0
    fi
    local any=0
    for name in "$MZX_NAME" "$MZB_NAME"; do
        if [ -e "$BINFMT_DIR/$name" ]; then
            any=1
            echo "=== $name ==="
            cat "$BINFMT_DIR/$name"
        fi
    done
    [ "$any" -eq 1 ] || echo "no Maize handlers registered."
}

ACTION="${1:-register}"
shift || true
while [ $# -gt 0 ]; do
    case "$1" in
        --interp) INTERP="${2:-}"; shift 2 ;;
        --interp=*) INTERP="${1#*=}"; shift ;;
        *) die "unknown argument: $1" ;;
    esac
done

case "$ACTION" in
    register)   do_register ;;
    unregister) do_unregister ;;
    status)     do_status ;;
    -h|--help|help)
        sed -n '2,45p' "$0" | sed 's/^# \{0,1\}//'
        ;;
    *) die "unknown action '$ACTION' (expected register | unregister | status)" ;;
esac
