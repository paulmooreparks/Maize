#Requires -Version 5.1
<#
.SYNOPSIS
    Build the Maize v2 binaries (mzvm, mzvmg, mzasm) and install stable copies into ~\bin (Windows).

.DESCRIPTION
    Configures the CMake preset, builds the mzvm/mzvmg/mzasm targets, and copies each
    built .exe to the install directory (default: $HOME\bin). The mzvmg VM is built
    with the SDL2 window backend (MAIZE_DISPLAY=ON) so `--display` opens a real
    window; the vendored SDL2 runtime (SDL2.dll) is installed alongside mzvmg.exe.
    If the install directory is not on the user PATH it is appended, so editors and
    shells find the tools without per-workspace configuration. Wired to the default
    build task (Ctrl+Shift+B) via .vscode/tasks.json, which runs this script on every
    press so the binaries it just built are the ones on PATH (maize-454).

    maize-454: the installed set is the v2 machine and the v2 assembler only. The
    frozen v1 binaries (maize, maizeg, mazm) are no longer built or copied. mzld,
    mzdis and mzcc keep their names under maize-422 D-1 but have not been ported yet
    (maize-423/424/425/426), so installing today's v1 builds of them would put tools
    on PATH that cannot read a v2 object; each comes back here as its parity card
    lands. The v1 C pipeline (mzcc plus the cproc-qbe/qbe cross-toolchain) is behind
    -WithCToolchain, opt-in, because it is sometimes a real wait and has no business
    in a loop pressed dozens of times a day.

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

.PARAMETER WithCToolchain
    Also build and install the v1 C pipeline: mzcc (plus the mazm, maize and mzld its
    resolver requires, built into the build directory but not installed), the mzcc.cmd
    Windows forwarder, and the cproc-qbe/qbe cross-toolchain. Off by default so the inner loop stays cheap; the "Install ..."
    VS Code task and build-world.ps1 both pass it.
#>
[CmdletBinding()]
param(
    [string]$Preset = 'windows-llvm-mingw-release',
    [string]$InstallDir = (Join-Path $HOME 'bin'),
    [switch]$Headless,
    [switch]$NoPgo,
    [switch]$WithCToolchain
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

# --- SDL2 window backend (MAIZE_DISPLAY) ------------------------------------------
# maize's --display window backend needs the vendored mingw SDL2 (dev config + DLL),
# resolved through the same per-user-then-in-repo order as the compiler above
# (maize-439). This install is display-supporting BY DEFAULT: when the SDL2
# libs are missing (fresh checkout, a clean, the toolchain wiped) they are auto-fetched
# via bootstrap-sdl2.ps1 (pinned + SHA256-verified, the counterpart of
# bootstrap-toolchain.ps1) rather than silently degrading to a headless maize. Pass
# -Headless to opt out (e.g. a headless server). Both branches pass MAIZE_DISPLAY
# EXPLICITLY: a bare configure would inherit a stale MAIZE_DISPLAY=ON from a prior
# CMakeCache and then hard-fail find_package(SDL2 REQUIRED) once SDL2 went missing,
# which was the recurring "install suddenly breaks" trap.
$Sdl2Probe = 'lib/cmake/SDL2/sdl2-config.cmake'
$Sdl2Root  = Resolve-MaizeToolchainDir -Tool 'sdl2' -ProbeRelativePath $Sdl2Probe

if ($Headless) {
    Write-Warning "-Headless: building maizeg WITHOUT the --display window backend."
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
            Write-Error "SDL2 provisioning failed (bootstrap-sdl2.ps1 exit $LASTEXITCODE). The --display build requires SDL2; run 'scripts/bootstrap-sdl2.ps1' to diagnose, or pass -Headless to build without the window backend. Refusing to silently build a headless maize." -ErrorAction Continue
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
    # Informational, not a warning: since maize-454 the default build task runs this
    # script on every press against a debug preset, which has no committed profile by
    # design, and a yellow warning on every Ctrl+Shift+B would train the operator to
    # ignore the warning stream.
    Write-Host "No committed PGO profile for preset '$Preset' at $PgoProfile; building without PGO. Run scripts/build-pgo.ps1 to produce one (see scripts/pgo-profiles/README.md)."
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

# maize-439: a build directory configured before the toolchain move can never pick up
# cmake/ToolchainRoot.cmake, because CMake only re-includes a toolchain file in a
# directory that was FIRST configured with one, so CMakeLists.txt refuses it. The
# refusal is right and its message explains the one-time repair, but this script OWNS
# build/$Preset end to end, so making the operator perform that repair by hand on their
# primary install path is asking them to fix something we can just fix. Retry once from
# scratch, mirroring the PGO retry immediately below.
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
        & $Cmake --preset $Preset @displayArgs @pgoArgs
    }
}

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

# maize-454: the installed set is v2's machine and assembler. -WithCToolchain adds the
# v1 C pipeline, and the extra targets below are mzcc's PRECONDITION LIST, not the set
# of tools a compile happens to spawn. mzcc.c resolve_toolchain checks five tools at
# startup, before it does any work and whatever the subcommand: toolchain/cproc/cproc-qbe
# and toolchain/qbe/obj/qbe (built by the cross-toolchain step further down), then
# build/<preset>/mazm, build/<preset>/maize and build/<preset>/mzld, each a hard exit 2
# when missing. maize is in that list even though only `mzcc -r` executes it, so leaving
# it out makes EVERY mzcc invocation fail. All three build/ tools are built here and
# none is installed: mzcc resolves them by path out of the build directory, so they need
# to exist there, not on PATH. D-1 is about PATH and is untouched by this.
$InstallTools = @('mzvm', 'mzvmg', 'mzasm')
$BuildTargets = $InstallTools
if ($WithCToolchain) {
    $BuildTargets = $InstallTools + @('mzcc', 'mazm', 'maize', 'mzld')
}
$TargetList = $BuildTargets -join ', '

Write-Host "Building $TargetList ($Preset)..."
& $Cmake --build $BuildDir --target @BuildTargets
if ($LASTEXITCODE -ne 0) {
    if ($pgoArgs[0] -eq '-DMAIZE_PGO=use') {
        Write-Warning "cmake build failed for preset '$Preset' with Clang PGO active (exit $LASTEXITCODE); the committed profile at $PgoProfile may be incompatible with the current toolchain pin. Reconfiguring and retrying once without PGO..."
        $pgoArgs = @('-DMAIZE_PGO=')
        & $Cmake --preset $Preset @displayArgs @pgoArgs
        if ($LASTEXITCODE -ne 0) {
            Write-Error "cmake reconfigure without PGO failed (exit $LASTEXITCODE)." -ErrorAction Continue
            exit 2
        }
        & $Cmake --build $BuildDir --target @BuildTargets
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
# maize-418: mzvm is the console-subsystem Maize v2 machine (terminal I/O); mzvmg is the
# graphical one (SDL window). maize-422 (D-1): mzasm is the v2 assembler.
# maize-454: the frozen v1 machine (maize, maizeg, mazm) is no longer installed, and neither
# are mzld, mzdis and mzcc, whose v2 ports have not landed yet.
$CopyTools = $InstallTools
if ($WithCToolchain) {
    # mzcc is the C pipeline's entry point, so it travels with the toolchain rather than
    # with the v2 machine. mazm, maize and mzld stay in the build directory, unexported.
    $CopyTools = $InstallTools + @('mzcc')
}
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

# mzvmg.exe (graphical) links SDL2 dynamically; install the runtime DLL alongside it so it
# starts from anywhere on PATH ($InstallDir is on PATH, so a co-located DLL resolves).
if ($displayOn) {
    if (Test-Path $Sdl2Dll) {
        Copy-Item $Sdl2Dll (Join-Path $InstallDir 'SDL2.dll') -Force
        # Stamp the copied DLL to now for the same reason as the tool exes above (maize-366).
        (Get-Item (Join-Path $InstallDir 'SDL2.dll')).LastWriteTime = Get-Date
        Write-Host "Installed $Sdl2Dll -> $(Join-Path $InstallDir 'SDL2.dll')"
    }
    else {
        Write-Warning "MAIZE_DISPLAY is ON but $Sdl2Dll is missing; mzvmg.exe will fail to start until SDL2.dll is on PATH."
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

# --- v1 C pipeline (opt-in, -WithCToolchain) --------------------------------------
# maize-454: everything below is the v1 C pipeline. It is off by default because it is
# a cache hit most of the time and a real wait when it is not, and Ctrl+Shift+B now runs
# this script on every press.
if ($WithCToolchain) {

# --- Resolve Git Bash (maize-257): the native mzcc forwarder and the C cross- -----
# toolchain build below both need bash.exe, not WSL. Resolve-GitBash lives in
# scripts/lib/gitbash.ps1 (maize-258 Decision 3), the sole definition site shared by
# install-mzasm.ps1, build-quesos.ps1, build-userland.ps1, and build-demos.ps1.
. (Join-Path $ScriptDir 'lib\gitbash.ps1')
$BashExe = Resolve-GitBash

# --- Windows forwarder: refresh <InstallDir>\mzcc.cmd from the repo template --------
# mzcc.cmd is the Windows entry point for the C toolchain. maize-257: it now runs
# scripts/cc-maize.sh (the single canonical C driver) NATIVELY via Git Bash, with no
# WSL involved; the toolchain (cproc-qbe/qbe) it depends on is built the same way,
# below. Rewriting it here on every build keeps it build-managed alongside the four
# .exe surfaces and structurally unable to go stale (maize-96 OQ2). Renamed from
# maize-cc to fit the mz* tool family (mzld, mzdis); the legacy name is removed below
# so two names cannot drift. $BashExe and $Preset are baked into the generated
# forwarder (absolute path + this install's preset), mirroring how
# scripts/refresh-c-toolchain.sh bakes REPO_ROOT into the old WSL-side ~/bin/mzcc.
if (-not $BashExe) {
    Write-Warning 'Git Bash (bash.exe) not found; skipping the mzcc.cmd forwarder refresh. Install Git for Windows (ships Git Bash) and re-run to install mzcc.'
}
else {
    $mzccCmd = @"
@echo off
rem mzcc: compile a C source through the Maize C toolchain (gcc-like CLI).
rem GENERATED by scripts/install-mzasm.ps1 (maize-257) on every install: runs
rem natively via Git Bash + the vendored llvm-mingw toolchain, no WSL involved.
rem   mzcc <file.c>          compile+link to <file>.mzx beside the source (no run)
rem   mzcc <file.c> -r       compile and run, propagating the guest exit code
rem   mzcc <file.c> --emit   also leave <file>.mazm (qbe body) beside the source
rem   mzcc --build           rebuild the cproc/qbe toolchain
setlocal
if "%~1"=="" (
  echo usage: mzcc ^<file.c^> [-r ^| --emit]   ^(also: mzcc --build^)
  exit /b 2
)
"$BashExe" "$("$RepoRoot" -replace '\\','/')/scripts/cc-maize.sh" --preset $Preset %*
exit /b %errorlevel%
"@
    $cmdPath = Join-Path $InstallDir 'mzcc.cmd'
    Set-Content -Path $cmdPath -Value $mzccCmd -Encoding Ascii
    Write-Host "Refreshed $cmdPath (native Git Bash forwarder, preset $Preset)."
}

# Retire the pre-rename forwarder so a stale maize-cc.cmd can't shadow or drift.
$legacyCmd = Join-Path $InstallDir 'maize-cc.cmd'
if (Test-Path $legacyCmd) {
    Remove-Item $legacyCmd -Force
    Write-Host "Removed legacy $legacyCmd (renamed to mzcc.cmd)."
}

# --- C cross-toolchain build (cproc-qbe + qbe, native via Git Bash) ---------------
# maize-257: scripts/build-toolchain.sh now builds cproc-qbe.exe + qbe.exe natively
# on Windows (Git Bash + the vendored llvm-mingw clang; no WSL, no MSYS2, no
# driver.c). Non-fatal: the v2 tools above are already installed and smoke-checked, so
# a missing Git Bash or a toolchain hiccup only warns; mzcc then falls back to whatever
# toolchain build already exists.
# Build output goes to stderr; under Windows PowerShell 5.1 with
# ErrorActionPreference=Stop that would become a terminating NativeCommandError, so
# relax it for exactly this call (pwsh 7 is unaffected).
if (-not $BashExe) {
    Write-Warning 'Git Bash (bash.exe) not found; skipping C cross-toolchain (cproc/qbe) build. mzcc will use any previously built toolchain.'
}
else {
    Write-Host 'Building C cross-toolchain (cproc-qbe + qbe, native Windows) via Git Bash...'
    $repoPosix = ("$RepoRoot" -replace '\\', '/')
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    & $BashExe -lc "'$repoPosix/scripts/build-toolchain.sh'"
    $tcExit = $LASTEXITCODE
    $ErrorActionPreference = $prevEap
    if ($tcExit -ne 0) {
        Write-Warning "C cross-toolchain build failed (exit $tcExit). Native tools are installed; retry with 'bash scripts/build-toolchain.sh'."
    }
    else {
        Write-Host 'C cross-toolchain built (cproc-qbe + qbe + Maize target, native Windows).'
    }
}

} # end -WithCToolchain

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
