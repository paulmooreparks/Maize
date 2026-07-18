#Requires -Version 5.1
<#
.SYNOPSIS
    Build quesOS into a loadable Maize image (quesos.mzx) from a Windows terminal.

.DESCRIPTION
    Links quesOS (the guest operating system) into a single quesos.mzx image using
    the same C toolchain the rest of the guest world is built with. The compile runs
    inside WSL (the cproc/QBE C toolchain is POSIX-only), but you drive it entirely
    from PowerShell: WSL is the invisible substrate.

    With no arguments the image is written beside its source at os\quesos\quesos.mzx.
    Once built, load it directly with `maize <path>`; a quesOS image is portable Maize
    bytecode and runs on the native Windows maize.exe no matter which host built it.

    Requires WSL with the Maize C cross-toolchain provisioned (run scripts\install-mazm.ps1
    once to set that up). If WSL is not installed the script stops with a clear message.

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
    [string]$Out = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$RepoRoot = Split-Path -Parent $PSScriptRoot

# Resolve the output image path (default: beside the source), always to an absolute
# Windows path so the WSL translation and the run-it hint below are unambiguous.
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

# WSL is required: the cproc/QBE C toolchain is POSIX-only. Fail fast with a clear
# message rather than a raw .NET exception when wsl.exe is not on PATH.
$wsl = Get-Command wsl.exe -ErrorAction SilentlyContinue
if (-not $wsl) {
    Write-Error 'WSL is required to build quesOS (the C toolchain is POSIX-only) but wsl.exe was not found on PATH. Install WSL, or build from an MSYS2 shell instead.'
    exit 2
}

# Translate the repo root and the output path to WSL paths BEFORE building the bash
# command, so the command is a single fully-resolved literal with no bash-side variable
# expansion (forward-slash first: a backslash Windows path handed to wslpath collapses).
$wslRepo = (& wsl.exe wslpath ($RepoRoot -replace '\\', '/')).Trim()
$wslOut = (& wsl.exe wslpath ($OutPath -replace '\\', '/')).Trim()

# One bash statement, nothing chained after it, so $LASTEXITCODE is exactly the build
# script's own exit code (a trailing statement would mask it).
$cmd = "'$wslRepo/os/quesos/build-quesos.sh'"
if ($Preset -ne '') {
    $cmd += " --preset '$Preset'"
}
$cmd += " -o '$wslOut'"

# Windows PowerShell 5.1 turns any native-command stderr into a terminating
# NativeCommandError while ErrorActionPreference is 'Stop'; the build writes progress to
# stderr, so relax it for exactly the wsl.exe call and restore it after (pwsh 7 is
# unaffected either way).
$prevEap = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
& wsl.exe -e bash -lc $cmd
$code = $LASTEXITCODE
$ErrorActionPreference = $prevEap

if ($code -eq 0) {
    Write-Host "Built $OutPath"
    Write-Host "Run it with: maize `"$OutPath`""
}
exit $code
