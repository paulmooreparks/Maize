#Requires -Version 5.1
# scripts/lib/ToolchainRoot.ps1 (maize-439): shared PowerShell helper, dot-sourced by
# scripts/bootstrap-toolchain.ps1, scripts/bootstrap-sdl2.ps1, scripts/install-mzasm.ps1
# and scripts/build-pgo.ps1. This is the SOLE PowerShell definition site for the
# toolchain resolution order; do not copy a function body into another script,
# dot-source this file instead (scripts/lib/gitbash.ps1 is the precedent for exactly
# this shape, and scripts/lib/harness-env.sh is the sh-side one).
#
# This file defines functions and sets no top-level state beyond the house
# EAP/StrictMode hardening below; it must be dot-sourced
# (". <path>\lib\ToolchainRoot.ps1"), never executed directly.
#
# THE RESOLUTION ORDER, which three files implement because three runtimes cannot
# share code (this one, scripts/lib/toolchain-root.sh, cmake/ToolchainRoot.cmake):
#
#   1. $env:MAIZE_TOOLCHAIN_ROOT, when set and non-empty, names the toolchains root.
#   2. Otherwise the per-user default root: %LOCALAPPDATA%\Maize\toolchains on
#      Windows, ${XDG_CACHE_HOME:-~/.cache}/maize/toolchains elsewhere.
#   3. Under that root the versioned directory is <root>/<tool>/<pinned-version>/
#      plus the tool's arch leaf, and it answers when it holds the probe file.
#   4. Otherwise the in-repo fallback <repo-root>/.toolchains/<tool>/ plus the same
#      arch leaf, when IT holds the probe file. This keeps a checkout that predates
#      maize-439 building with no migration step.
#   5. Otherwise nothing resolves. Callers that can bootstrap do so, installing to
#      step 2 and never to the repository.
#
# The three implementations are held to the same answer by
# scripts/test-toolchain-resolution.sh, which populates several candidates at once
# with distinguishable probe bytes and asserts all three pick the same winner. It runs
# on the Windows CI job (.github/workflows/ci.yml, "Toolchain resolver agreement"),
# which is where both of its prerequisites, PowerShell and cmake, are present. It is
# NOT registered with ctest. Change the order here and that job fails.

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# Per-tool layout. Three facts differ per tool and nothing else does:
#
#   RepoDir  the directory name under .toolchains/ in the pre-maize-439 in-repo
#            layout. It is 'SDL2' rather than 'sdl2' because that is what
#            bootstrap-sdl2.ps1 has always written and an existing checkout is not
#            being renamed. The per-user layout uses the lowercase tool name
#            throughout, for filesystem-name consistency with 'llvm-mingw'.
#   ArchLeaf a subdirectory below the tool directory that both layouts share. SDL2's
#            upstream archive is organized by target triple and the vendoring keeps
#            that shape; llvm-mingw has no such level.
#   Probe    the file whose presence means "this directory really holds the tool",
#            relative to the arch leaf. Callers pass their own probe, so this is the
#            documented default rather than a hard rule.
$script:MaizeToolLayout = @{
    'llvm-mingw' = @{ RepoDir = 'llvm-mingw'; ArchLeaf = '';                    Probe = 'bin/x86_64-w64-mingw32-clang++.exe' }
    'sdl2'       = @{ RepoDir = 'SDL2';       ArchLeaf = 'x86_64-w64-mingw32';  Probe = 'lib/cmake/SDL2/sdl2-config.cmake' }
}

function Get-MaizeToolchainRoot {
    <#
    .SYNOPSIS
        The toolchains root: the MAIZE_TOOLCHAIN_ROOT override, else the per-user
        default. Steps 1 and 2 of the order above.
    #>
    if ($env:MAIZE_TOOLCHAIN_ROOT) { return $env:MAIZE_TOOLCHAIN_ROOT }
    if ($env:LOCALAPPDATA) { return (Join-Path $env:LOCALAPPDATA 'Maize\toolchains') }
    # No LOCALAPPDATA means this is not a Windows session, which no caller of this
    # file currently is; answer the POSIX default rather than an empty path so a
    # misplaced call fails on a missing directory instead of on a malformed one.
    if ($env:XDG_CACHE_HOME) { return (Join-Path $env:XDG_CACHE_HOME 'maize/toolchains') }
    return (Join-Path $HOME '.cache/maize/toolchains')
}

function Get-MaizePinnedVersion {
    <#
    .SYNOPSIS
        Line 1 of scripts/toolchain-pins/<Tool>.pin, the pinned version.
    #>
    param([Parameter(Mandatory)][ValidateSet('llvm-mingw','sdl2')][string]$Tool)
    return (Get-MaizePinField -Tool $Tool -Index 0)
}

function Get-MaizePinnedSha256 {
    <#
    .SYNOPSIS
        Line 2 of scripts/toolchain-pins/<Tool>.pin, the pinned asset checksum.
    #>
    param([Parameter(Mandatory)][ValidateSet('llvm-mingw','sdl2')][string]$Tool)
    return (Get-MaizePinField -Tool $Tool -Index 1)
}

function Get-MaizePinField {
    <#
    .SYNOPSIS
        Field <Index> of a pin file, comments and blank lines removed.
    .DESCRIPTION
        The pin path is resolved against THIS file's own location, never the
        caller's working directory (the $PSScriptRoot pattern gitbash.ps1 and every
        scripts/*.ps1 already use), so a dot-sourcing script can be invoked from
        anywhere. Throws rather than returning $null on a malformed pin: a pin file
        that cannot be read is a broken checkout, and every caller's next act would
        be to build a path around the missing value.
    #>
    param(
        [Parameter(Mandatory)][ValidateSet('llvm-mingw','sdl2')][string]$Tool,
        [Parameter(Mandatory)][int]$Index
    )
    $pinPath = Join-Path (Split-Path -Parent $PSScriptRoot) "toolchain-pins/$Tool.pin"
    if (-not (Test-Path $pinPath)) {
        throw "Toolchain pin file not found: $pinPath"
    }
    # Comment and blank-line filtering is the pin format's one rule; keep it
    # identical in toolchain-root.sh and ToolchainRoot.cmake.
    $lines = @(Get-Content -Path $pinPath | ForEach-Object { $_.Trim() } |
               Where-Object { $_ -ne '' -and -not $_.StartsWith('#') })
    if ($lines.Count -le $Index) {
        throw "Toolchain pin file $pinPath has no value at index $Index (found $($lines.Count) value lines; expected version then sha256)."
    }
    return $lines[$Index]
}

function Get-MaizeToolLayout {
    <#
    .SYNOPSIS
        The per-tool RepoDir / ArchLeaf / Probe record documented above.
    #>
    param([Parameter(Mandatory)][ValidateSet('llvm-mingw','sdl2')][string]$Tool)
    return $script:MaizeToolLayout[$Tool]
}

function Get-MaizeToolchainInstallDir {
    <#
    .SYNOPSIS
        Where a bootstrap script INSTALLS this tool: the versioned per-user
        directory, step 3 of the order, whether or not anything is there yet.
    .DESCRIPTION
        Separate from Resolve-MaizeToolchainDir on purpose. Resolution answers
        "where is the tool", which can legitimately be the in-repo fallback;
        installation always targets the per-user location and never the repository,
        which is the whole point of maize-439.
    #>
    param([Parameter(Mandatory)][ValidateSet('llvm-mingw','sdl2')][string]$Tool)
    $layout    = Get-MaizeToolLayout -Tool $Tool
    $versioned = Join-Path (Join-Path (Get-MaizeToolchainRoot) $Tool) (Get-MaizePinnedVersion -Tool $Tool)
    if ($layout.ArchLeaf) { return (Join-Path $versioned $layout.ArchLeaf) }
    return $versioned
}

function Resolve-MaizeToolchainDir {
    <#
    .SYNOPSIS
        Steps 3 and 4 of the resolution order: the directory that actually holds the
        tool, or $null.
    .DESCRIPTION
        Returns the resolved absolute directory, or $null when neither candidate
        carries $ProbeRelativePath. Never bootstraps and never writes; a caller that
        wants a missing tool fetched checks for $null and invokes the bootstrap
        script itself, which is the shape install-mzasm.ps1 already had.
    #>
    param(
        [Parameter(Mandatory)][ValidateSet('llvm-mingw','sdl2')][string]$Tool,
        [Parameter(Mandatory)][string]$ProbeRelativePath
    )
    foreach ($candidate in (Get-MaizeToolchainCandidateDirs -Tool $Tool)) {
        if (Test-Path (Join-Path $candidate $ProbeRelativePath)) { return $candidate }
    }
    return $null
}

function Get-MaizeToolchainCandidateDirs {
    <#
    .SYNOPSIS
        The candidate directories, highest precedence first: the versioned per-user
        (or overridden) directory, then the in-repo fallback.
    .DESCRIPTION
        Exposed so a diagnostic ("checked X, then Y") can name the same paths the
        resolution actually walked rather than recomposing them, which is how the
        two drift apart. cmake/ToolchainRoot.cmake's FATAL_ERROR prints its own two
        candidates for the same reason.
    #>
    param([Parameter(Mandatory)][ValidateSet('llvm-mingw','sdl2')][string]$Tool)
    $layout   = Get-MaizeToolLayout -Tool $Tool
    $repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
    $repoDir  = Join-Path (Join-Path $repoRoot '.toolchains') $layout.RepoDir
    if ($layout.ArchLeaf) { $repoDir = Join-Path $repoDir $layout.ArchLeaf }
    return @((Get-MaizeToolchainInstallDir -Tool $Tool), $repoDir)
}
