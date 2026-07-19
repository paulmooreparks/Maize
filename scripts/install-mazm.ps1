#Requires -Version 5.1
<#
.SYNOPSIS
    Build the Maize toolchain (maize, maizeg, mazm, mzld, mzdis) and install stable copies into ~\bin (Windows).

.DESCRIPTION
    Configures the CMake preset, builds the maize/mazm/mzld/mzdis targets as an
    optimized Release, and copies each built .exe to the install directory
    (default: $HOME\bin). The maizeg VM is built with the SDL2 window backend
    (MAIZE_DISPLAY=ON) so `--display --input=keyboard` opens a real window; the
    vendored SDL2 runtime (SDL2.dll) is installed alongside maizeg.exe. Also
    refreshes the mzcc.cmd Windows forwarder (the C-toolchain entry point that
    dispatches into the WSL driver) from a repo template. If the install directory
    is not on the user PATH it is appended, so editors and shells find the tools
    without per-workspace configuration. Wired to the default build task
    (Ctrl+Shift+B) via .vscode/tasks.json.

    Never prompts; safe for non-interactive use.

    maize-259: when a committed Clang PGO profile exists for the chosen preset
    (scripts/pgo-profiles/<Preset>/default.profdata), the build applies it via
    -DMAIZE_PGO=use: this is what closes the ~26-28% clang-vs-gcc interpreter gap
    on the shipped Windows binary (measured: clang baseline ~18700-19300 us/frame
    on demos/doom's doom_bench workload, clang+PGO ~14333-14366 us/frame, matching
    gcc/Linux's ~14330 us/frame on the same workload). The profile ships in the
    repo, so a fresh clone reproduces this without a separate training pass; pass
    -NoPgo to opt out (e.g. profiling a change to the interpreter itself before a
    retrain). See scripts/build-pgo.ps1 to regenerate the profile and
    scripts/pgo-profiles/README.md for provenance / retrain triggers.

.PARAMETER Preset
    CMake preset to build. Defaults to windows-llvm-mingw-release (optimized).

.PARAMETER InstallDir
    Destination directory. Defaults to $HOME\bin.

.PARAMETER NoPgo
    Build without Clang PGO even when a committed profile exists for this preset.
#>
[CmdletBinding()]
param(
    [string]$Preset = 'windows-llvm-mingw-release',
    [string]$InstallDir = (Join-Path $HOME 'bin'),
    [switch]$Headless,
    [switch]$NoPgo
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot  = Resolve-Path (Join-Path $ScriptDir '..')
$BuildDir  = Join-Path $RepoRoot "build/$Preset"

# --- llvm-mingw compiler toolchain -------------------------------------------------
# windows-llvm-mingw-release hardcodes CMAKE_C/CXX_COMPILER to
# ${sourceDir}/.toolchains/llvm-mingw/bin/x86_64-w64-mingw32-clang(++).exe. On a fresh
# checkout .toolchains/ is empty (gitignored), so auto-fetch the pinned toolchain via
# bootstrap-toolchain.ps1 (SHA256-verified, the counterpart of bootstrap-sdl2.ps1) the
# same way the SDL2 block below does. Runs regardless of -Headless: a compiler is
# needed for every build, unlike the display backend.
$ToolchainDir = Join-Path $RepoRoot '.toolchains/llvm-mingw'
$ClangC       = Join-Path $ToolchainDir 'bin/x86_64-w64-mingw32-clang.exe'
$ClangCxx     = Join-Path $ToolchainDir 'bin/x86_64-w64-mingw32-clang++.exe'

if (-not (Test-Path $ClangC) -or -not (Test-Path $ClangCxx)) {
    Write-Host "Vendored llvm-mingw compiler not found at $ToolchainDir; fetching it via bootstrap-toolchain.ps1 ..."
    & (Join-Path $ScriptDir 'bootstrap-toolchain.ps1')
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $ClangC) -or -not (Test-Path $ClangCxx)) {
        Write-Error "llvm-mingw provisioning failed (bootstrap-toolchain.ps1 exit $LASTEXITCODE). The '$Preset' preset requires the vendored compiler at $ToolchainDir; run 'scripts/bootstrap-toolchain.ps1' to diagnose."
        exit 2
    }
}

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
    Write-Warning "-Headless: building maizeg WITHOUT the --display window backend."
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

# --- Clang PGO (maize-259) --------------------------------------------------------
# A committed, merged profile ships per preset at scripts/pgo-profiles/<Preset>/
# default.profdata (see that directory's README.md for provenance/retrain triggers).
# When present, build against it (-DMAIZE_PGO=use): this is what makes the shipped
# Windows maize/maizeg competitive with the gcc/Linux build (~26-28% faster
# interpreter than a plain clang build). -NoPgo opts out; a missing profile for this
# preset (e.g. windows-msys2-release, which is GCC and MAIZE_PGO ignores) also
# degrades to a plain build, with a warning, rather than failing the install.
$PgoProfileDir = Join-Path $RepoRoot "scripts/pgo-profiles/$Preset"
$PgoProfile    = Join-Path $PgoProfileDir 'default.profdata'
if ($NoPgo) {
    Write-Host "-NoPgo: building '$Preset' WITHOUT Clang PGO."
    $pgoArgs = @('-DMAIZE_PGO=')
}
elseif (Test-Path $PgoProfile) {
    $pgoArgs = @('-DMAIZE_PGO=use', "-DMAIZE_PGO_DIR=$(($PgoProfileDir) -replace '\\','/')")
}
else {
    Write-Warning "No committed PGO profile for preset '$Preset' at $PgoProfile; building without PGO. Run scripts/build-pgo.ps1 to produce one (see scripts/pgo-profiles/README.md)."
    $pgoArgs = @('-DMAIZE_PGO=')
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
# Always reconfigure (idempotent, ~1s with Ninja) so the display/PGO cache vars are
# applied even to a build directory first configured without them.
Write-Host "Configuring preset '$Preset'$(if ($displayOn) { ' with SDL2 window backend' })$(if ($pgoArgs[0] -eq '-DMAIZE_PGO=use') { ' with Clang PGO' })..."
& $Cmake --preset $Preset @displayArgs @pgoArgs
if ($LASTEXITCODE -ne 0) {
    if ($pgoArgs[0] -eq '-DMAIZE_PGO=use') {
        # maize-259 cycle-1 fix: a stale/incompatible committed profile (e.g. after a
        # llvm-mingw major-version bump; profile format/function hashing can shift
        # across Clang versions, see scripts/pgo-profiles/README.md "When to
        # retrain") can turn into a hard configure failure instead of a soft
        # per-function skip. Don't leave the operator with a bare exit code: retry
        # once without PGO and signpost the escape hatch either way.
        Write-Warning "cmake configure failed for preset '$Preset' with Clang PGO active (exit $LASTEXITCODE); the committed profile at $PgoProfile may be incompatible with the current toolchain pin. Retrying once without PGO..."
        $pgoArgs = @('-DMAIZE_PGO=')
        & $Cmake --preset $Preset @displayArgs @pgoArgs
        if ($LASTEXITCODE -ne 0) {
            Write-Error "cmake configure failed for preset '$Preset' (exit $LASTEXITCODE), with and without PGO; not a PGO issue." -ErrorAction Continue
            exit 2
        }
        Write-Warning "Configured '$Preset' WITHOUT Clang PGO after the PGO-enabled configure failed. Retrain the profile with scripts/build-pgo.ps1 (see scripts/pgo-profiles/README.md), or pass -NoPgo to silence this warning."
    }
    else {
        Write-Error "cmake configure failed for preset '$Preset' (exit $LASTEXITCODE)." -ErrorAction Continue
        exit 2
    }
}

Write-Host "Building maize, maizeg, mazm, mzld, mzdis ($Preset)..."
& $Cmake --build $BuildDir --target maize maizeg mazm mzld mzdis
if ($LASTEXITCODE -ne 0) {
    if ($pgoArgs[0] -eq '-DMAIZE_PGO=use') {
        Write-Warning "cmake build failed for preset '$Preset' with Clang PGO active (exit $LASTEXITCODE); the committed profile at $PgoProfile may be incompatible with the current toolchain pin. Reconfiguring and retrying once without PGO..."
        $pgoArgs = @('-DMAIZE_PGO=')
        & $Cmake --preset $Preset @displayArgs @pgoArgs
        if ($LASTEXITCODE -ne 0) {
            Write-Error "cmake reconfigure without PGO failed (exit $LASTEXITCODE)." -ErrorAction Continue
            exit 2
        }
        & $Cmake --build $BuildDir --target maize maizeg mazm mzld mzdis
        if ($LASTEXITCODE -ne 0) {
            Write-Error "cmake build failed for preset '$Preset' (exit $LASTEXITCODE), with and without PGO; not a PGO issue." -ErrorAction Continue
            exit 2
        }
        Write-Warning "Built '$Preset' WITHOUT Clang PGO after the PGO-enabled build failed. Retrain the profile with scripts/build-pgo.ps1 (see scripts/pgo-profiles/README.md), or pass -NoPgo to silence this warning."
    }
    else {
        Write-Error "cmake build failed (exit $LASTEXITCODE)." -ErrorAction Continue
        exit 2
    }
}

# --- Install ----------------------------------------------------------------------
New-Item -ItemType Directory -Force $InstallDir | Out-Null
# maize-217/230: `maize` is the console-subsystem VM (terminal I/O); `maizeg` is the graphical
# one (SDL window). Both are installed; console programs run under maize, the screen under maizeg.
foreach ($tool in 'maize', 'maizeg', 'mazm', 'mzld', 'mzdis') {
    $builtExe = Join-Path $BuildDir "$tool.exe"
    if (-not (Test-Path $builtExe)) {
        Write-Error "build reported success but $builtExe does not exist."
        exit 2
    }
    Copy-Item $builtExe (Join-Path $InstallDir "$tool.exe") -Force
    Write-Host "Installed $builtExe -> $(Join-Path $InstallDir "$tool.exe")"
}

# maizeg.exe (graphical) links SDL2 dynamically; install the runtime DLL alongside it so it
# starts from anywhere on PATH ($InstallDir is on PATH, so a co-located DLL resolves).
if ($displayOn) {
    if (Test-Path $Sdl2Dll) {
        Copy-Item $Sdl2Dll (Join-Path $InstallDir 'SDL2.dll') -Force
        Write-Host "Installed $Sdl2Dll -> $(Join-Path $InstallDir 'SDL2.dll')"
    }
    else {
        Write-Warning "MAIZE_DISPLAY is ON but $Sdl2Dll is missing; maizeg.exe will fail to start until SDL2.dll is on PATH."
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

Write-Host 'maize, maizeg, mazm, mzld, mzdis installed and smoke-checked.'
exit 0
