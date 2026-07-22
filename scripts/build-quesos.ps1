#Requires -Version 5.1

<#
.SYNOPSIS
    Build quesOS into a loadable Maize image (quesos.mzx) from a Windows terminal.

.DESCRIPTION
    Links quesOS (the guest operating system) into a single quesos.mzx image using
    the same C toolchain the rest of the guest world is built with. The compile runs
    natively via Git Bash (maize-258; the cproc/QBE C toolchain is POSIX-only but
    runs under the vendored llvm-mingw toolchain through Git Bash, no WSL involved),
    but you drive it entirely from PowerShell.

    With no arguments the image is written beside its source at os\quesos\quesos.mzx.
    Once built, load it directly with `maize <path>`; a quesOS image is portable Maize
    bytecode and runs on the native Windows maize.exe no matter which host built it.

    Requires Git for Windows (ships Git Bash) with the Maize C cross-toolchain
    provisioned (run scripts\install-mazm.ps1 once to set that up). If Git Bash is
    not found the script stops with a clear message.

.PARAMETER Preset
    Build preset to compile against. When omitted, the underlying build picks the
    default preset for the platform.

.PARAMETER Out
    Where to write the linked image. Defaults to os\quesos\quesos.mzx beside the source.

.EXAMPLE
    .\scripts\build-quesos.ps1
    Builds os\quesos\quesos.mzx, then run it with: maize os\quesos\quesos.mzx

.EXAMPLE
    .\scripts\build-quesos.ps1 -Out C:\tmp\quesos.mzx
    Writes the image to the given path instead of the default location.
#>
[CmdletBinding()]
param(
    [string]$Preset = '',
    [string]$Out = '',
    [string]$Map = ''          # maize-261: forward to mzld's symbol-map sidecar (--map)
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$RepoRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'lib\gitbash.ps1')

# Resolve the output image path (default: beside the source), always to an absolute
# Windows path so the Git Bash translation and the run-it hint below are unambiguous.
if ($Out -ne '') {
    $OutPath = $Out
}
else {
    $OutPath = Join-Path $RepoRoot 'os\quesos\quesos.mzx'
}
if (-not [System.IO.Path]::IsPathRooted($OutPath)) {
    $OutPath = Join-Path (Get-Location).Path $OutPath
}
$OutPath = [System.IO.Path]::GetFullPath($OutPath)

# Git Bash is required: the cproc/QBE C toolchain is POSIX-only. maize-258 repoints
# this forwarder from WSL to Git Bash (mzcc.cmd's already-shipped pattern,
# install-mazm.ps1:250-292): the WSL-native-mirror the underlying .sh script applies
# on a /mnt/* repo excludes /build, so cc-maize.sh could not find mazm inside the
# mirror (maize-265's blast radius). A Git-Bash-resolved repo root is never /mnt/*,
# so the mirror guard never engages. Fail fast with a clear message rather than a
# raw .NET exception when bash.exe cannot be found.
$BashExe = Resolve-GitBash
if (-not $BashExe) {
    # -ErrorAction Continue: Set-StrictMode/EAP='Stop' would otherwise make Write-Error
    # terminating, so the process would die on the throw (exit 1) before reaching exit 2.
    Write-Error 'Git Bash (bash.exe) not found; install Git for Windows (ships Git Bash) to build quesOS (the C toolchain is POSIX-only).' -ErrorAction Continue
    exit 2
}

# No path translation is needed: mzcc.cmd already proves a forward-slashed absolute
# Windows path (C:/Users/.../repo) resolves natively under Git Bash / MSYS without
# cygpath, because MSYS bash understands drive-letter paths directly.
$repoPosix = ($RepoRoot -replace '\\', '/')
$outPosix = ($OutPath -replace '\\', '/')

# One bash statement, nothing chained after it, so $LASTEXITCODE is exactly the build
# script's own exit code (a trailing statement would mask it).
$cmd = "'$repoPosix/os/quesos/build-quesos.sh'"
if ($Preset -ne '') {
    $cmd += " --preset '$Preset'"
}
$cmd += " -o '$outPosix'"
if ($Map -ne '') {
    $mapPosix = ($Map -replace '\\', '/')
    $cmd += " --map '$mapPosix'"
}

# Windows PowerShell 5.1 turns any native-command stderr into a terminating
# NativeCommandError while ErrorActionPreference is 'Stop'; the build writes progress to
# stderr, so relax it for exactly the Git Bash call and restore it after (pwsh 7 is
# unaffected either way).
#
# INTERIM DEPENDENCY NOTE (maize-266 tracks the end state): Git Bash ships with Git
# for Windows itself, so this call adds no dependency beyond Git plus what the repo
# vendors, but calling out to bash at all is a tolerated interim shape, not the
# target; maize-266 designs that dependency out.
$prevEap = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
& $BashExe -lc $cmd
$code = $LASTEXITCODE
$ErrorActionPreference = $prevEap

if ($code -eq 0) {
    Write-Host "Built $OutPath"
    Write-Host "Run it with: maize `"$OutPath`""
}
exit $code
