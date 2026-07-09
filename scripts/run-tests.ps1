#Requires -Version 5.1
<#
.SYNOPSIS
    Build both Maize binaries and run the in-scope asm/ test suite (Windows).

.DESCRIPTION
    Configures and builds the windows-llvm-mingw-debug preset (or an override given
    via -Preset), then assembles and runs each of the 21 in-scope tests under asm/,
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
    [pscustomobject]@{ Name = 'hello';            File = 'hello.mazm';             Expected = 'Hello, world!';                 Golden = $true }
    [pscustomobject]@{ Name = 'test_mul';          File = 'test_mul.mazm';          Expected = 'MUL test: PASS (1/2/4/8-byte)'; Golden = $false }
    [pscustomobject]@{ Name = 'test_mulw';         File = 'test_mulw.mazm';         Expected = 'MULW test: PASS';                Golden = $false }
    [pscustomobject]@{ Name = 'test_flags_arith';  File = 'test_flags_arith.mazm';  Expected = 'flags arith: PASS';             Golden = $false }
    [pscustomobject]@{ Name = 'test_flags_branch'; File = 'test_flags_branch.mazm'; Expected = 'flags branch: PASS';           Golden = $false }
    [pscustomobject]@{ Name = 'test_flags_shl';    File = 'test_flags_shl.mazm';    Expected = 'flags shl: PASS';               Golden = $false }
    [pscustomobject]@{ Name = 'test_flags_shr';    File = 'test_flags_shr.mazm';    Expected = 'flags shr: PASS';               Golden = $false }
    [pscustomobject]@{ Name = 'test_flags_sar';    File = 'test_flags_sar.mazm';    Expected = 'flags sar: PASS';               Golden = $false }
    [pscustomobject]@{ Name = 'test_flags_mul8';   File = 'test_flags_mul8.mazm';   Expected = 'flags mul8: PASS';              Golden = $false }
    [pscustomobject]@{ Name = 'test_flags_move';   File = 'test_flags_move.mazm';   Expected = 'flags move: PASS';              Golden = $false }
    [pscustomobject]@{ Name = 'test_addr64';       File = 'test_addr64.mazm';       Expected = 'addr64: PASS';                  Golden = $false }
    [pscustomobject]@{ Name = 'test_cmptest';       File = 'test_cmptest.mazm';       Expected = 'cmptest: PASS';                 Golden = $false }
    [pscustomobject]@{ Name = 'test_ldimm';         File = 'test_ldimm.mazm';         Expected = 'ld imm: PASS';                  Golden = $false }
    [pscustomobject]@{ Name = 'test_stack64';       File = 'test_stack64.mazm';       Expected = 'stack64: PASS';                 Golden = $false }
    [pscustomobject]@{ Name = 'test_rsinit';        File = 'test_rsinit.mazm';        Expected = 'rsinit: PASS';                  Golden = $false }
    [pscustomobject]@{ Name = 'test_div';           File = 'test_div.mazm';           Expected = 'div: PASS';                     Golden = $false }
    [pscustomobject]@{ Name = 'test_jcc';           File = 'test_jcc.mazm';           Expected = 'jcc: PASS';                     Golden = $false }
    [pscustomobject]@{ Name = 'test_jcc_all';       File = 'test_jcc_all.mazm';       Expected = 'jcc-all: PASS';                 Golden = $false }
    [pscustomobject]@{ Name = 'test_neg';           File = 'test_neg.mazm';           Expected = 'neg: PASS';                     Golden = $false }
    [pscustomobject]@{ Name = 'test_setcc';         File = 'test_setcc.mazm';         Expected = 'setcc: PASS';                   Golden = $false }
    [pscustomobject]@{ Name = 'test_memblock';      File = 'test_memblock.mazm';      Expected = 'memblock: PASS';                Golden = $false }
    [pscustomobject]@{ Name = 'test_widecount';     File = 'test_widecount.mazm';     Expected = 'widecount: PASS';               Golden = $false }
    [pscustomobject]@{ Name = 'test_crossblock';    File = 'test_crossblock.mazm';    Expected = 'crossblk: PASS';                Golden = $false }
    [pscustomobject]@{ Name = 'test_adc';           File = 'test_adc.mazm';           Expected = 'adc: PASS';                     Golden = $false }
    [pscustomobject]@{ Name = 'test_copywidth';     File = 'test_copywidth.mazm';     Expected = 'copywidth: PASS';               Golden = $false }
    [pscustomobject]@{ Name = 'reject_ld_value';    File = 'test_reject_ldval.mazm';   Expected = 'reads from a memory address';   Golden = $false; ExpectAsmError = $true }
    [pscustomobject]@{ Name = 'reject_ldz';         File = 'test_reject_ldz.mazm';     Expected = "unknown keyword or opcode 'LDZ'"; Golden = $false; ExpectAsmError = $true }
    [pscustomobject]@{ Name = 'test_call_ind';      File = 'test_call_ind.mazm';      Expected = 'call ind: PASS';                Golden = $false }
    [pscustomobject]@{ Name = 'test_setint';        File = 'test_setint.mazm';        Expected = 'setint: PASS';                  Golden = $false }
    [pscustomobject]@{ Name = 'test_outr_in';       File = 'test_outr_in.mazm';       Expected = 'outr/in: PASS';                 Golden = $false }
    [pscustomobject]@{ Name = 'test_brk';           File = 'test_brk.mazm';           Expected = 'brk: PASS';                     Golden = $false }
    [pscustomobject]@{ Name = 'test_tstind';        File = 'test_tstind.mazm';        Expected = 'tstind: PASS';                  Golden = $false }
    [pscustomobject]@{ Name = 'reject_bad_register'; File = 'test_reject_badreg.mazm';     Expected = "unknown register 'R99'";      Golden = $false; ExpectAsmError = $true }
    [pscustomobject]@{ Name = 'reject_bad_literal';  File = 'test_reject_badliteral.mazm'; Expected = 'malformed hex literal';       Golden = $false; ExpectAsmError = $true }
    [pscustomobject]@{ Name = 'reject_include_self'; File = 'test_reject_include_self.mazm'; Expected = 'circular INCLUDE';          Golden = $false; ExpectAsmError = $true }
    [pscustomobject]@{ Name = 'reject_label_trunc';  File = 'test_reject_label_trunc.mazm'; Expected = 'unexpected end of file';     Golden = $false; ExpectAsmError = $true }
    [pscustomobject]@{ Name = 'reject_address_trunc'; File = 'test_reject_address_trunc.mazm'; Expected = 'unexpected end of file';  Golden = $false; ExpectAsmError = $true }
    [pscustomobject]@{ Name = 'reject_jcc_reg';      File = 'test_reject_jcc_reg.mazm';    Expected = 'immediate target only';       Golden = $false; ExpectAsmError = $true }
    [pscustomobject]@{ Name = 'reject_jmp_subreg';   File = 'test_reject_jmp_subreg.mazm'; Expected = 'full 64-bit width';           Golden = $false; ExpectAsmError = $true }
    [pscustomobject]@{ Name = 'nested_include';      File = 'test_nested_include.mazm';    Expected = 'nested include: PASS';        Golden = $true }
    [pscustomobject]@{ Name = 'address_fwdlabel';    File = 'test_address_fwdlabel.mazm';  Expected = 'address fwd-ref: PASS';       Golden = $false }
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
$MzldExe  = Join-Path $BuildDir 'mzld.exe'
$MzdisExe = Join-Path $BuildDir 'mzdis.exe'

if (-not (Test-Path $MazmExe)) {
    Write-SetupError "Expected built executable not found: $MazmExe"
    exit 2
}
if (-not (Test-Path $MaizeExe)) {
    Write-SetupError "Expected built executable not found: $MaizeExe"
    exit 2
}
if (-not (Test-Path $MzldExe)) {
    Write-SetupError "Expected built executable not found: $MzldExe"
    exit 2
}
if (-not (Test-Path $MzdisExe)) {
    Write-SetupError "Expected built executable not found: $MzdisExe"
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
            Write-SetupError "Setup failure: declared test '$($test.Name)' has no corresponding .mazm file."
            exit 2
        }
        $asmPath = Join-Path $TestRunDir $test.File
        Copy-Item -Path $srcPath -Destination $asmPath -Force
    }
    $binPath = [System.IO.Path]::ChangeExtension($asmPath, 'mzb')

    $mazmLog = [System.IO.Path]::GetTempFileName()
    # mazm.exe writes its rejection diagnostic to stderr on purpose for the
    # negative (ExpectAsmError) tests. Under Windows PowerShell 5.1,
    # ErrorActionPreference = Stop turns that redirected native stderr write into
    # a terminating NativeCommandError even though the *> redirect still captures
    # it fine (escalation happens on the error-record pipeline, independent of
    # where the redirected content lands); pwsh 7 is unaffected either way.
    # Relax it for exactly this invocation, then restore immediately and keep
    # relying on $LASTEXITCODE plus the captured $mazmLog content, unchanged, for
    # pass/fail (see scripts/install-mazm.ps1:90-98 for the same idiom).
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    & $MazmExe $asmPath *> $mazmLog
    $mazmExit = $LASTEXITCODE
    $ErrorActionPreference = $prevEap

    # Negative test (ExpectAsmError): the assembler MUST reject this source with a
    # diagnostic containing $test.Expected. Passes iff mazm exits nonzero and says so.
    if ($test.PSObject.Properties['ExpectAsmError'] -and $test.ExpectAsmError) {
        $mazmOutput = Get-Content -Raw -Path $mazmLog -ErrorAction SilentlyContinue
        Remove-Item -Force $mazmLog -ErrorAction SilentlyContinue
        $rejected = ($mazmExit -ne 0) -and ($null -ne $mazmOutput) -and ($mazmOutput -like "*$($test.Expected)*")
        return [pscustomobject]@{
            Name     = $test.Name
            Pass     = $rejected
            Expected = "assembler rejects with: $($test.Expected)"
            Actual   = if ($mazmExit -ne 0) { Trim-TrailingNewlines $mazmOutput } else { '(assembled successfully; expected rejection)' }
        }
    }

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
    # A program that legitimately writes to stderr (e.g. test_widecount routes its
    # large payload there so the harness can still exact-match the stdout verdict)
    # would, under Windows PowerShell 5.1 with ErrorActionPreference = Stop, turn the
    # redirected native stderr write into a terminating NativeCommandError even though
    # the 2> redirect captures it fine. Relax it for exactly this invocation, then
    # restore, and keep relying on $LASTEXITCODE plus the captured stdout (same idiom
    # as the mazm call above and scripts/install-mazm.ps1:90-98).
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    & $MaizeExe $binPath > $stdoutFile 2> $stderrFile
    $maizeExit = $LASTEXITCODE
    $ErrorActionPreference = $prevEap
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

# --- maize-12: multi-TU assemble -> link -> run -----------------------------------
# Assemble two objects with `mazm -c`, link them with mzld into one .mzx, and run
# it under maize. Also exercises two hard link-error paths.
function Emit-Object($src) {
    $srcPath = Join-Path $AsmDir $src
    $dst = Join-Path $TestRunDir $src
    Copy-Item -Path $srcPath -Destination $dst -Force
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    & $MazmExe -c $dst *> $null
    $ErrorActionPreference = $prevEap
}

function Invoke-LinkRunTest {
    Emit-Object 'link_a.mazm'
    Emit-Object 'link_b.mazm'
    $aObj = Join-Path $TestRunDir 'link_a.mzo'
    $bObj = Join-Path $TestRunDir 'link_b.mzo'
    $mzx  = Join-Path $TestRunDir 'link.mzx'

    $log = [System.IO.Path]::GetTempFileName()
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    & $MzldExe -o $mzx $aObj $bObj *> $log
    $linkExit = $LASTEXITCODE
    $ErrorActionPreference = $prevEap

    if ($linkExit -ne 0) {
        $out = Get-Content -Raw -Path $log -ErrorAction SilentlyContinue
        Remove-Item -Force $log -ErrorAction SilentlyContinue
        return [pscustomobject]@{ Name = 'link_multi_tu'; Pass = $false; Expected = 'Linked!'; Actual = (Trim-TrailingNewlines $out) }
    }
    Remove-Item -Force $log -ErrorAction SilentlyContinue

    $stdoutFile = [System.IO.Path]::GetTempFileName()
    & $MaizeExe $mzx > $stdoutFile 2> $null
    $me = $LASTEXITCODE
    $actualRaw = Get-Content -Raw -Path $stdoutFile -ErrorAction SilentlyContinue
    Remove-Item -Force $stdoutFile -ErrorAction SilentlyContinue
    $actual = Trim-TrailingNewlines $actualRaw
    return [pscustomobject]@{ Name = 'link_multi_tu'; Pass = (($me -eq 0) -and ($actual -eq 'Linked!')); Expected = 'Linked!'; Actual = $actual }
}

function Invoke-LinkRejectTest($name, $expected, $objs) {
    $mzx = Join-Path $TestRunDir 'err.mzx'
    $mzldArgs = @('-o', $mzx) + $objs
    $log = [System.IO.Path]::GetTempFileName()
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    & $MzldExe @mzldArgs *> $log
    $ec = $LASTEXITCODE
    $ErrorActionPreference = $prevEap
    $out = Get-Content -Raw -Path $log -ErrorAction SilentlyContinue
    Remove-Item -Force $log -ErrorAction SilentlyContinue
    $rejected = ($ec -ne 0) -and ($null -ne $out) -and ($out -like "*$expected*")
    return [pscustomobject]@{ Name = $name; Pass = $rejected; Expected = "link rejects with: $expected"; Actual = (Trim-TrailingNewlines $out) }
}

$results += Invoke-LinkRunTest
$results += Invoke-LinkRejectTest 'link_undefined_symbol' "undefined symbol 'msgB'" @((Join-Path $TestRunDir 'link_a.mzo'))
Emit-Object 'link_range.mazm'
$results += Invoke-LinkRejectTest 'link_range_overflow' 'does not fit in 8-bit' @((Join-Path $TestRunDir 'link_range.mzo'))

# --- maize-14: mzdis disassembler ---------------------------------------------------
# Round trip (AC6477/AC6478/AC6483): assemble a code-only, SECTION-clean fixture that
# hits every addressing-mode family and operand-count shape, disassemble it, reassemble
# mzdis's own output text, and diff the resulting .mzb against the original byte-for-
# byte -- the strongest test the spec names.
function Invoke-MzdisRoundtripTest {
    $name = 'mzdis_roundtrip'
    $srcPath = Join-Path $AsmDir 'test_mzdis_roundtrip.mazm'
    $asmPath = Join-Path $TestRunDir 'test_mzdis_roundtrip.mazm'
    Copy-Item -Path $srcPath -Destination $asmPath -Force
    $binPath = [System.IO.Path]::ChangeExtension($asmPath, 'mzb')

    & $MazmExe $asmPath *> $null
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $binPath)) {
        return [pscustomobject]@{ Name = $name; Pass = $false; Expected = 'fixture assembles cleanly'; Actual = 'mazm failed to produce a .mzb' }
    }

    $disPath = Join-Path $TestRunDir 'test_mzdis_roundtrip.dis.mazm'
    & $MzdisExe -o $disPath $binPath
    $disExit = $LASTEXITCODE
    if ($disExit -ne 0 -or -not (Test-Path $disPath)) {
        return [pscustomobject]@{ Name = $name; Pass = $false; Expected = 'mzdis exits 0 with clean decode'; Actual = "mzdis exit $disExit" }
    }
    $disText = Get-Content -Raw -Path $disPath
    if ($disText -match '(?i)unknown opcode|malformed|TRUNCATED') {
        return [pscustomobject]@{ Name = $name; Pass = $false; Expected = 'no unknown/malformed/truncated lines'; Actual = 'decode diagnostic found in a code-only fixture' }
    }

    $reasmBin = [System.IO.Path]::ChangeExtension($disPath, 'mzb')
    & $MazmExe $disPath *> $null
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $reasmBin)) {
        return [pscustomobject]@{ Name = $name; Pass = $false; Expected = "mzdis's own output reassembles cleanly"; Actual = 'mazm failed to reassemble mzdis output' }
    }

    $origBytes = [System.IO.File]::ReadAllBytes($binPath)
    $reasmBytes = [System.IO.File]::ReadAllBytes($reasmBin)
    $identical = ($origBytes.Length -eq $reasmBytes.Length) -and (-not (Compare-Object $origBytes $reasmBytes))
    return [pscustomobject]@{ Name = $name; Pass = $identical; Expected = 'reassembled .mzb byte-identical to original'; Actual = if ($identical) { 'byte-identical' } else { "length $($origBytes.Length) vs $($reasmBytes.Length), or content differs" } }
}

# Reserved-opcode resync (AC6481): two reserved bytes decode as DB $XX / unknown
# opcode, advancing exactly one byte each, and decoding resumes correctly afterward.
function Invoke-MzdisReservedTest {
    $name = 'mzdis_reserved_resync'
    $srcPath = Join-Path $AsmDir 'test_mzdis_reserved.mazm'
    $asmPath = Join-Path $TestRunDir 'test_mzdis_reserved.mazm'
    Copy-Item -Path $srcPath -Destination $asmPath -Force
    $binPath = [System.IO.Path]::ChangeExtension($asmPath, 'mzb')

    & $MazmExe $asmPath *> $null
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $binPath)) {
        return [pscustomobject]@{ Name = $name; Pass = $false; Expected = 'fixture assembles cleanly'; Actual = 'mazm failed to produce a .mzb' }
    }

    $stdoutFile = Join-Path $TestRunDir 'test_mzdis_reserved.out'
    & $MzdisExe $binPath > $stdoutFile
    $disExit = $LASTEXITCODE
    $text = Get-Content -Raw -Path $stdoutFile

    # Reserved-byte resyncs alone must NOT force exit 1 (spec: "Exit codes").
    $pass = ($disExit -eq 0) `
        -and ($text -match [regex]::Escape('DB $21') + '.*unknown opcode') `
        -and ($text -match [regex]::Escape('DB $93') + '.*unknown opcode') `
        -and ($text -match '(?m)^\s+NOP\b') `
        -and ($text -notmatch '(?i)TRUNCATED')
    return [pscustomobject]@{ Name = $name; Pass = $pass; Expected = 'DB $21/$93 unknown-opcode lines, NOP decodes correctly after, exit 0'; Actual = if ($pass) { 'as expected' } else { "exit $disExit; see $stdoutFile" } }
}

# Truncated tail (AC6482): chop the assembled fixture mid-immediate so the final
# instruction's declared bytes run past EOF. mzdis must still emit everything decoded
# before the cut, a trailing "; TRUNCATED ..." line, and exit 1.
function Invoke-MzdisTruncatedTest {
    $name = 'mzdis_truncated_tail'
    $srcPath = Join-Path $AsmDir 'test_mzdis_truncate_src.mazm'
    $asmPath = Join-Path $TestRunDir 'test_mzdis_truncate_src.mazm'
    Copy-Item -Path $srcPath -Destination $asmPath -Force
    $binPath = [System.IO.Path]::ChangeExtension($asmPath, 'mzb')

    & $MazmExe $asmPath *> $null
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $binPath)) {
        return [pscustomobject]@{ Name = $name; Pass = $false; Expected = 'fixture assembles cleanly'; Actual = 'mazm failed to produce a .mzb' }
    }

    # HALT(1) + CLR R0(2) + CP $12345678 R0 (opcode+param+4-byte imm = 6) = 9 real
    # bytes; keep only the first 8, cutting the immediate 2 bytes short.
    $allBytes = [System.IO.File]::ReadAllBytes($binPath)
    $truncPath = Join-Path $TestRunDir 'test_mzdis_truncate.mzb'
    [System.IO.File]::WriteAllBytes($truncPath, $allBytes[0..7])

    $stdoutFile = Join-Path $TestRunDir 'test_mzdis_truncate.out'
    & $MzdisExe $truncPath > $stdoutFile
    $disExit = $LASTEXITCODE
    $text = Get-Content -Raw -Path $stdoutFile

    $pass = ($disExit -eq 1) `
        -and ($text -match '(?m)^\s+HALT\b') `
        -and ($text -match '(?m)^\s+CLR R0\b') `
        -and ($text -match '(?i)TRUNCATED')
    return [pscustomobject]@{ Name = $name; Pass = $pass; Expected = 'partial output (HALT, CLR R0) + TRUNCATED diagnostic, exit 1'; Actual = if ($pass) { 'as expected' } else { "exit $disExit; see $stdoutFile" } }
}

# .mzo rejection (AC6480): exit 1, a diagnostic naming the file, and no stdout output.
function Invoke-MzdisMzoRejectTest {
    $name = 'mzdis_mzo_reject'
    $mzoPath = Join-Path $TestRunDir 'link_a.mzo'
    if (-not (Test-Path $mzoPath)) {
        return [pscustomobject]@{ Name = $name; Pass = $false; Expected = 'link_a.mzo present from the link tests'; Actual = 'missing (link tests must run first)' }
    }

    $stdoutFile = Join-Path $TestRunDir 'test_mzdis_mzo.out'
    $stderrFile = Join-Path $TestRunDir 'test_mzdis_mzo.err'
    & $MzdisExe $mzoPath > $stdoutFile 2> $stderrFile
    $disExit = $LASTEXITCODE
    $stdoutText = Get-Content -Raw -Path $stdoutFile -ErrorAction SilentlyContinue
    $stderrText = Get-Content -Raw -Path $stderrFile -ErrorAction SilentlyContinue

    $pass = ($disExit -eq 1) `
        -and ([string]::IsNullOrEmpty($stdoutText)) `
        -and ($stderrText -like '*.mzo relocatable object*') `
        -and ($stderrText -like "*$([System.IO.Path]::GetFileName($mzoPath))*")
    return [pscustomobject]@{ Name = $name; Pass = $pass; Expected = 'exit 1, no stdout, stderr names the .mzo file'; Actual = if ($pass) { 'as expected' } else { "exit $disExit; stdout=`"$stdoutText`"; stderr=`"$stderrText`"" } }
}

# .mzx segment routing (AC6479): CODE decodes as instructions at vaddr (with an ENTRY
# annotation), RODATA renders as DATA lines, never passed through the instruction
# decoder.
function Invoke-MzdisMzxTest {
    $name = 'mzdis_mzx_segments'
    $mzxPath = Join-Path $TestRunDir 'link.mzx'
    if (-not (Test-Path $mzxPath)) {
        return [pscustomobject]@{ Name = $name; Pass = $false; Expected = 'link.mzx present from the link tests'; Actual = 'missing (link tests must run first)' }
    }

    $stdoutFile = Join-Path $TestRunDir 'test_mzdis_mzx.out'
    & $MzdisExe $mzxPath > $stdoutFile
    $disExit = $LASTEXITCODE
    $text = Get-Content -Raw -Path $stdoutFile

    $pass = ($disExit -eq 0) `
        -and ($text -match '(?m)^\s+CALL\b.*ENTRY') `
        -and ($text -match '(?m)^\s+RET\b') `
        -and ($text -match 'RODATA') `
        -and ($text -match [regex]::Escape('DATA $4C $69 $6E $6B $65 $64 $21'))
    return [pscustomobject]@{ Name = $name; Pass = $pass; Expected = 'CODE decoded with ENTRY annotation, RODATA rendered as DATA $4C ... ("Linked!")'; Actual = if ($pass) { 'as expected' } else { "exit $disExit; see $stdoutFile" } }
}

$results += Invoke-MzdisRoundtripTest
$results += Invoke-MzdisReservedTest
$results += Invoke-MzdisTruncatedTest
$results += Invoke-MzdisMzoRejectTest
$results += Invoke-MzdisMzxTest

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
