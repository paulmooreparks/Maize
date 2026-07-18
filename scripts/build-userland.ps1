#Requires -Version 5.1
<#
.SYNOPSIS
    Build the vendored userland programs (the wave-1 /bin set) into Maize images.

.DESCRIPTION
    Compiles the vendored userland (the sbase coreutils plus the oksh shell) into one
    .mzx image per program using the Maize C toolchain. The compile runs inside WSL (the
    cproc/QBE C toolchain is POSIX-only), but you drive it entirely from PowerShell.

    With no program names, the full default set is built. Name one or more programs to
    build just those. By default the images are staged into %USERPROFILE%\.maize\root\bin,
    which is the guest's /bin under Maize's default sandbox root, so the programs appear
    at /bin when you run maize without a custom root.

    Requires WSL with the Maize C cross-toolchain provisioned (run scripts\install-mazm.ps1
    once to set that up). If WSL is not installed the script stops with a clear message.

.PARAMETER Preset
    Build preset to compile against. When omitted, the underlying build picks the
    default preset for the platform.

.PARAMETER Out
    Directory to stage the built images into. Defaults to %USERPROFILE%\.maize\root\bin
    (the guest /bin under the default sandbox root).

.PARAMETER Prog
    Zero or more program names to build (for example: ls cat oksh). With none named, the
    full default userland set is built.

.EXAMPLE
    .\scripts\build-userland.ps1
    Builds the full default set into %USERPROFILE%\.maize\root\bin.

.EXAMPLE
    .\scripts\build-userland.ps1 -Out C:\tmp\bin true false ls
    Builds just true, false, and ls into the given directory.
#>
[CmdletBinding()]
param(
    [string]$Preset = '',
    [string]$Out = '',
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$Prog = @()
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

# WSL is required: the cproc/QBE C toolchain is POSIX-only. Fail fast with a clear
# message rather than a raw .NET exception when wsl.exe is not on PATH.
$wsl = Get-Command wsl.exe -ErrorAction SilentlyContinue
if (-not $wsl) {
    Write-Error 'WSL is required to build the userland (the C toolchain is POSIX-only) but wsl.exe was not found on PATH. Install WSL, or build from an MSYS2 shell instead.'
    exit 2
}

# Translate the repo root and the output directory to WSL paths BEFORE building the bash
# command, so the command is a single fully-resolved literal with no bash-side variable
# expansion (forward-slash first: a backslash Windows path handed to wslpath collapses).
$wslRepo = (& wsl.exe wslpath ($RepoRoot -replace '\\', '/')).Trim()
$wslOut = (& wsl.exe wslpath ($OutDir -replace '\\', '/')).Trim()

# One bash statement, nothing chained after it, so $LASTEXITCODE is exactly the build
# script's own exit code. The program names (if any) forward through as trailing
# positional args; with none, build-userland.sh applies its own default set.
$cmd = "'$wslRepo/userland/build-userland.sh'"
if ($Preset -ne '') {
    $cmd += " --preset '$Preset'"
}
$cmd += " --out '$wslOut'"
foreach ($p in $Prog) {
    $cmd += " '$p'"
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
    Write-Host "Staged userland images in $OutDir"
    if (-not $OutOverridden) {
        Write-Host 'These appear at guest /bin when you run maize with the default sandbox root.'
        $RootDir = Join-Path $HOME '.maize\root'
        if (-not (Test-Path $RootDir)) {
            Write-Warning "The default sandbox root $RootDir does not exist yet, so the guest /bin is not populated. Run ``maize`` once with the default sandbox root (no --no-root) to auto-create it, or create the directory yourself."
        }
    }
}
exit $code
