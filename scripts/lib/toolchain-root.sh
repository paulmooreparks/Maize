# shellcheck shell=sh
# toolchain-root.sh (maize-439): the POSIX-sh half of the toolchain resolution order.
#
# This file is SOURCED, never executed directly (it defines functions and sets no
# top-level state), the same shape scripts/lib/harness-env.sh established. Sourced by
# scripts/bootstrap-toolchain.sh, scripts/cc-maize.sh, scripts/build-toolchain.sh and
# os/quesos/build-quesos.sh, each of which had its own inline
# ${REPO_ROOT}/.toolchains/llvm-mingw path before this file existed.
#
# THE RESOLUTION ORDER, which three files implement because three runtimes cannot
# share code (this one, scripts/lib/ToolchainRoot.ps1, cmake/ToolchainRoot.cmake):
#
#   1. $MAIZE_TOOLCHAIN_ROOT, when set and non-empty, names the toolchains root.
#   2. Otherwise the per-user default root: $LOCALAPPDATA/Maize/toolchains on a
#      Windows-shaped host, ${XDG_CACHE_HOME:-$HOME/.cache}/maize/toolchains
#      elsewhere.
#   3. Under that root the versioned directory is <root>/<tool>/<pinned-version>/
#      plus the tool's arch leaf, and it answers when it holds the probe file.
#   4. Otherwise the in-repo fallback <repo-root>/.toolchains/<tool>/ plus the same
#      arch leaf, when IT holds the probe file. This keeps a checkout that predates
#      maize-439 building with no migration step.
#   5. Otherwise nothing resolves and maize_resolve_toolchain_dir returns 1.
#
# scripts/test-toolchain-resolution.sh holds all three implementations to the same
# answer by populating several candidates at once with distinguishable probe bytes. It
# runs on the Windows CI job (.github/workflows/ci.yml, "Toolchain resolver agreement"),
# which is where both of its prerequisites, PowerShell and cmake, are present. It is
# NOT registered with ctest. Change the order here and that job fails.
#
# Every function here is pure: it reads the environment and the filesystem, echoes a
# path, and writes nothing. Bootstrapping is the caller's business.

# The directory holding THIS file, resolved against the file rather than the caller's
# CWD. Every sourcing script already computes its own SCRIPT_DIR the same way; this
# one has to compute its own because the sourcing script may live in os/quesos rather
# than scripts/, so its SCRIPT_DIR is not this one's.
maize__lib_dir() {
    # $0 is the SOURCING script under POSIX sh, which has no equivalent of
    # BASH_SOURCE, so the lib directory cannot be derived from $0 here. Callers pass
    # it in via MAIZE_TOOLCHAIN_LIB_DIR, which every sourcing site sets to the
    # directory it just sourced this file from.
    if [ -n "${MAIZE_TOOLCHAIN_LIB_DIR:-}" ]; then
        printf '%s' "${MAIZE_TOOLCHAIN_LIB_DIR}"
        return 0
    fi
    echo "toolchain-root.sh: MAIZE_TOOLCHAIN_LIB_DIR is unset; the sourcing script must set it to the directory it sourced this file from." >&2
    return 1
}

# Echo field N (0-based) of a pin file, comments and blank lines removed. The pin
# format's one rule, kept identical in ToolchainRoot.ps1 and ToolchainRoot.cmake:
# a line whose first character is '#' is a comment, blank lines are ignored, and of
# what remains line 1 is the version and line 2 is the sha256.
maize_pin_field() {
    _mpf_tool="$1"
    _mpf_index="$2"
    _mpf_lib=$(maize__lib_dir) || return 1
    _mpf_file="${_mpf_lib}/../toolchain-pins/${_mpf_tool}.pin"
    if [ ! -f "$_mpf_file" ]; then
        echo "toolchain-root.sh: pin file not found: ${_mpf_file}" >&2
        return 1
    fi
    _mpf_value=$(sed -e 's/[[:space:]]*$//' "$_mpf_file" \
                 | grep -v '^[[:space:]]*#' \
                 | grep -v '^[[:space:]]*$' \
                 | sed -n "$((_mpf_index + 1))p")
    if [ -z "$_mpf_value" ]; then
        echo "toolchain-root.sh: ${_mpf_file} has no value at index ${_mpf_index} (expected version then sha256)." >&2
        return 1
    fi
    printf '%s' "$_mpf_value"
}

maize_pinned_version() { maize_pin_field "$1" 0; }
maize_pinned_sha256()  { maize_pin_field "$1" 1; }

# Echo the toolchains root: the override, else the per-user default. Steps 1 and 2.
maize_toolchain_root() {
    if [ -n "${MAIZE_TOOLCHAIN_ROOT:-}" ]; then
        printf '%s' "${MAIZE_TOOLCHAIN_ROOT}"
        return 0
    fi
    case "$(uname -s)" in
        MINGW*|MSYS*|CYGWIN*)
            # A Windows-shaped host answers %LOCALAPPDATA%, so a Git Bash session and
            # a PowerShell session resolve the same directory rather than two.
            if [ -n "${LOCALAPPDATA:-}" ]; then
                printf '%s' "$(maize__to_slashes "${LOCALAPPDATA}")/Maize/toolchains"
                return 0
            fi
            ;;
    esac
    if [ -n "${XDG_CACHE_HOME:-}" ]; then
        printf '%s' "${XDG_CACHE_HOME}/maize/toolchains"
        return 0
    fi
    printf '%s' "${HOME}/.cache/maize/toolchains"
}

# Backslashes to forward slashes. LOCALAPPDATA arrives from Windows in native form
# (C:\Users\x\AppData\Local) and every consumer here concatenates it into a path a
# POSIX-sh test and a Windows program both have to accept; the mixed form
# C:/Users/x/AppData/Local is the one both do.
maize__to_slashes() {
    printf '%s' "$1" | tr '\\' '/'
}

# Echo the tool's arch leaf, the subdirectory both layouts share below the tool
# directory. SDL2's upstream archive is organized by target triple and the vendoring
# keeps that shape; llvm-mingw has no such level.
maize__tool_arch_leaf() {
    case "$1" in
        sdl2) printf '%s' 'x86_64-w64-mingw32' ;;
        *)    printf '%s' '' ;;
    esac
}

# Echo the tool's directory name under the in-repo .toolchains/ fallback. It is
# 'SDL2' rather than 'sdl2' because that is what bootstrap-sdl2.ps1 has always
# written and an existing checkout is not being renamed; the per-user layout uses the
# lowercase tool name throughout, for filesystem-name consistency with 'llvm-mingw'.
maize__tool_repo_dir() {
    case "$1" in
        sdl2) printf '%s' 'SDL2' ;;
        *)    printf '%s' "$1" ;;
    esac
}

# Echo where a bootstrap script INSTALLS this tool: the versioned per-user directory,
# step 3, whether or not anything is there yet. Separate from
# maize_resolve_toolchain_dir on purpose: resolution can legitimately answer the
# in-repo fallback, installation always targets the per-user location and never the
# repository, which is the whole point of maize-439.
maize_toolchain_install_dir() {
    _mtid_tool="$1"
    _mtid_version=$(maize_pinned_version "$_mtid_tool") || return 1
    _mtid_leaf=$(maize__tool_arch_leaf "$_mtid_tool")
    _mtid_dir="$(maize_toolchain_root)/${_mtid_tool}/${_mtid_version}"
    if [ -n "$_mtid_leaf" ]; then
        _mtid_dir="${_mtid_dir}/${_mtid_leaf}"
    fi
    printf '%s' "$_mtid_dir"
}

# Echo the in-repo fallback directory for this tool, step 4, whether or not anything
# is there.
#
# The fallback is derived from THIS file's location rather than from a caller's
# REPO_ROOT, so scripts/cc-maize.sh and os/quesos/build-quesos.sh reach the same
# directory without either of them having to agree on how deep it sits.
maize_toolchain_repo_dir() {
    _mtrd_tool="$1"
    _mtrd_lib=$(maize__lib_dir) || return 1
    _mtrd_leaf=$(maize__tool_arch_leaf "$_mtrd_tool")
    _mtrd_dir="${_mtrd_lib}/../../.toolchains/$(maize__tool_repo_dir "$_mtrd_tool")"
    if [ -n "$_mtrd_leaf" ]; then
        _mtrd_dir="${_mtrd_dir}/${_mtrd_leaf}"
    fi
    printf '%s' "$_mtrd_dir"
}

# Echo the candidate directories, highest precedence first, one per line. Exposed so
# a diagnostic ("checked X, then Y") can name the same paths the resolution actually
# walked rather than recomposing them, which is how the two drift apart.
maize_toolchain_candidate_dirs() {
    _mtcd_tool="$1"
    _mtcd_inst=$(maize_toolchain_install_dir "$_mtcd_tool") || return 1
    _mtcd_repo=$(maize_toolchain_repo_dir "$_mtcd_tool")    || return 1
    printf '%s\n%s\n' "$_mtcd_inst" "$_mtcd_repo"
}

# Steps 3 and 4. Echo the resolved directory on stdout and return 0, or return 1 with
# nothing on stdout when neither candidate carries <probe-relative-path>. Never
# bootstraps, never writes.
#
# No pipeline and no subshell loop here on purpose: the found/not-found answer is
# this function's exit status, and a `while read` on the right of a pipe cannot
# return one to its caller.
#
# Usage: maize_resolve_toolchain_dir <tool> <probe-relative-path>
maize_resolve_toolchain_dir() {
    _mrtd_tool="$1"
    _mrtd_probe="$2"
    _mrtd_inst=$(maize_toolchain_install_dir "$_mrtd_tool") || return 1
    if [ -f "${_mrtd_inst}/${_mrtd_probe}" ]; then
        printf '%s' "$_mrtd_inst"
        return 0
    fi
    _mrtd_repo=$(maize_toolchain_repo_dir "$_mrtd_tool") || return 1
    if [ -f "${_mrtd_repo}/${_mrtd_probe}" ]; then
        printf '%s' "$_mrtd_repo"
        return 0
    fi
    return 1
}
