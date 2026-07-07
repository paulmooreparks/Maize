#Requires -Version 5.1
<#
.SYNOPSIS
    Build mazm and install a stable copy into ~\bin (Windows).

.DESCRIPTION
    Configures the CMake preset if needed, builds the mazm target, and copies
    mazm.exe to the install directory (default: $HOME\bin). If the install
    directory is not on the user PATH it is appended, so editors and shells
    find `mazm` without per-workspace configuration. Wired to the default
    build task (Ctrl+Shift+B) via .vscode/tasks.json.

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

Write-Host "Building mazm ($Preset)..."
& $Cmake --build $BuildDir --target mazm
if ($LASTEXITCODE -ne 0) {
    Write-Error "cmake build failed (exit $LASTEXITCODE)."
    exit 2
}

$BuiltExe = Join-Path $BuildDir 'mazm.exe'
if (-not (Test-Path $BuiltExe)) {
    Write-Error "build reported success but $BuiltExe does not exist."
    exit 2
}

# --- Install ----------------------------------------------------------------------
New-Item -ItemType Directory -Force $InstallDir | Out-Null
Copy-Item $BuiltExe (Join-Path $InstallDir 'mazm.exe') -Force
Write-Host "Installed $BuiltExe -> $(Join-Path $InstallDir 'mazm.exe')"

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
$probeOut = 'STRING "x' | & (Join-Path $InstallDir 'mazm.exe') --check --stdin --base-path $env:TEMP --source-name mazm-install-probe 2>&1 | Out-String

if ($LASTEXITCODE -ne 1 -or $probeOut -notmatch 'mazm-install-probe:1: error:') {
    Write-Error "installed mazm failed the --stdin probe smoke test (exit $LASTEXITCODE)."
    exit 1
}

Write-Host 'mazm installed and smoke-checked (stdin diagnostics probe passed).'
