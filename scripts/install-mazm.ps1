#Requires -Version 5.1
<#
.SYNOPSIS
    Build the Maize toolchain (maize, mazm, mzld, mzdis) and install stable copies into ~\bin (Windows).

.DESCRIPTION
    Configures the CMake preset, builds the maize/mazm/mzld/mzdis targets as an
    optimized Release, and copies each built .exe to the install directory
    (default: $HOME\bin). The maize VM is built with the SDL2 window backend
    (MAIZE_DISPLAY=ON) so `--display --input=keyboard` opens a real window; the
    vendored SDL2 runtime (SDL2.dll) is installed alongside maize.exe. Also
    refreshes the mzcc.cmd Windows forwarder (the C-toolchain entry point that
    dispatches into the WSL driver) from a repo template. If the install directory
    is not on the user PATH it is appended, so editors and shells find the tools
    without per-workspace configuration. Wired to the default build task
    (Ctrl+Shift+B) via .vscode/tasks.json.

    Never prompts; safe for non-interactive use.

.PARAMETER Preset
    CMake preset to build. Defaults to windows-llvm-mingw-release (optimized).

.PARAMETER InstallDir
    Destination directory. Defaults to $HOME\bin.
#>
[CmdletBinding()]
param(
    [string]$Preset = 'windows-llvm-mingw-release',
    [string]$InstallDir = (Join-Path $HOME 'bin'),
    [switch]$Headless
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot  = Resolve-Path (Join-Path $ScriptDir '..')
$BuildDir  = Join-Path $RepoRoot "build/$Preset"

# --- SDL2 window backend (MAIZE_DISPLAY) ------------------------------------------
# maize's --display window backend needs the vendored mingw SDL2 (dev config + DLL)
# under .toolchains/SDL2. This install is display-supporting BY DEFAULT: when the SDL2
# libs are missing (fresh checkout, a clean, .toolchains wiped) they are auto-fetched
# via bootstrap-sdl2.ps1 (pinned + SHA256-verified, the counterpart of
# bootstrap-toolchain.ps1) rather than silently degrading to a headless maize. Pass
# -Headless to opt out (e.g. a headless server). Both branches pass MAIZE_DISPLAY
# EXPLICITLY: a bare configure would inherit a stale MAIZE_DISPLAY=ON from a prior
# CMakeCache and then hard-fail find_package(SDL2 REQUIRED) once SDL2 went missing,
# which was the recurring "install suddenly breaks" trap.
$Sdl2Root     = Join-Path $RepoRoot '.toolchains/SDL2/x86_64-w64-mingw32'
$Sdl2CmakeDir = Join-Path $Sdl2Root 'lib/cmake/SDL2'
$Sdl2Dll      = Join-Path $Sdl2Root 'bin/SDL2.dll'

if ($Headless) {
    Write-Warning "-Headless: building maize WITHOUT the --display window backend."
    $displayOn   = $false
    $displayArgs = @('-DMAIZE_DISPLAY=OFF')
}
else {
    if (-not (Test-Path $Sdl2CmakeDir)) {
        Write-Host "Vendored SDL2 not found at $Sdl2CmakeDir; fetching it via bootstrap-sdl2.ps1 ..."
        & (Join-Path $ScriptDir 'bootstrap-sdl2.ps1')
        if ($LASTEXITCODE -ne 0 -or -not (Test-Path $Sdl2CmakeDir)) {
            Write-Error "SDL2 provisioning failed (bootstrap-sdl2.ps1 exit $LASTEXITCODE). The --display build requires SDL2; run 'scripts/bootstrap-sdl2.ps1' to diagnose, or pass -Headless to build without the window backend. Refusing to silently build a headless maize."
            exit 2
        }
    }
    $displayOn   = $true
    $displayArgs = @('-DMAIZE_DISPLAY=ON', "-DSDL2_DIR=$(($Sdl2CmakeDir) -replace '\\','/')")
}

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

# --- Configure and build ----------------------------------------------------------
# Always reconfigure (idempotent, ~1s with Ninja) so the display cache vars are applied
# even to a build directory first configured without them.
Write-Host "Configuring preset '$Preset'$(if ($displayOn) { ' with SDL2 window backend' })..."
& $Cmake --preset $Preset @displayArgs
if ($LASTEXITCODE -ne 0) {
    Write-Error "cmake configure failed for preset '$Preset' (exit $LASTEXITCODE)."
    exit 2
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

# maize.exe now links SDL2 dynamically; install the runtime DLL alongside it so it
# starts from anywhere on PATH ($InstallDir is on PATH, so a co-located DLL resolves).
if ($displayOn) {
    if (Test-Path $Sdl2Dll) {
        Copy-Item $Sdl2Dll (Join-Path $InstallDir 'SDL2.dll') -Force
        Write-Host "Installed $Sdl2Dll -> $(Join-Path $InstallDir 'SDL2.dll')"
    }
    else {
        Write-Warning "MAIZE_DISPLAY is ON but $Sdl2Dll is missing; maize.exe will fail to start until SDL2.dll is on PATH."
    }
}

# --- Windows forwarder: refresh <InstallDir>\mzcc.cmd from the repo template --------
# mzcc.cmd is the Windows entry point for the C toolchain: it wslpath-translates
# the source path and calls the WSL-side ~/bin/mzcc (regenerated by
# scripts/refresh-c-toolchain.sh), which in turn execs scripts/cc-maize.sh, the single
# canonical C driver. Rewriting it here on every build keeps it build-managed alongside
# the four .exe surfaces and structurally unable to go stale (maize-96 OQ2). Renamed
# from maize-cc to fit the mz* tool family (mzld, mzdis); the legacy name is removed
# below so two names cannot drift.
$mzccCmd = @'
@echo off
rem mzcc: compile a C source through the Maize C toolchain (gcc-like CLI).
rem cproc/qbe are POSIX-only, so the pipeline runs in WSL; this forwarder
rem translates the Windows path and invokes ~/bin/mzcc inside WSL, which
rem execs scripts/cc-maize.sh (the single canonical C driver).
rem   mzcc <file.c>          compile+link to <file>.mzx beside the source (no run)
rem   mzcc <file.c> -r       compile and run, propagating the guest exit code
rem   mzcc <file.c> --emit   also leave <file>.mazm (qbe body) beside the source
rem   mzcc --build           rebuild the cproc/qbe toolchain
setlocal enabledelayedexpansion
if "%~1"=="" (
  echo usage: mzcc ^<file.c^> [-r ^| --emit]   ^(also: mzcc --build^)
  exit /b 2
)
if /I "%~1"=="--build" (
  wsl.exe -e bash -lc "~/bin/mzcc --build"
  exit /b !errorlevel!
)
for /f "usebackq delims=" %%p in (`wsl.exe wslpath "%~f1"`) do set "WSLSRC=%%p"
wsl.exe -e bash -lc "~/bin/mzcc '!WSLSRC!' %~2"
exit /b !errorlevel!
'@
$cmdPath = Join-Path $InstallDir 'mzcc.cmd'
Set-Content -Path $cmdPath -Value $mzccCmd -Encoding Ascii
Write-Host "Refreshed $cmdPath (Windows forwarder to WSL mzcc)."

# Retire the pre-rename forwarder so a stale maize-cc.cmd can't shadow or drift.
$legacyCmd = Join-Path $InstallDir 'maize-cc.cmd'
if (Test-Path $legacyCmd) {
    Remove-Item $legacyCmd -Force
    Write-Host "Removed legacy $legacyCmd (renamed to mzcc.cmd)."
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

# --- C cross-toolchain refresh (cproc/qbe + linux-debug mazm/maize for mzcc) -------
# cproc/qbe are POSIX-only (they cannot be built as native Windows PE binaries;
# see toolchain/VENDORING.md), so this runs under WSL. Non-fatal: the native tools
# above are already installed and smoke-checked, so a missing WSL or a toolchain
# hiccup only warns. WSL writes build output to stderr; under Windows PowerShell
# 5.1 with ErrorActionPreference=Stop that would become a terminating
# NativeCommandError, so relax it for exactly this call (pwsh 7 is unaffected).
$wsl = Get-Command wsl.exe -ErrorAction SilentlyContinue
if (-not $wsl) {
    Write-Warning 'WSL not found; skipping C cross-toolchain (cproc/qbe) refresh. mzcc will use any previously built toolchain.'
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
