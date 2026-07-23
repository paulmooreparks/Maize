#Requires -Version 5.1
<#
.SYNOPSIS
    Two-phase Clang PGO (Profile-Guided Optimization) build for the maize VM
    (maize-259): instrument, train, merge, then rebuild optimized.

.DESCRIPTION
    Drives CMakeLists.txt's MAIZE_PGO option end to end:

      1. Configure + build `maize` with -DMAIZE_PGO=generate (instrumented; the
         binary writes raw profile counters into MAIZE_PGO_DIR on exit).
      2. Run the training workload against that instrumented binary -Runs times
         (default 5), so the counters cover a representative interpreter workload
         rather than one noisy sample.
      3. Merge the raw profiles into a single default.profdata via llvm-profdata.
      4. Reconfigure + rebuild maize AND maizeg with -DMAIZE_PGO=use against that
         merged profile: this is the shipped, profile-guided binary.

    The training workload is the headless DOOM benchmark (demos/doom/doom_bench.c):
    it is not vendored as prebuilt bytecode (cproc/qbe, which compile it, are
    POSIX-only), so pass -BenchImage/-Wad pointing at your own build of it. See
    demos/doom/README.md, "Build command", for how to produce doom_bench.mzx
    (build under WSL/Linux/macOS; the resulting bytecode is portable and this
    script runs it on native Windows).

    On success this overwrites scripts/pgo-profiles/<Preset>/default.profdata, the
    profile install-mazm.ps1 ships by default. Review the diff and commit it
    deliberately; this script does not commit for you.

.PARAMETER Preset
    CMake preset to profile. Defaults to windows-llvm-mingw-release.

.PARAMETER BenchImage
    Path to the training workload's linked Maize image (e.g. doom_bench.mzx).
    Required.

.PARAMETER Wad
    Path to the WAD the training workload's -iwad expects (e.g. min.wad).
    Required.

.PARAMETER Runs
    How many back-to-back training runs to merge. Default 5.

.EXAMPLE
    scripts/build-pgo.ps1 -BenchImage C:\scratch\doom_bench.mzx -Wad C:\scratch\min.wad
#>
[CmdletBinding()]
param(
    [string]$Preset = 'windows-llvm-mingw-release',
    [Parameter(Mandatory)][string]$BenchImage,
    [Parameter(Mandatory)][string]$Wad,
    [int]$Runs = 5
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot  = Resolve-Path (Join-Path $ScriptDir '..')
$BuildDir  = Join-Path $RepoRoot "build/$Preset"
$PgoDir    = Join-Path $BuildDir 'pgo-data'
$ShipDir   = Join-Path $RepoRoot "scripts/pgo-profiles/$Preset"

if (-not (Test-Path $BenchImage)) { Write-Error "BenchImage not found: $BenchImage" -ErrorAction Continue; exit 2 }
if (-not (Test-Path $Wad))        { Write-Error "Wad not found: $Wad" -ErrorAction Continue; exit 2 }

# --- Resolve cmake the same way install-mazm.ps1 / run-tests.ps1 do ---------------
$cmakeCmd = Get-Command cmake -ErrorAction SilentlyContinue
if ($cmakeCmd) { $Cmake = $cmakeCmd.Source }
elseif (Test-Path 'C:\Program Files\CMake\bin\cmake.exe') { $Cmake = 'C:\Program Files\CMake\bin\cmake.exe' }
else { Write-Error 'cmake not found on PATH or at C:\Program Files\CMake\bin\cmake.exe.' -ErrorAction Continue; exit 2 }

$ToolchainDir = Join-Path $RepoRoot '.toolchains/llvm-mingw'
$LlvmProfdata = Join-Path $ToolchainDir 'bin/llvm-profdata.exe'
if (-not (Test-Path $LlvmProfdata)) {
    Write-Error "llvm-profdata not found at $LlvmProfdata; run scripts/bootstrap-toolchain.ps1 first." -ErrorAction Continue
    exit 2
}

# cmake cache vars land on a compiler command line verbatim (MAIZE_PGO_DIR feeds
# clang's -fprofile-generate=/-fprofile-use=); normalize to forward slashes the
# same way install-mazm.ps1's SDL2_DIR/MAIZE_PGO_DIR args do, so a native
# backslash path is never handed to clang on the command line.
$PgoDirArg = ($PgoDir -replace '\\', '/')

# --- Phase 1: instrument -----------------------------------------------------------
Write-Host "== Phase 1/4: configuring '$Preset' with MAIZE_PGO=generate =="
if (Test-Path $PgoDir) { Remove-Item -Recurse -Force $PgoDir }
& $Cmake --preset $Preset "-DMAIZE_PGO=generate" "-DMAIZE_PGO_DIR=$PgoDirArg"
if ($LASTEXITCODE -ne 0) { Write-Error "configure (generate) failed (exit $LASTEXITCODE)." -ErrorAction Continue; exit 2 }

Write-Host "== Phase 1/4: building instrumented maize =="
& $Cmake --build $BuildDir --target maize
if ($LASTEXITCODE -ne 0) { Write-Error "build (generate) failed (exit $LASTEXITCODE)." -ErrorAction Continue; exit 2 }

# --- Phase 2: train ------------------------------------------------------------------
Write-Host "== Phase 2/4: training ($Runs runs of $BenchImage) =="
$maizeExe = Join-Path $BuildDir 'maize.exe'
$wadDir   = Split-Path -Parent $Wad
$wadName  = Split-Path -Leaf $Wad
for ($i = 1; $i -le $Runs; $i++) {
    Write-Host "  run $i/$Runs"
    # maize-360: --bare runs the benchmark image directly (no quesOS ROM). PGO training
    # profiles the raw interpreter path; a plain `maize <image>` would boot the default
    # quesOS ROM (or fail "no boot ROM found"), so --bare preserves the pre-360 run.
    & $maizeExe --bare --mount "$($wadDir)=/ro:ro" $BenchImage -iwad "/ro/$wadName" -warp 1 1 -nomonsters | Select-Object -Last 1
    if ($LASTEXITCODE -ne 0) { Write-Error "training run $i failed (exit $LASTEXITCODE)." -ErrorAction Continue; exit 2 }
}

$rawProfiles = @(Get-ChildItem -Path $PgoDir -Filter '*.profraw' -ErrorAction SilentlyContinue)
if ($rawProfiles.Count -eq 0) { Write-Error "no .profraw files produced under $PgoDir; instrumented maize.exe did not run cleanly." -ErrorAction Continue; exit 2 }

# --- Phase 3: merge ------------------------------------------------------------------
Write-Host "== Phase 3/4: merging $($rawProfiles.Count) raw profile(s) =="
$merged = Join-Path $PgoDir 'default.profdata'
& $LlvmProfdata merge "-output=$merged" (Join-Path $PgoDir '*.profraw')
if ($LASTEXITCODE -ne 0 -or -not (Test-Path $merged)) { Write-Error "llvm-profdata merge failed (exit $LASTEXITCODE)." -ErrorAction Continue; exit 2 }

# --- Phase 4: rebuild optimized ------------------------------------------------------
Write-Host "== Phase 4/4: configuring '$Preset' with MAIZE_PGO=use =="
& $Cmake --preset $Preset "-DMAIZE_PGO=use" "-DMAIZE_PGO_DIR=$PgoDirArg"
if ($LASTEXITCODE -ne 0) { Write-Error "configure (use) failed (exit $LASTEXITCODE)." -ErrorAction Continue; exit 2 }

Write-Host "== Phase 4/4: building profile-guided maize + maizeg =="
& $Cmake --build $BuildDir --target maize maizeg
if ($LASTEXITCODE -ne 0) { Write-Error "build (use) failed (exit $LASTEXITCODE)." -ErrorAction Continue; exit 2 }

# --- Ship the merged profile ---------------------------------------------------------
New-Item -ItemType Directory -Force $ShipDir | Out-Null
Copy-Item $merged (Join-Path $ShipDir 'default.profdata') -Force
Write-Host ""
Write-Host "Profile-guided $Preset built: $BuildDir\maize.exe, $BuildDir\maizeg.exe"
Write-Host "Merged profile copied to $ShipDir\default.profdata"
Write-Host "Review and commit that file (scripts/pgo-profiles/README.md has the provenance note to update)."
exit 0
