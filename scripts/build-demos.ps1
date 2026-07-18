#Requires -Version 5.1

<#
.SYNOPSIS
    Build the Maize demo programs (kilo and doom) into runnable images.

.DESCRIPTION
    Compiles the operator-facing demo guest programs into .mzx images using the Maize C
    toolchain. The compile runs inside WSL (the cproc/QBE C toolchain is POSIX-only), but
    you drive it entirely from PowerShell.

    With no demo names, the full default set is built: kilo (a terminal text editor) and
    doom. Name one or more demos to build just those. By default the images are staged into
    %USERPROFILE%\.maize\root\bin, which is the guest's /bin under Maize's default sandbox
    root, so the demos appear at /bin when you run maize without a custom root.

    Building doom needs its engine source fetched first
    (git submodule update --init demos/doom/doomgeneric); the script stops with that exact
    command if it is missing. This script never supplies or touches a DOOM WAD: doom.mzx is
    just the engine, and you provide your own WAD (see demos\doom\README.md) at
    %USERPROFILE%\.maize\root\home\user\doom\doom1.wad or via a mount at run time.

    Requires WSL with the Maize C cross-toolchain provisioned (run scripts\install-mazm.ps1
    once to set that up). If WSL is not installed the script stops with a clear message.

.PARAMETER Preset
    Build preset to compile against. When omitted, the underlying build picks the
    default preset for the platform.

.PARAMETER Out
    Directory to stage the built images into. Defaults to %USERPROFILE%\.maize\root\bin
    (the guest /bin under the default sandbox root).

.PARAMETER Demo
    Zero or more demo names to build (kilo, doom). With none named, both are built.

.EXAMPLE
    .\scripts\build-demos.ps1
    Builds kilo and doom into %USERPROFILE%\.maize\root\bin.

.EXAMPLE
    .\scripts\build-demos.ps1 -Out C:\tmp\bin kilo
    Builds just kilo into the given directory.
#>
# PositionalBinding=$false: -Preset and -Out are named-only, so bare positional args
# (the demo names) all flow to -Demo rather than the first one binding to -Preset.
[CmdletBinding(PositionalBinding = $false)]
param(
    [string]$Preset = '',
    [string]$Out = '',
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$Demo = @()
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$RepoRoot = Split-Path -Parent $PSScriptRoot

# Track whether the caller overrode -Out: the default-destination reminder and the
# missing-root warning below only apply when the default guest-/bin staging is in effect.
$OutOverridden = ($Out -ne '')
if ($OutOverridden) {
    $OutDir = $Out
}
else {
    $OutDir = Join-Path $HOME '.maize\root\bin'
}
if (-not [System.IO.Path]::IsPathRooted($OutDir)) {
    $OutDir = Join-Path (Get-Location).Path $OutDir
}
$OutDir = [System.IO.Path]::GetFullPath($OutDir)

# Snapshot whether the default sandbox root exists BEFORE the build. The build creates
# <root>\bin (and thus <root>) via mkdir -p, so a post-build check would always find it;
# capturing the first-run case up front is what lets the OQ-9246 warning fire. Only
# meaningful when the default -Out is in effect.
$RootDir = Join-Path $HOME '.maize\root'
$RootMissingBefore = (-not $OutOverridden) -and (-not (Test-Path $RootDir))

# WSL is required: the cproc/QBE C toolchain is POSIX-only. Fail fast with a clear
# message rather than a raw .NET exception when wsl.exe is not on PATH.
$wsl = Get-Command wsl.exe -ErrorAction SilentlyContinue
if (-not $wsl) {
    # -ErrorAction Continue: Set-StrictMode/EAP='Stop' would otherwise make Write-Error
    # terminating, so the process would die on the throw (exit 1) before reaching exit 2.
    Write-Error 'WSL is required to build the demos (the C toolchain is POSIX-only) but wsl.exe was not found on PATH. Install WSL, or build from an MSYS2 shell instead.' -ErrorAction Continue
    exit 2
}

# Translate the repo root and the output directory to WSL paths BEFORE building the bash
# command, so the command is a single fully-resolved literal with no bash-side variable
# expansion (forward-slash first: a backslash Windows path handed to wslpath collapses).
$wslRepo = (& wsl.exe wslpath ($RepoRoot -replace '\\', '/')).Trim()
$wslOut = (& wsl.exe wslpath ($OutDir -replace '\\', '/')).Trim()

# One bash statement, nothing chained after it, so $LASTEXITCODE is exactly the build
# script's own exit code. The demo names (if any) forward through as trailing positional
# args; with none, build-demos.sh applies its own default set (kilo doom).
$cmd = "'$wslRepo/demos/build-demos.sh'"
if ($Preset -ne '') {
    $cmd += " --preset '$Preset'"
}
$cmd += " --out '$wslOut'"
foreach ($d in $Demo) {
    $cmd += " '$d'"
}

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
    Write-Host "Staged demo images in $OutDir"
    Write-Host 'doom.mzx needs a WAD you supply: drop one at %USERPROFILE%\.maize\root\home\user\doom\doom1.wad (default root), or mount one at run time. See demos\doom\README.md.'
    if (-not $OutOverridden) {
        Write-Host 'These appear at guest /bin when you run maize with the default sandbox root.'
        if ($RootMissingBefore) {
            Write-Warning "The default sandbox root $RootDir did not exist, so the guest /bin is not set up yet. Run ``maize`` once with the default sandbox root (no --no-root) to auto-create it, or create the directory yourself."
        }
    }
}
exit $code
