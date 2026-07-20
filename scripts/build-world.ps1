#Requires -Version 5.1

<#
.SYNOPSIS
    Build the whole Maize world (native tools, C toolchain, quesOS, userland, demos)
    with one command.

.DESCRIPTION
    Composes the existing per-piece build scripts, in order: submodule init, native
    binaries + C toolchain (install-mazm.ps1), quesOS (build-quesos.ps1), the wave-1
    userland (build-userland.ps1), and the demos (build-demos.ps1). No build logic is
    duplicated here; this script only sequences the existing entry points, checks each
    one's exit code, and prints a stage banner plus a final artifact/timing summary.

    One preset is pinned end to end and passed explicitly to every composed call, so a
    bare invocation is always internally coherent: every stage resolves tools from the
    SAME build directory install-mazm.ps1 just populated, even though the composed
    scripts' own standalone defaults differ by platform.

    This is the documented "I pulled, now what" answer; run it after a fresh clone or
    pull to build everything in one call.

.PARAMETER Preset
    CMake preset to build, pinned across every composed stage. Defaults to
    windows-llvm-mingw-release.

.PARAMETER InstallDir
    Where the native tools (maize, maizeg, mazm, mzld, mzdis) and SDL2.dll install.
    Defaults to $HOME\bin.

.PARAMETER UserlandOut
    Directory to stage the wave-1 userland images into. Empty ('') uses
    build-userland.ps1's own default (%USERPROFILE%\.maize\root\bin).

.PARAMETER DemosOut
    Directory to stage the demo images into. Empty ('') uses build-demos.ps1's own
    default (%USERPROFILE%\.maize\root\bin).

.PARAMETER QuesosOut
    Where to write the linked quesOS image. Empty ('') uses build-quesos.ps1's own
    default (os\quesos\quesos.mzx).

.PARAMETER Headless
    Passthrough to install-mazm.ps1: build maizeg without the --display window backend.

.PARAMETER NoPgo
    Passthrough to install-mazm.ps1: build without Clang PGO even when a committed
    profile exists for this preset.

.EXAMPLE
    .\scripts\build-world.ps1
    Builds everything (native tools, C toolchain, quesOS, userland, demos), all under
    windows-llvm-mingw-release.
#>
[CmdletBinding()]
param(
    [string]$Preset = 'windows-llvm-mingw-release',
    [string]$InstallDir = (Join-Path $HOME 'bin'),
    [string]$UserlandOut = '',
    [string]$DemosOut = '',
    [string]$QuesosOut = '',
    [switch]$Headless,
    [switch]$NoPgo
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$ScriptDir = $PSScriptRoot
$RepoRoot = Split-Path -Parent $ScriptDir

$Stopwatch = [System.Diagnostics.Stopwatch]::StartNew()

# INTERIM DEPENDENCY NOTE (maize-266 tracks the end state): stages 2-5 below call
# into install-mazm.ps1 / build-quesos.ps1 / build-userland.ps1 / build-demos.ps1,
# each of which resolves Git Bash (Resolve-GitBash, scripts/lib/gitbash.ps1) to run
# the POSIX-only cproc/QBE C toolchain. Git Bash ships with Git for Windows itself,
# so this adds no dependency beyond Git plus what the repo vendors (no WSL, no
# MSYS2, nothing else on PATH), but calling out to bash at all is a tolerated
# interim shape, not the target: maize-266 designs that dependency out.

# Resolve the effective output paths up front, matching each composed script's own
# default resolution, so the closing summary can list them even when the caller left
# a param at '' (its composed script's own default).
if ($QuesosOut -ne '') { $EffectiveQuesosOut = $QuesosOut }
else { $EffectiveQuesosOut = Join-Path $RepoRoot 'os\quesos\quesos.mzx' }
if ($UserlandOut -ne '') { $EffectiveUserlandOut = $UserlandOut }
else { $EffectiveUserlandOut = Join-Path $HOME '.maize\root\bin' }
if ($DemosOut -ne '') { $EffectiveDemosOut = $DemosOut }
else { $EffectiveDemosOut = Join-Path $HOME '.maize\root\bin' }

# --- Stage 1/5: submodule init -------------------------------------------------------
# A "clean pull" has no submodules checked out; build-demos.ps1 (doom) otherwise
# die()s mid-run on the missing submodule, a confusing place to first discover it on
# a one-command promise (Decision 4).
Write-Host '=== [1/5] git submodule init ==='
$prevEap = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
& git -C $RepoRoot submodule update --init --recursive
$stageCode = $LASTEXITCODE
$ErrorActionPreference = $prevEap
if ($stageCode -ne 0) {
    Write-Error "build-world.ps1: stage [1/5] 'git submodule init' failed (exit $stageCode). Fix the submodule error above, then re-run scripts\build-world.ps1; no later stage ran." -ErrorAction Continue
    exit $stageCode
}

# --- Stage 2/5: native binaries + C toolchain ----------------------------------------
# Hashtable splatting (@{...}), not array splatting: a flat array of alternating
# '-Name'/value strings is NOT re-scanned for leading-dash parameter names by
# PowerShell's binder (confirmed identical on Windows PowerShell 5.1 and PowerShell
# 7; only a hashtable splat binds by name). An earlier array-splat draft of this
# script mis-bound '-InstallDir' positionally and failed with
# "A positional parameter cannot be found that accepts argument '-InstallDir'".
Write-Host '=== [2/5] native binaries + C toolchain (install-mazm.ps1) ==='
$installArgs = @{ Preset = $Preset; InstallDir = $InstallDir }
if ($Headless) { $installArgs['Headless'] = $true }
if ($NoPgo) { $installArgs['NoPgo'] = $true }
& (Join-Path $ScriptDir 'install-mazm.ps1') @installArgs
if ($LASTEXITCODE -ne 0) {
    Write-Error "build-world.ps1: stage [2/5] 'native binaries + C toolchain' failed (exit $LASTEXITCODE). See install-mazm.ps1's own error above; no later stage ran." -ErrorAction Continue
    exit $LASTEXITCODE
}

# --- Stage 3/5: quesOS ----------------------------------------------------------------
Write-Host '=== [3/5] quesOS (build-quesos.ps1) ==='
$quesosArgs = @{ Preset = $Preset }
if ($QuesosOut -ne '') { $quesosArgs['Out'] = $QuesosOut }
& (Join-Path $ScriptDir 'build-quesos.ps1') @quesosArgs
if ($LASTEXITCODE -ne 0) {
    Write-Error "build-world.ps1: stage [3/5] 'quesOS' failed (exit $LASTEXITCODE). See build-quesos.ps1's own error above; no later stage ran." -ErrorAction Continue
    exit $LASTEXITCODE
}

# --- Stage 4/5: wave-1 userland --------------------------------------------------------
Write-Host '=== [4/5] wave-1 userland (build-userland.ps1) ==='
$userlandArgs = @{ Preset = $Preset }
if ($UserlandOut -ne '') { $userlandArgs['Out'] = $UserlandOut }
& (Join-Path $ScriptDir 'build-userland.ps1') @userlandArgs
if ($LASTEXITCODE -ne 0) {
    Write-Error "build-world.ps1: stage [4/5] 'wave-1 userland' failed (exit $LASTEXITCODE). See build-userland.ps1's own error above; no later stage ran." -ErrorAction Continue
    exit $LASTEXITCODE
}

# --- Stage 5/5: demos -------------------------------------------------------------------
Write-Host '=== [5/5] demos (build-demos.ps1) ==='
$demosArgs = @{ Preset = $Preset }
if ($DemosOut -ne '') { $demosArgs['Out'] = $DemosOut }
& (Join-Path $ScriptDir 'build-demos.ps1') @demosArgs
if ($LASTEXITCODE -ne 0) {
    Write-Error "build-world.ps1: stage [5/5] 'demos' failed (exit $LASTEXITCODE). See build-demos.ps1's own error above; no later stage ran." -ErrorAction Continue
    exit $LASTEXITCODE
}

# --- Success: artifact + timing summary ------------------------------------------------
$Stopwatch.Stop()
Write-Host ''
Write-Host '=== build-world.ps1: all stages complete ==='

Write-Host 'Native tools + SDL2 runtime:'
foreach ($tool in 'maize', 'maizeg', 'mazm', 'mzld', 'mzdis') {
    $exePath = Join-Path $InstallDir "$tool.exe"
    if (Test-Path $exePath) { Write-Host "  $exePath" }
}
$sdl2Path = Join-Path $InstallDir 'SDL2.dll'
if (Test-Path $sdl2Path) { Write-Host "  $sdl2Path" }

Write-Host 'C cross-toolchain:'
$tcCandidates = @(
    (Join-Path $RepoRoot 'toolchain\qbe\obj\qbe.exe'),
    (Join-Path $RepoRoot 'toolchain\qbe\obj\qbe'),
    (Join-Path $RepoRoot 'toolchain\cproc\cproc-qbe.exe'),
    (Join-Path $RepoRoot 'toolchain\cproc\cproc-qbe'),
    (Join-Path $RepoRoot 'toolchain\cproc\cproc.exe'),
    (Join-Path $RepoRoot 'toolchain\cproc\cproc')
)
foreach ($tcExe in $tcCandidates) {
    if (Test-Path $tcExe) { Write-Host "  $tcExe" }
}

Write-Host 'quesOS image:'
if (Test-Path $EffectiveQuesosOut) { Write-Host "  $EffectiveQuesosOut" }

Write-Host "Userland ($EffectiveUserlandOut):"
if (Test-Path $EffectiveUserlandOut) {
    Get-ChildItem -Path $EffectiveUserlandOut -File | ForEach-Object { Write-Host "  $($_.FullName)" }
}

Write-Host "Demos ($EffectiveDemosOut):"
if (Test-Path $EffectiveDemosOut) {
    Get-ChildItem -Path $EffectiveDemosOut -File | ForEach-Object { Write-Host "  $($_.FullName)" }
}

$elapsed = $Stopwatch.Elapsed
Write-Host ("Total elapsed: {0:hh\:mm\:ss}" -f $elapsed)
exit 0
