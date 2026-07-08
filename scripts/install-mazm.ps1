#Requires -Version 5.1
<#
.SYNOPSIS
    Build the Maize toolchain (maize, mazm, mzld, mzdis) and install stable copies into ~\bin (Windows).

.DESCRIPTION
    Configures the CMake preset if needed, builds the maize/mazm/mzld/mzdis
    targets, and copies each built .exe to the install directory (default:
    $HOME\bin). If the install directory is not on the user PATH it is
    appended, so editors and shells find the tools without per-workspace
    configuration. Wired to the default build task (Ctrl+Shift+B) via
    .vscode/tasks.json.

    Never prompts; safe for non-interactive use.

.PARAMETER Preset
    CMake preset to build. Defaults to windows-llvm-mingw-debug.

.PARAMETER InstallDir
    Destination directory. Defaults to $HOME\bin.
#>
[CmdletBinding()]
param(
    [string]$Preset = 'windows-llvm-mingw-debug',
    [string]$InstallDir = (Join-Path $HOME 'bin')
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot  = Resolve-Path (Join-Path $ScriptDir '..')
$BuildDir  = Join-Path $RepoRoot "build/$Preset"

# --- Resolve cmake the same way run-tests.ps1 does ------------------------------
$cmakeCmd = Get-Command cmake -ErrorAction SilentlyContinue
if ($cmakeCmd) {
    $Cmake = $cmakeCmd.Source
}
elseif (Test-Path 'C:\Program Files\CMake\bin\cmake.exe') {
    $Cmake = 'C:\Program Files\CMake\bin\cmake.exe'
}
else {
    Write-Error 'cmake not found on PATH or at C:\Program Files\CMake\bin\cmake.exe.'
    exit 2
}

# --- Configure (first run only) and build ----------------------------------------
if (-not (Test-Path (Join-Path $BuildDir 'CMakeCache.txt'))) {
    Write-Host "Configuring preset '$Preset'..."
    & $Cmake --preset $Preset
    if ($LASTEXITCODE -ne 0) {
        Write-Error "cmake configure failed for preset '$Preset' (exit $LASTEXITCODE)."
        exit 2
    }
}

Write-Host "Building maize, mazm, mzld, mzdis ($Preset)..."
& $Cmake --build $BuildDir --target maize mazm mzld mzdis
if ($LASTEXITCODE -ne 0) {
    Write-Error "cmake build failed (exit $LASTEXITCODE)."
    exit 2
}

# --- Install ----------------------------------------------------------------------
New-Item -ItemType Directory -Force $InstallDir | Out-Null
foreach ($tool in 'maize', 'mazm', 'mzld', 'mzdis') {
    $builtExe = Join-Path $BuildDir "$tool.exe"
    if (-not (Test-Path $builtExe)) {
        Write-Error "build reported success but $builtExe does not exist."
        exit 2
    }
    Copy-Item $builtExe (Join-Path $InstallDir "$tool.exe") -Force
    Write-Host "Installed $builtExe -> $(Join-Path $InstallDir "$tool.exe")"
}

# --- Ensure the install dir is on the user PATH -----------------------------------
$userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
$onPath = ($userPath -split ';') | Where-Object { $_ -eq $InstallDir }

if (-not $onPath) {
    [Environment]::SetEnvironmentVariable('Path', "$userPath;$InstallDir", 'User')
    Write-Host "Added $InstallDir to the user PATH."
    Write-Host 'Restart VS Code (and any shells) so they pick up the new PATH.'
}

# --- Smoke check -------------------------------------------------------------------
# Deliberately-broken stdin probe: proves the installed binary supports the
# editor's --stdin diagnostics path (exit 1 + marker line), independent of
# whether any repo .mazm file currently assembles.
#
# The probe WRITES TO STDERR ON PURPOSE. Under Windows PowerShell 5.1,
# ErrorActionPreference=Stop turns redirected native stderr into a terminating
# NativeCommandError, so relax it for exactly this pipeline (pwsh 7 is
# unaffected either way).
$prevEap = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
$probeOut = ('STRING "x' | & (Join-Path $InstallDir 'mazm.exe') --check --stdin --base-path $env:TEMP --source-name mazm-install-probe 2>&1 | Out-String)
$probeExit = $LASTEXITCODE
$ErrorActionPreference = $prevEap

if ($probeExit -ne 1 -or $probeOut -notmatch 'mazm-install-probe:1: error:') {
    Write-Error "installed mazm failed the --stdin probe smoke test (exit $probeExit)."
    exit 1
}

# mzld smoke: no inputs prints the usage line to stderr and exits 1. Same
# stderr-under-5.1 caveat as above, so the same relaxed-EAP pipeline.
$prevEap = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
$ldOut = (& (Join-Path $InstallDir 'mzld.exe') 2>&1 | Out-String)
$ldExit = $LASTEXITCODE
$ErrorActionPreference = $prevEap

if ($ldExit -ne 1 -or $ldOut -notmatch 'usage: mzld') {
    Write-Error "installed mzld failed the usage smoke test (exit $ldExit)."
    exit 1
}

# --- C cross-toolchain refresh (cproc/qbe + linux-debug mazm/maize for maize-cc) ---
# cproc/qbe are POSIX-only (they cannot be built as native Windows PE binaries;
# see toolchain/VENDORING.md), so this runs under WSL. Non-fatal: the native tools
# above are already installed and smoke-checked, so a missing WSL or a toolchain
# hiccup only warns. WSL writes build output to stderr; under Windows PowerShell
# 5.1 with ErrorActionPreference=Stop that would become a terminating
# NativeCommandError, so relax it for exactly this call (pwsh 7 is unaffected).
$wsl = Get-Command wsl.exe -ErrorAction SilentlyContinue
if (-not $wsl) {
    Write-Warning 'WSL not found; skipping C cross-toolchain (cproc/qbe) refresh. maize-cc will use any previously built toolchain.'
}
else {
    Write-Host 'Refreshing C cross-toolchain (cproc/qbe + linux-debug mazm/maize) under WSL...'
    # Forward-slash the path first: passing a backslash Windows path straight to
    # wsl.exe strips the separators (C:\a\b -> C:ab). wslpath accepts forward slashes.
    $wslRepo = (& wsl.exe wslpath ("$RepoRoot" -replace '\\', '/')).Trim()
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    & wsl.exe -e bash -lc "'$wslRepo/scripts/refresh-c-toolchain.sh'"
    $tcExit = $LASTEXITCODE
    $ErrorActionPreference = $prevEap
    if ($tcExit -ne 0) {
        Write-Warning "C cross-toolchain refresh failed (exit $tcExit). Native tools are installed; retry with 'wsl bash scripts/refresh-c-toolchain.sh'."
    }
    else {
        Write-Host 'C cross-toolchain refreshed (cproc + qbe + Maize target).'
    }
}

Write-Host 'maize, mazm, mzld, mzdis installed and smoke-checked.'
exit 0
