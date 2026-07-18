#Requires -Version 5.1

<#
.SYNOPSIS
    Build the vendored C cross-toolchain (cproc + QBE) that the guest-world builds need.

.DESCRIPTION
    Builds the C compiler and code generator (cproc and QBE with the Maize target) that
    the quesOS, userland, and demo builds compile through. The build runs inside WSL (the
    toolchain is POSIX-only), but you drive it entirely from PowerShell.

    Takes no arguments. The first run may take a while (a cold compiler build); later runs
    are faster once the result is cached. On success the tools are left in the repo where
    the other build scripts already look for them, so nothing needs installing.

    Requires WSL with the toolchain source submodules initialized
    (git submodule update --init --recursive). If WSL is not installed the script stops
    with a clear message.

.EXAMPLE
    .\scripts\build-toolchain.ps1
    Builds cproc and QBE so the other guest-world build scripts can compile C.
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$RepoRoot = Split-Path -Parent $PSScriptRoot

# WSL is required: the cproc/QBE toolchain is POSIX-only. Fail fast with a clear message
# rather than a raw .NET exception when wsl.exe is not on PATH.
$wsl = Get-Command wsl.exe -ErrorAction SilentlyContinue
if (-not $wsl) {
    # -ErrorAction Continue: Set-StrictMode/EAP='Stop' would otherwise make Write-Error
    # terminating, so the process would die on the throw (exit 1) before reaching exit 2.
    Write-Error 'WSL is required to build the C toolchain (it is POSIX-only) but wsl.exe was not found on PATH. Install WSL, or build from an MSYS2 shell instead.' -ErrorAction Continue
    exit 2
}

# Translate the repo root to a WSL path BEFORE building the bash command, so the command
# is a single fully-resolved literal with no bash-side variable expansion (forward-slash
# first: a backslash Windows path handed to wslpath collapses).
$wslRepo = (& wsl.exe wslpath ($RepoRoot -replace '\\', '/')).Trim()

# One bash statement, nothing chained after it, so $LASTEXITCODE is exactly the build
# script's own exit code. No arguments: build-toolchain.sh takes none.
$cmd = "'$wslRepo/scripts/build-toolchain.sh'"

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
    Write-Host 'C toolchain (cproc + QBE) built.'
}
exit $code
