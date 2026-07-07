#Requires -Version 5.1
<#
.SYNOPSIS
    Build both Maize binaries and run the in-scope asm/ test suite (Windows).

.DESCRIPTION
    Configures and builds the windows-llvm-mingw-debug preset (or an override given
    via -Preset), then assembles and runs each of the 16 in-scope tests under asm/,
    comparing captured stdout against the expected output embedded below. Prints a
    per-test PASS/FAIL report plus a summary line.

    This is the one-command entry point: configure, build, assemble, run, diff,
    report. Never prompts for input, so it is safe for non-interactive CI use.

    Exit codes:
      0 - all tests passed
      1 - one or more tests failed
      2 - environment/setup failure (missing cmake/ninja, configure/build failure,
          missing built executables) that prevented tests from running at all

.PARAMETER Preset
    CMake preset to use. Defaults to windows-llvm-mingw-debug.

.PARAMETER SkipBuild
    Skip the configure+build step and run against whatever is already built.
#>
[CmdletBinding()]
param(
    [string]$Preset = 'windows-llvm-mingw-debug',
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# --- Paths resolved relative to THIS script, not the caller's CWD ----------------
$ScriptDir  = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot   = Resolve-Path (Join-Path $ScriptDir '..')
$AsmDir     = Join-Path $RepoRoot 'asm'
$BuildDir   = Join-Path $RepoRoot "build/$Preset"
$TestRunDir = Join-Path $BuildDir 'test-run'

# --- In-scope test corpus. Expected values already have trailing newlines
#     stripped, since the comparison rule strips trailing newlines from both
#     sides before comparing (src/maize.cpp appends an extra trailing newline
#     on Linux only; this keeps the same expected table valid on both platforms).
$Tests = @(
    [pscustomobject]@{ Name = 'hello';            File = 'hello.asm';             Expected = 'Hello, world!';                 Golden = $true }
    [pscustomobject]@{ Name = 'test_mul';          File = 'test_mul.asm';          Expected = 'MUL test: PASS (1/2/4/8-byte)'; Golden = $false }
    [pscustomobject]@{ Name = 'test_flags_arith';  File = 'test_flags_arith.asm';  Expected = 'flags arith: PASS';             Golden = $false }
    [pscustomobject]@{ Name = 'test_flags_branch'; File = 'test_flags_branch.asm'; Expected = 'flags branch: PASS';           Golden = $false }
    [pscustomobject]@{ Name = 'test_flags_shl';    File = 'test_flags_shl.asm';    Expected = 'flags shl: PASS';               Golden = $false }
    [pscustomobject]@{ Name = 'test_flags_shr';    File = 'test_flags_shr.asm';    Expected = 'flags shr: PASS';               Golden = $false }
    [pscustomobject]@{ Name = 'test_flags_mul8';   File = 'test_flags_mul8.asm';   Expected = 'flags mul8: PASS';              Golden = $false }
    [pscustomobject]@{ Name = 'test_flags_move';   File = 'test_flags_move.asm';   Expected = 'flags move: PASS';              Golden = $false }
    [pscustomobject]@{ Name = 'test_addr64';       File = 'test_addr64.asm';       Expected = 'addr64: PASS';                  Golden = $false }
    [pscustomobject]@{ Name = 'test_cmptest';       File = 'test_cmptest.asm';       Expected = 'cmptest: PASS';                 Golden = $false }
    [pscustomobject]@{ Name = 'test_ldimm';         File = 'test_ldimm.asm';         Expected = 'ld imm: PASS';                  Golden = $false }
    [pscustomobject]@{ Name = 'test_stack64';       File = 'test_stack64.asm';       Expected = 'stack64: PASS';                 Golden = $false }
    [pscustomobject]@{ Name = 'test_div';           File = 'test_div.asm';           Expected = 'div: PASS';                     Golden = $false }
    [pscustomobject]@{ Name = 'test_jcc';           File = 'test_jcc.asm';           Expected = 'jcc: PASS';                     Golden = $false }
    [pscustomobject]@{ Name = 'test_memblock';      File = 'test_memblock.asm';      Expected = 'memblock: PASS';                Golden = $false }
    [pscustomobject]@{ Name = 'test_crossblock';    File = 'test_crossblock.asm';    Expected = 'crossblk: PASS';                Golden = $false }
)

function Trim-TrailingNewlines([string]$s) {
    if ($null -eq $s) { return '' }
    return [regex]::Replace($s, '(\r\n|\r|\n)+$', '')
}

# Setup-failure diagnostics (missing tools, failed configure/build, missing
# corpus files) go to stderr, matching run-tests.sh, so both scripts send
# environment/setup errors to the same stream. The normal per-test PASS/FAIL
# report and summary line stay on stdout via Write-Host.
function Write-SetupError([string]$msg) {
    [Console]::Error.WriteLine($msg)
}

function Resolve-Cmake {
    $cmd = Get-Command cmake -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $fallback = 'C:\Program Files\CMake\bin\cmake.exe'
    if (Test-Path $fallback) { return $fallback }
    return $null
}

function Resolve-Ninja {
    $cmd = Get-Command ninja -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    return $null
}

# --- Resolve build tools before attempting a configure ---------------------------
$CmakeExe = Resolve-Cmake
if (-not $CmakeExe) {
    Write-SetupError "cmake not found on PATH or at C:\Program Files\CMake\bin\cmake.exe."
    Write-SetupError "Install it: winget install Kitware.CMake"
    exit 2
}

$NinjaExe = Resolve-Ninja
if (-not $NinjaExe) {
    Write-SetupError "ninja not found on PATH."
    Write-SetupError "Install it: winget install Ninja-build.Ninja"
    exit 2
}

# --- Configure + build ------------------------------------------------------------
if (-not $SkipBuild) {
    Push-Location $RepoRoot
    try {
        & $CmakeExe --preset $Preset
        $configureExit = $LASTEXITCODE
        if ($configureExit -eq 0) {
            & $CmakeExe --build --preset $Preset
            $buildExit = $LASTEXITCODE
        } else {
            $buildExit = 0
        }
    }
    finally {
        Pop-Location
    }

    if ($configureExit -ne 0) {
        Write-SetupError "cmake configure failed for preset '$Preset' (exit $configureExit)."
        exit 2
    }
    if ($buildExit -ne 0) {
        Write-SetupError "cmake build failed for preset '$Preset' (exit $buildExit)."
        exit 2
    }
}

# --- Locate the built executables --------------------------------------------------
$MaizeExe = Join-Path $BuildDir 'maize.exe'
$MazmExe  = Join-Path $BuildDir 'mazm.exe'

if (-not (Test-Path $MazmExe)) {
    Write-SetupError "Expected built executable not found: $MazmExe"
    exit 2
}
if (-not (Test-Path $MaizeExe)) {
    Write-SetupError "Expected built executable not found: $MaizeExe"
    exit 2
}

if (-not (Test-Path $TestRunDir)) {
    New-Item -ItemType Directory -Path $TestRunDir -Force | Out-Null
}

# --- Per-test execution -------------------------------------------------------------
function Invoke-Test($test) {
    if ($test.Golden) {
        $asmPath = Join-Path $AsmDir $test.File
    } else {
        $srcPath = Join-Path $AsmDir $test.File
        if (-not (Test-Path $srcPath)) {
            Write-SetupError "Missing test source file: $srcPath"
            Write-SetupError "Setup failure: declared test '$($test.Name)' has no corresponding .asm file."
            exit 2
        }
        $asmPath = Join-Path $TestRunDir $test.File
        Copy-Item -Path $srcPath -Destination $asmPath -Force
    }
    $binPath = [System.IO.Path]::ChangeExtension($asmPath, 'bin')

    $mazmLog = [System.IO.Path]::GetTempFileName()
    & $MazmExe $asmPath *> $mazmLog
    $mazmExit = $LASTEXITCODE

    if ($mazmExit -ne 0 -or -not (Test-Path $binPath)) {
        $mazmOutput = Get-Content -Raw -Path $mazmLog -ErrorAction SilentlyContinue
        Remove-Item -Force $mazmLog -ErrorAction SilentlyContinue
        return [pscustomobject]@{
            Name     = $test.Name
            Pass     = $false
            Expected = $test.Expected
            Actual   = Trim-TrailingNewlines $mazmOutput
        }
    }
    Remove-Item -Force $mazmLog -ErrorAction SilentlyContinue

    # Redirect to a temp file and read back rather than relying on naive
    # array-of-lines capture (& $exe args), which can normalize line endings.
    $stdoutFile = [System.IO.Path]::GetTempFileName()
    $stderrFile = [System.IO.Path]::GetTempFileName()
    & $MaizeExe $binPath > $stdoutFile 2> $stderrFile
    $maizeExit = $LASTEXITCODE
    $actualRaw = Get-Content -Raw -Path $stdoutFile -ErrorAction SilentlyContinue
    Remove-Item -Force $stdoutFile, $stderrFile -ErrorAction SilentlyContinue

    $actual = Trim-TrailingNewlines $actualRaw
    $pass = ($maizeExit -eq 0) -and ($actual -eq $test.Expected)

    return [pscustomobject]@{
        Name     = $test.Name
        Pass     = $pass
        Expected = $test.Expected
        Actual   = $actual
    }
}

$results = @()
foreach ($t in $Tests) {
    $results += Invoke-Test $t
}

$failCount = 0
foreach ($r in $results) {
    if ($r.Pass) {
        Write-Host "[PASS] $($r.Name)"
    } else {
        $failCount++
        Write-Host "[FAIL] $($r.Name)"
        Write-Host "        expected: `"$($r.Expected)`""
        Write-Host "        actual:   `"$($r.Actual)`""
    }
}

$passCount = $results.Count - $failCount
Write-Host ""
Write-Host "$passCount passed, $failCount failed ($($results.Count) total)"

if ($failCount -gt 0) {
    exit 1
}
exit 0
