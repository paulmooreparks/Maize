#Requires -Version 5.1
<#
.SYNOPSIS
    Build the Maize v2 binaries (mzvm, mzvmg, mzasm) and install stable copies into ~\bin (Windows).

.DESCRIPTION
    Configures the CMake preset, builds the mzvm/mzvmg/mzasm targets, and copies each
    built .exe to the install directory (default: $HOME\bin). SDL2 is still fetched
    and its runtime DLL installed alongside mzvmg.exe, but nothing links it yet: v1's
    maizeg carried the window backend and is archived, and mzvmg's display device has
    not landed (maize-456). So MAIZE_DISPLAY=ON opens no window today, and the SDL2
    provisioning is there to keep the pinned, verified copy on the machine and beside
    mzvmg.exe for when the port arrives.
    If the install directory is not on the user PATH it is appended, so editors and
    shells find the tools without per-workspace configuration. Wired to the default
    build task (Ctrl+Shift+B) via .vscode/tasks.json, which runs this script on every
    press so the binaries it just built are the ones on PATH (maize-454).

    maize-454: the installed set is the v2 machine and the v2 assembler only. mzld,
    mzdis and mzcc keep their names under maize-422 D-1 but have not been ported yet
    (maize-423/424/425/426), so installing today's v1 builds of them would put tools
    on PATH that cannot read a v2 object; each comes back here as its parity card
    lands.

    maize-450: -WithCToolchain and -NoPgo are both gone with the v1 build.
    -WithCToolchain named mzcc plus the mazm, maize and mzld its resolver requires,
    and none of those targets exists in this tree's CMakeLists any more. -NoPgo
    controlled MAIZE_PGO, which applied a committed Clang profile to v1's interpreter
    and nothing else; the profiles were trained against that interpreter, so feeding
    them to mzvm would apply a profile that means nothing while looking like it means
    something. Tuning the v2 interpreter is its own card. v1 is archived: its sources
    are still here to port from, and it still builds, installs and tests on the `v1`
    branch.

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

# --- llvm-mingw compiler toolchain -------------------------------------------------
# windows-llvm-mingw-release resolves CMAKE_C/CXX_COMPILER through
# cmake/ToolchainRoot.cmake (maize-439), which looks in the per-user, version-keyed
# location first and the in-repo .toolchains/ fallback second. Ask the same resolver
# here, and when nothing answers, auto-fetch the pinned toolchain via
# bootstrap-toolchain.ps1 (SHA256-verified, the counterpart of bootstrap-sdl2.ps1) the
# same way the SDL2 block below does. Runs regardless of -Headless: a compiler is
# needed for every build, unlike the display backend.
. (Join-Path $ScriptDir 'lib/ToolchainRoot.ps1')

$ToolchainDir = Resolve-MaizeToolchainDir -Tool 'llvm-mingw' -ProbeRelativePath 'bin/x86_64-w64-mingw32-clang++.exe'
if (-not $ToolchainDir) {
    $checked = (Get-MaizeToolchainCandidateDirs -Tool 'llvm-mingw') -join ', '
    Write-Host "Vendored llvm-mingw compiler not found (checked $checked); fetching it via bootstrap-toolchain.ps1 ..."
    & (Join-Path $ScriptDir 'bootstrap-toolchain.ps1')
    $ToolchainDir = Resolve-MaizeToolchainDir -Tool 'llvm-mingw' -ProbeRelativePath 'bin/x86_64-w64-mingw32-clang++.exe'
    if ($LASTEXITCODE -ne 0 -or -not $ToolchainDir) {
        Write-Error "llvm-mingw provisioning failed (bootstrap-toolchain.ps1 exit $LASTEXITCODE). The '$Preset' preset requires the vendored compiler; run 'scripts/bootstrap-toolchain.ps1' to diagnose." -ErrorAction Continue
        exit 2
    }
}
Write-Host "Using llvm-mingw at $ToolchainDir"

# --- SDL2 provisioning (MAIZE_DISPLAY) --------------------------------------------
# The window backend will need the vendored mingw SDL2 (dev config + DLL), resolved
# through the same per-user-then-in-repo order as the compiler above (maize-439). No
# binary consumes it yet (maize-450 archived v1's maizeg; mzvmg's display device is
# maize-456), so this block provisions rather than enables. This install is
# display-supporting BY DEFAULT: when the SDL2 libs are missing (fresh checkout, a
# clean, the toolchain wiped) they are auto-fetched via bootstrap-sdl2.ps1 (pinned +
# SHA256-verified, the counterpart of bootstrap-toolchain.ps1), so the pinned copy is
# already in place when the port lands. Pass -Headless to skip the fetch (e.g. a
# headless server, or an install that wants no download). Both branches pass MAIZE_DISPLAY
# EXPLICITLY: a bare configure would inherit a stale MAIZE_DISPLAY=ON from a prior
# CMakeCache and then hard-fail find_package(SDL2 REQUIRED) once SDL2 went missing,
# which was the recurring "install suddenly breaks" trap.
$Sdl2Probe = 'lib/cmake/SDL2/sdl2-config.cmake'
$Sdl2Root  = Resolve-MaizeToolchainDir -Tool 'sdl2' -ProbeRelativePath $Sdl2Probe

if ($Headless) {
    Write-Warning "-Headless: configuring MAIZE_DISPLAY=OFF and skipping the SDL2 fetch. No binary links SDL2 yet, so the machines you get are the same either way."
    $displayOn   = $false
    $displayArgs = @('-DMAIZE_DISPLAY=OFF')
}
else {
    if (-not $Sdl2Root) {
        $checked = (Get-MaizeToolchainCandidateDirs -Tool 'sdl2') -join ', '
        Write-Host "Vendored SDL2 not found (checked $checked); fetching it via bootstrap-sdl2.ps1 ..."
        & (Join-Path $ScriptDir 'bootstrap-sdl2.ps1')
        $Sdl2Root = Resolve-MaizeToolchainDir -Tool 'sdl2' -ProbeRelativePath $Sdl2Probe
        if ($LASTEXITCODE -ne 0 -or -not $Sdl2Root) {
            Write-Error "SDL2 provisioning failed (bootstrap-sdl2.ps1 exit $LASTEXITCODE). No binary links SDL2 on this branch, so nothing you build today is degraded by its absence. The fetch is still a hard prerequisite of the default install so that the pinned, SHA256-verified SDL2 is already on this machine, and already beside mzvmg.exe, when the display device lands (maize-456), rather than being scrambled for then on a machine that may be offline. Run 'scripts/bootstrap-sdl2.ps1' to diagnose, or pass -Headless to install without it." -ErrorAction Continue
            exit 2
        }
    }
    $Sdl2CmakeDir = Join-Path $Sdl2Root 'lib/cmake/SDL2'
    # Read by the DLL-install step near the end of this script. It is assigned only on
    # this branch because that step runs only when $displayOn, and Set-StrictMode
    # would fault on an unassigned variable rather than treating it as empty.
    $Sdl2Dll      = Join-Path $Sdl2Root 'bin/SDL2.dll'
    $displayOn    = $true
    $displayArgs  = @('-DMAIZE_DISPLAY=ON', "-DSDL2_DIR=$(($Sdl2CmakeDir) -replace '\\','/')")
}

# --- Resolve cmake --------------------------------------------------------------
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
# Always reconfigure (idempotent, ~1s with Ninja) so the display cache var is applied
# even to a build directory first configured without it.
Write-Host "Configuring preset '$Preset'$(if ($displayOn) { ' with SDL2 provisioned' })..."
& $Cmake --preset $Preset @displayArgs

# maize-439: a build directory configured before the toolchain move can never pick up
# cmake/ToolchainRoot.cmake, because CMake only re-includes a toolchain file in a
# directory that was FIRST configured with one, so CMakeLists.txt refuses it. The
# refusal is right and its message explains the one-time repair, but this script OWNS
# build/$Preset end to end, so making the operator perform that repair by hand on their
# primary install path is asking them to fix something we can just fix. Retry once from
# scratch.
#
# The condition is read off the cache rather than off the preset name, so it fires only
# where the toolchain file is genuinely in play: the failed configure leaves
# CMAKE_TOOLCHAIN_FILE naming ToolchainRoot.cmake (CMake caches a command-line -D even
# on a run that errors, measured on this card), while the stamp the toolchain file
# writes is absent because it never ran. windows-msys2-* and the POSIX presets name no
# toolchain file at all, so they never match and are never deleted.
#
# Deleting the directory rather than passing --fresh: --fresh needs CMake 3.24 and this
# project's floor is 3.21, the two are equivalent (both discard the cache and
# CMakeFiles), and deletion works on every version the project supports.
if ($LASTEXITCODE -ne 0) {
    $CacheFile = Join-Path $BuildDir 'CMakeCache.txt'
    if ((Test-Path $CacheFile) -and
        (Select-String -Path $CacheFile -Pattern '^CMAKE_TOOLCHAIN_FILE.*ToolchainRoot\.cmake' -Quiet) -and
        -not (Select-String -Path $CacheFile -Pattern '^MAIZE_RESOLVED_TOOLCHAIN_DIR' -Quiet)) {
        Write-Warning "Build directory $BuildDir predates the maize-439 toolchain move, so cmake/ToolchainRoot.cmake cannot run in it. Deleting it and configuring once from scratch."
        Remove-Item -Recurse -Force $BuildDir
        & $Cmake --preset $Preset @displayArgs
    }
}

if ($LASTEXITCODE -ne 0) {
    Write-Error "cmake configure failed for preset '$Preset' (exit $LASTEXITCODE)." -ErrorAction Continue
    exit 2
}

# maize-418: mzvm is the console-subsystem Maize v2 machine (terminal I/O); mzvmg is the
# graphical one, whose display device has not landed yet (maize-456), so today it is a
# name-reserving twin of mzvm. maize-422 (D-1): mzasm is the v2 assembler.
$InstallTools = @('mzvm', 'mzvmg', 'mzasm')
$BuildTargets = $InstallTools
$TargetList = $BuildTargets -join ', '

Write-Host "Building $TargetList ($Preset)..."
& $Cmake --build $BuildDir --target @BuildTargets
if ($LASTEXITCODE -ne 0) {
    Write-Error "cmake build failed (exit $LASTEXITCODE)." -ErrorAction Continue
    exit 2
}

# --- Install ----------------------------------------------------------------------
New-Item -ItemType Directory -Force $InstallDir | Out-Null
# maize-418: mzvm is the console-subsystem Maize v2 machine (terminal I/O); mzvmg is the
# graphical one, whose display device has not landed yet (maize-456), so today it is a
# name-reserving twin of mzvm. maize-422 (D-1): mzasm is the v2 assembler.
# maize-454: mzld, mzdis and mzcc are not installed, because their v2 ports have not
# landed yet.
$CopyTools = $InstallTools
foreach ($tool in $CopyTools) {
    $builtExe = Join-Path $BuildDir "$tool.exe"
    if (-not (Test-Path $builtExe)) {
        Write-Error "build reported success but $builtExe does not exist."
        exit 2
    }
    Copy-Item $builtExe (Join-Path $InstallDir "$tool.exe") -Force
    # Copy-Item preserves the source artifact's mtime, so an up-to-date
    # incremental reinstall would leave an old timestamp on the installed copy
    # and look stale. Stamp it to now so a completed install always shows fresh (maize-366).
    (Get-Item (Join-Path $InstallDir "$tool.exe")).LastWriteTime = Get-Date
    Write-Host "Installed $builtExe -> $(Join-Path $InstallDir "$tool.exe")"
}

# mzvmg.exe will link SDL2 dynamically once its display device lands (maize-456). Install
# the runtime DLL alongside it now so it is already in place then ($InstallDir is on PATH,
# so a co-located DLL resolves). Today nothing loads it and its presence changes nothing.
if ($displayOn) {
    if (Test-Path $Sdl2Dll) {
        Copy-Item $Sdl2Dll (Join-Path $InstallDir 'SDL2.dll') -Force
        # Stamp the copied DLL to now for the same reason as the tool exes above (maize-366).
        (Get-Item (Join-Path $InstallDir 'SDL2.dll')).LastWriteTime = Get-Date
        Write-Host "Installed $Sdl2Dll -> $(Join-Path $InstallDir 'SDL2.dll')"
    }
    else {
        Write-Warning "MAIZE_DISPLAY is ON but $Sdl2Dll is missing, so it was not installed beside mzvmg.exe. Nothing links SDL2 yet (maize-456), so every binary this install produced still runs; re-run scripts/bootstrap-sdl2.ps1 before that changes."
    }
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
# Deliberately-broken stdin probe: proves the installed mzasm supports the editor's
# --stdin diagnostics path (exit 1 + marker line), independent of whether any repo
# .mzasm file currently assembles.
#
# The probe WRITES TO STDERR ON PURPOSE. Under Windows PowerShell 5.1,
# ErrorActionPreference=Stop turns redirected native stderr into a terminating
# NativeCommandError, so relax it for exactly this pipeline (pwsh 7 is
# unaffected either way).
$prevEap = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
$probeOut = ('no_such_instruction' | & (Join-Path $InstallDir 'mzasm.exe') --check --stdin --base-path $env:TEMP --source-name mzasm-install-probe 2>&1 | Out-String)
$probeExit = $LASTEXITCODE
$ErrorActionPreference = $prevEap

if ($probeExit -ne 1 -or $probeOut -notmatch 'mzasm-install-probe:1: error:') {
    Write-Error "installed mzasm failed the --stdin probe smoke test (exit $probeExit)."
    exit 1
}

# mzvm smoke: no image argument prints the usage line to stderr and exits 2. Same
# stderr-under-5.1 caveat as above, so the same relaxed-EAP pipeline.
$prevEap = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
$vmOut = (& (Join-Path $InstallDir 'mzvm.exe') 2>&1 | Out-String)
$vmExit = $LASTEXITCODE
$ErrorActionPreference = $prevEap

if ($vmExit -ne 2 -or $vmOut -notmatch 'usage: mzvm') {
    Write-Error "installed mzvm failed the usage smoke test (exit $vmExit)."
    exit 1
}

# maize-450: the v1 C pipeline used to be installed here behind -WithCToolchain. It wrote
# the mzcc.cmd Git Bash forwarder into the install directory and built the cproc-qbe and
# qbe cross-toolchain, all of which produce v1 guest code. It is archived with the rest of
# v1 rather than kept as a switch nothing can satisfy, since the mzcc target it forwarded
# to no longer exists in this tree's CMakeLists.
#
# An mzcc.cmd from an earlier install is deliberately left alone. It points at
# scripts/cc-maize.sh, which is still in the tree, so it keeps working for as long as a
# v1 toolchain build survives in the build directory, and removing a tool from somebody's
# PATH is not this script's call to make.

# Resolve the git revision the tree was built from, for a visible provenance
# stamp in the summary line. git describe --always --dirty yields the nearest
# tag (or abbreviated hash) plus a -dirty suffix when the tree has uncommitted
# changes, in one call. Probe git via Get-Command first (the same idiom the
# cmake resolution above uses) so a machine without git degrades to "unknown";
# relax ErrorActionPreference around the call (matching the smoke-check blocks
# above) so a non-repo checkout does not abort under EAP=Stop (maize-366).
$gitCmd = Get-Command git -ErrorAction SilentlyContinue
if ($gitCmd) {
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $Revision = (& $gitCmd.Source -C $RepoRoot describe --always --dirty 2>$null | Out-String).Trim()
    if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($Revision)) {
        $Revision = 'unknown'
    }
    $ErrorActionPreference = $prevEap
}
else {
    $Revision = 'unknown'
}

Write-Host "Installed $($CopyTools -join ', ') to $InstallDir (built from $Revision)."
exit 0
