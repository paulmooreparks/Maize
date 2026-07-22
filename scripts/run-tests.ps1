#Requires -Version 5.1
<#
.SYNOPSIS
    Build both Maize binaries and run the in-scope asm/ test suite (Windows).

.DESCRIPTION
    Configures and builds the windows-llvm-mingw-debug preset (or an override given
    via -Preset), then assembles and runs each of the 49 in-scope tests under asm/,
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
    [pscustomobject]@{ Name = 'test_flags_arith';  File = 'test_flags_arith.mazm';  Expected = 'flags arith: PASS';             Golden = $true }
    [pscustomobject]@{ Name = 'test_flags_branch'; File = 'test_flags_branch.mazm'; Expected = 'flags branch: PASS';           Golden = $false }
    [pscustomobject]@{ Name = 'test_flags_shl';    File = 'test_flags_shl.mazm';    Expected = 'flags shl: PASS';               Golden = $false }
    [pscustomobject]@{ Name = 'test_flags_shr';    File = 'test_flags_shr.mazm';    Expected = 'flags shr: PASS';               Golden = $false }
    [pscustomobject]@{ Name = 'test_flags_sar';    File = 'test_flags_sar.mazm';    Expected = 'flags sar: PASS';               Golden = $false }
    [pscustomobject]@{ Name = 'test_flags_mul8';   File = 'test_flags_mul8.mazm';   Expected = 'flags mul8: PASS';              Golden = $false }
    [pscustomobject]@{ Name = 'test_mul_zero';     File = 'test_mul_zero.mazm';     Expected = 'mul zero: PASS';                Golden = $true }
    [pscustomobject]@{ Name = 'test_flags_move';   File = 'test_flags_move.mazm';   Expected = 'flags move: PASS';              Golden = $false }
    # maize-197 lazy / on-demand flag computation boundary tests (materialize hook +
    # narrow/neutral producers run after an unresolved pending ALU op).
    [pscustomobject]@{ Name = 'test_flags_lazy_setcc';   File = 'test_flags_lazy_setcc.mazm';   Expected = 'flags lazy setcc: PASS';   Golden = $true }
    [pscustomobject]@{ Name = 'test_flags_lazy_setcry';  File = 'test_flags_lazy_setcry.mazm';  Expected = 'flags lazy setcry: PASS';  Golden = $true }
    [pscustomobject]@{ Name = 'test_flags_lazy_cmpxchg'; File = 'test_flags_lazy_cmpxchg.mazm'; Expected = 'flags lazy cmpxchg: PASS'; Golden = $true }
    [pscustomobject]@{ Name = 'test_flags_lazy_lea';     File = 'test_flags_lazy_lea.mazm';     Expected = 'flags lazy lea: PASS';     Golden = $true }
    [pscustomobject]@{ Name = 'test_flags_lazy_fcmp';    File = 'test_flags_lazy_fcmp.mazm';    Expected = 'flags lazy fcmp: PASS';    Golden = $true }
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
    [pscustomobject]@{ Name = 'test_setcc_alias';   File = 'test_setcc_alias.mazm';   Expected = 'setcc-alias: PASS';             Golden = $false }
    [pscustomobject]@{ Name = 'test_memblock';      File = 'test_memblock.mazm';      Expected = 'memblock: PASS';                Golden = $false }
    [pscustomobject]@{ Name = 'test_widecount';     File = 'test_widecount.mazm';     Expected = 'widecount: PASS';               Golden = $false }
    [pscustomobject]@{ Name = 'test_crossblock';    File = 'test_crossblock.mazm';    Expected = 'crossblk: PASS';                Golden = $false }
    [pscustomobject]@{ Name = 'test_adc';           File = 'test_adc.mazm';           Expected = 'adc: PASS';                     Golden = $false }
    [pscustomobject]@{ Name = 'test_copywidth';     File = 'test_copywidth.mazm';     Expected = 'copywidth: PASS';               Golden = $false }
    # maize-196: ALU/CMP memory-source operands read at the destination-subregister
    # width (not a fixed 8 bytes); sub-width regAddr forms incl. high-offset dest R0.B3.
    [pscustomobject]@{ Name = 'test_alu_memsrc_width'; File = 'test_alu_memsrc_width.mazm'; Expected = 'alu memsrc width: PASS';       Golden = $false }
    # Self-modifying-code contract (from the maize-307 investigation): a store into code
    # bytes takes effect for the NEXT fetch; any future decoded/JIT tier must preserve this.
    [pscustomobject]@{ Name = 'test_selfmod';       File = 'test_selfmod.mazm';       Expected = 'selfmod: PASS';                 Golden = $false }
    [pscustomobject]@{ Name = 'test_jit_selfmod_hot'; File = 'test_jit_selfmod_hot.mazm'; Expected = 'selfmod hot: PASS';         Golden = $false }
    # maize-272 PUSHALL/POPALL: flag neutrality, the 13-register frame ABI, full round trip.
    [pscustomobject]@{ Name = 'test_pushall';       File = 'test_pushall.mazm';       Expected = 'pushall: PASS';                 Golden = $false }
    [pscustomobject]@{ Name = 'oob_subreg_guard';   File = 'test_oob_subreg.mazm';    Expected = 'oob subreg: PASS';              Golden = $false }
    [pscustomobject]@{ Name = 'reject_ld_value';    File = 'test_reject_ldval.mazm';   Expected = 'reads from a memory address';   Golden = $false; ExpectAsmError = $true }
    [pscustomobject]@{ Name = 'test_ldz';           File = 'test_ldz.mazm';            Expected = 'ldz: PASS';                     Golden = $false }
    [pscustomobject]@{ Name = 'test_call_ind';      File = 'test_call_ind.mazm';      Expected = 'call ind: PASS';                Golden = $false }
    [pscustomobject]@{ Name = 'test_setint';        File = 'test_setint.mazm';        Expected = 'setint: PASS';                  Golden = $false }
    [pscustomobject]@{ Name = 'test_outr_in';       File = 'test_outr_in.mazm';       Expected = 'outr/in: PASS';                 Golden = $false }
    [pscustomobject]@{ Name = 'test_unpop_port';     File = 'test_unpop_port.mazm';     Expected = 'unpop: PASS';                   Golden = $false }
    [pscustomobject]@{ Name = 'test_portio';         File = 'test_portio.mazm';         Expected = 'portio: PASS';                  Golden = $false }
    [pscustomobject]@{ Name = 'test_timer';          File = 'test_timer.mazm';          Expected = 'timer: PASS';                   Golden = $false }
    [pscustomobject]@{ Name = 'test_framebuffer';    File = 'test_framebuffer.mazm';    Expected = 'framebuffer: PASS';             Golden = $false }
    [pscustomobject]@{ Name = 'fb_registration';     File = 'test_fb_registration.mazm'; Expected = 'fbreg: PASS';                  Golden = $false }
    [pscustomobject]@{ Name = 'test_sysbrk';        File = 'test_sysbrk.mazm';        Expected = 'sysbrk: PASS';                  Golden = $false }
    [pscustomobject]@{ Name = 'test_syserrno';      File = 'test_syserrno.mazm';      Expected = 'syserrno: PASS';                Golden = $false }
    [pscustomobject]@{ Name = 'test_tstind';        File = 'test_tstind.mazm';        Expected = 'tstind: PASS';                  Golden = $false }
    [pscustomobject]@{ Name = 'reject_bad_register'; File = 'test_reject_badreg.mazm';     Expected = "unknown register 'R99'";      Golden = $false; ExpectAsmError = $true }
    [pscustomobject]@{ Name = 'reject_bad_literal';  File = 'test_reject_badliteral.mazm'; Expected = 'malformed hex literal';       Golden = $false; ExpectAsmError = $true }
    [pscustomobject]@{ Name = 'reject_include_self'; File = 'test_reject_include_self.mazm'; Expected = 'circular INCLUDE';          Golden = $false; ExpectAsmError = $true }
    [pscustomobject]@{ Name = 'reject_label_trunc';  File = 'test_reject_label_trunc.mazm'; Expected = 'unexpected end of file';     Golden = $false; ExpectAsmError = $true }
    [pscustomobject]@{ Name = 'reject_address_trunc'; File = 'test_reject_address_trunc.mazm'; Expected = 'unexpected end of file';  Golden = $false; ExpectAsmError = $true }
    [pscustomobject]@{ Name = 'reject_jcc_reg';      File = 'test_reject_jcc_reg.mazm';    Expected = 'immediate target only';       Golden = $false; ExpectAsmError = $true }
    [pscustomobject]@{ Name = 'reject_jmp_subreg';   File = 'test_reject_jmp_subreg.mazm'; Expected = 'full 64-bit width';           Golden = $false; ExpectAsmError = $true }
    [pscustomobject]@{ Name = 'test_fp';             File = 'test_fp.mazm';                 Expected = 'fp: PASS';                    Golden = $false }
    [pscustomobject]@{ Name = 'reject_fp_subreg';    File = 'test_fp_reject_subreg.mazm';   Expected = 'B* or Q* subregister';        Golden = $false; ExpectAsmError = $true }
    [pscustomobject]@{ Name = 'reject_fp_mixwidth';  File = 'test_fp_reject_mixwidth.mazm'; Expected = 'same floating-point width';   Golden = $false; ExpectAsmError = $true }
    [pscustomobject]@{ Name = 'nested_include';      File = 'test_nested_include.mazm';    Expected = 'nested include: PASS';        Golden = $true }
    [pscustomobject]@{ Name = 'address_fwdlabel';    File = 'test_address_fwdlabel.mazm';  Expected = 'address fwd-ref: PASS';       Golden = $false }
    [pscustomobject]@{ Name = 'mmu_cr_roundtrip';    File = 'test_mmu_cr_roundtrip.mazm';  Expected = 'cr-roundtrip: PASS';          Golden = $false }
    # maize-194: Sv48 translation, software TLB, cause-8 page fault. Each fixture builds a
    # page table in guest RAM, MOVTCRs MODE=1 into CR0, and self-checks a PASS/FAIL marker.
    [pscustomobject]@{ Name = 'mmu_translate_rw';    File = 'test_mmu_translate_rw.mazm';          Expected = 'mmu-xlate-rw: PASS';   Golden = $false }
    [pscustomobject]@{ Name = 'mmu_pagefault_np';    File = 'test_mmu_pagefault_notpresent.mazm';  Expected = 'mmu-fault-np: PASS';   Golden = $false }
    [pscustomobject]@{ Name = 'mmu_pagefault_ro';    File = 'test_mmu_pagefault_ro.mazm';          Expected = 'mmu-fault-ro: PASS';   Golden = $false }
    [pscustomobject]@{ Name = 'mmu_pagefault_user';  File = 'test_mmu_pagefault_user.mazm';        Expected = 'mmu-fault-user: PASS'; Golden = $false }
    [pscustomobject]@{ Name = 'mmu_tlb_invalidate';  File = 'test_mmu_tlb_invalidate.mazm';        Expected = 'mmu-tlb: PASS';        Golden = $false }
    [pscustomobject]@{ Name = 'mmu_translate_alu_ra';File = 'test_mmu_translate_alu_regaddr.mazm'; Expected = 'mmu-alu-ra: PASS';     Golden = $false }
    [pscustomobject]@{ Name = 'mmu_translate_ind_cf';File = 'test_mmu_translate_indirect_cf.mazm'; Expected = 'mmu-ind-cf: PASS';     Golden = $false }
    [pscustomobject]@{ Name = 'mmu_translate_out';   File = 'test_mmu_translate_out.mazm';         Expected = 'mmu-out: PASS';        Golden = $false }
    [pscustomobject]@{ Name = 'mmu_pushfault_restart';File = 'test_mmu_pushfault_restart.mazm';     Expected = 'mmu-pushfault: PASS';  Golden = $false }
    # maize-71: flat-mode EXTERN'd-but-undefined reference has no linker to resolve it.
    [pscustomobject]@{ Name = 'flat_unresolved_extern'; File = 'test_reject_unresolved_extern.mazm'; Expected = "unresolved external 'undefsym'"; Golden = $false; ExpectAsmError = $true }
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
    # maize-263 (D13): cap build parallelism to leave the operator at least two cores
    # free, mirroring run-tests.sh so the .sh/.ps1 twins stay in parity. CI keeps the
    # current full-parallel behavior (the one hard must-not-regress). No native mirror
    # and no niceness here: there is no WSL/9P bridge on native Windows PowerShell and
    # no direct POSIX nice/ionice equivalent (both left as follow-ups per D13).
    $IsCi = ($env:CI) -or ($env:GITHUB_ACTIONS)
    Push-Location $RepoRoot
    try {
        & $CmakeExe --preset $Preset
        $configureExit = $LASTEXITCODE
        if ($configureExit -eq 0) {
            if ($IsCi) {
                & $CmakeExe --build --preset $Preset
            } else {
                $Jobs = [Environment]::ProcessorCount - 2
                if ($Jobs -lt 2) { $Jobs = 2 }
                Write-Host "run-tests.ps1: using $Jobs build jobs (ProcessorCount=$([Environment]::ProcessorCount))"
                & $CmakeExe --build --preset $Preset --parallel $Jobs
            }
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

# --- maize-330: optional JIT leg. MAIZE_JIT=1 runs every maize invocation under --jit;
# MAIZE_JIT=check runs under --jit-check (differential verification). Implemented as a
# .cmd exec wrapper so the suite's call sites (both `&` calls and ProcessStartInfo)
# stay unchanged. MAIZE_JIT_THRESHOLD overrides the hotness threshold (1 = compile
# everything, the aggressive correctness setting). Keep in sync with run-tests.sh.
if ($env:MAIZE_JIT) {
    $jitFlag = if ($env:MAIZE_JIT -eq 'check') { '--jit-check' } else { '--jit' }
    $jitThreshold = if ($env:MAIZE_JIT_THRESHOLD) { $env:MAIZE_JIT_THRESHOLD } else { '50' }
    $jitWrap = Join-Path $TestRunDir 'maize-jit-wrap.cmd'
    "@echo off`r`n`"$MaizeExe`" $jitFlag --jit-threshold $jitThreshold %*" |
        Set-Content -Path $jitWrap -Encoding ascii
    $MaizeExe = $jitWrap
    Write-Host "run-tests.ps1: running maize under $jitFlag (threshold $jitThreshold)"
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
    # maize-221: pipe $null so the child gets a non-interactive (closed) stdin; the
    # console VM traps a framebuffer takeover only on an interactive tty, so this keeps
    # the headless test_framebuffer untrapped no matter how the suite is launched.
    $null | & $MaizeExe $binPath > $stdoutFile 2> $stderrFile
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

# --- maize-72: per-reference undefined-label diagnostics --------------------------
# Several distinct undefined labels referenced from distinct lines must each report
# at their OWN file:line, and a label referenced from TWO different lines
# (undefined_beta, lines 14 and 19) must report on BOTH lines rather than twice on
# the first and never on the second. The generic Invoke-Test negative form only
# greps for a single substring, so this fixture gets a bespoke check that asserts
# each expected file:line diagnostic is present.
function Invoke-UndefMultirefTest {
    $name = 'undef_multiref'
    $srcPath = Join-Path $AsmDir 'test_reject_undef_multiref.mazm'
    $asmPath = Join-Path $TestRunDir 'test_reject_undef_multiref.mazm'
    Copy-Item -Path $srcPath -Destination $asmPath -Force

    $log = [System.IO.Path]::GetTempFileName()
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    & $MazmExe $asmPath *> $log
    $ec = $LASTEXITCODE
    $ErrorActionPreference = $prevEap
    $actual = Get-Content -Raw -Path $log -ErrorAction SilentlyContinue
    Remove-Item -Force $log -ErrorAction SilentlyContinue
    if ($null -eq $actual) { $actual = '' }

    $pass = ($ec -ne 0) `
        -and ($actual -match ":14: error: undefined label 'undefined_beta'") `
        -and ($actual -match ":15: error: undefined label 'undefined_alpha'") `
        -and ($actual -match ":19: error: undefined label 'undefined_beta'") `
        -and ($actual -match ":20: error: undefined label 'undefined_gamma'")
    return [pscustomobject]@{
        Name     = $name
        Pass     = $pass
        Expected = 'nonzero exit; diagnostics at lines 14, 15, 19, 20, each on its own site'
        Actual   = "exit ${ec}; $(Trim-TrailingNewlines $actual)"
    }
}

# --- Reusable expect-trap facility (maize-21, generalizing the former bespoke brk
#     runner). A trap-style fixture places an unreachable "FAIL" marker after the
#     instruction that must trap; with no handler installed the VM halts deterministically
#     with the cause surfaced, which the generic Invoke-Test (exact-stdout match) cannot
#     express. Invoke-TrapTest asserts the VM exits nonzero, surfaces $errSubstr on
#     stderr, and never reaches the fall-through marker on stdout. Mirrors run-tests.sh's
#     run_brk_trap_test / run_priv_fault_trap_test so the two runners assert the same thing.
#       - brk_trap        (test_brk.mazm):        'breakpoint' on stderr (cause 3).
#       - priv_fault_trap (test_priv_fault.mazm): 'privileg' on stderr (cause 4, maize-21).
function Invoke-TrapTest($name, $src, $errSubstr) {
    $srcPath = Join-Path $AsmDir $src
    $asmPath = Join-Path $TestRunDir $src
    Copy-Item -Path $srcPath -Destination $asmPath -Force
    $binPath = [System.IO.Path]::ChangeExtension($asmPath, 'mzb')

    & $MazmExe $asmPath *> $null
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $binPath)) {
        return [pscustomobject]@{ Name = $name; Pass = $false; Expected = 'fixture assembles cleanly'; Actual = 'mazm failed to assemble' }
    }

    $stdoutFile = [System.IO.Path]::GetTempFileName()
    $stderrFile = [System.IO.Path]::GetTempFileName()
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    & $MaizeExe $binPath > $stdoutFile 2> $stderrFile
    $maizeExit = $LASTEXITCODE
    $ErrorActionPreference = $prevEap
    $out = Get-Content -Raw -Path $stdoutFile -ErrorAction SilentlyContinue
    $err = Get-Content -Raw -Path $stderrFile -ErrorAction SilentlyContinue
    Remove-Item -Force $stdoutFile, $stderrFile -ErrorAction SilentlyContinue
    if ($null -eq $out) { $out = '' }
    if ($null -eq $err) { $err = '' }

    $pass = ($maizeExit -ne 0) -and ($err -like "*$errSubstr*") -and ($out -notlike '*FAIL*')
    return [pscustomobject]@{
        Name     = $name
        Pass     = $pass
        Expected = "nonzero exit, '$errSubstr' on stderr, no fall-through marker on stdout"
        Actual   = "exit ${maizeExit}; stdout=`"$(Trim-TrailingNewlines $out)`"; stderr=`"$(Trim-TrailingNewlines $err)`""
    }
}

# --- maize-75: sys_read byte-count fix (needs a known stdin) ----------------------
# Invoke-Test gives no stdin, so this bespoke runner pipes "hello" and checks the
# program echoes exactly the bytes read plus an EOF marker: "hello|EOF". The old
# fall-through-to-0 bug (and any short-read tail spill or nonzero EOF return)
# produces different output, so a byte-exact match gates the fix.
function Invoke-SysreadTest {
    $name = 'sysread_count'
    $expected = 'hello|EOF'
    $srcPath = Join-Path $AsmDir 'test_sysread.mazm'
    $asmPath = Join-Path $TestRunDir 'test_sysread.mazm'
    Copy-Item -Path $srcPath -Destination $asmPath -Force
    $binPath = [System.IO.Path]::ChangeExtension($asmPath, 'mzb')

    & $MazmExe $asmPath *> $null
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $binPath)) {
        return [pscustomobject]@{ Name = $name; Pass = $false; Expected = $expected; Actual = 'mazm failed to assemble' }
    }

    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $MaizeExe
    $psi.Arguments = "`"$binPath`""
    $psi.RedirectStandardInput = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.UseShellExecute = $false
    $proc = [System.Diagnostics.Process]::Start($psi)
    $proc.StandardInput.Write('hello')
    $proc.StandardInput.Close()
    $stdout = $proc.StandardOutput.ReadToEnd()
    $null = $proc.StandardError.ReadToEnd()
    $proc.WaitForExit()
    $maizeExit = $proc.ExitCode

    $actual = Trim-TrailingNewlines $stdout
    $pass = ($maizeExit -eq 0) -and ($actual -eq $expected)
    return [pscustomobject]@{ Name = $name; Pass = $pass; Expected = $expected; Actual = $actual }
}

# --- maize-21: a period-1 timer must not lose an IRQ raised during the masked handler --
# A period-1 periodic timer raises on every tick, including inside the masked handler
# window; delivery must gate on the durable irq_pending latch, not the RF interrupt-set
# mirror (which IRET restores clear), or the guest services one tick and livelocks. The
# handler drives termination, so this is an exact-stdout check bounded by a WaitForExit
# deadline: a lost-IRQ regression trips the deadline instead of hanging the suite (same
# Process API + bounded-wait idiom as Invoke-ObjBackjmpTest). Mirrors run-tests.sh's
# run_timer_period1_test.
function Invoke-TimerPeriod1Test {
    $name = 'timer_period1'
    $expected = 'timerp1: PASS'
    $srcPath = Join-Path $AsmDir 'test_timer_period1.mazm'
    $asmPath = Join-Path $TestRunDir 'test_timer_period1.mazm'
    Copy-Item -Path $srcPath -Destination $asmPath -Force
    $binPath = [System.IO.Path]::ChangeExtension($asmPath, 'mzb')

    & $MazmExe $asmPath *> $null
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $binPath)) {
        return [pscustomobject]@{ Name = $name; Pass = $false; Expected = $expected; Actual = 'mazm failed to assemble' }
    }

    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $MaizeExe
    $psi.Arguments = "`"$binPath`""
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.UseShellExecute = $false
    $proc = [System.Diagnostics.Process]::Start($psi)
    if (-not $proc.WaitForExit(10000)) {
        try { $proc.Kill() } catch { }
        return [pscustomobject]@{ Name = $name; Pass = $false; Expected = $expected; Actual = 'timed out (lost-IRQ livelock?)' }
    }
    $me = $proc.ExitCode
    $actualRaw = $proc.StandardOutput.ReadToEnd()
    $actual = Trim-TrailingNewlines $actualRaw
    return [pscustomobject]@{ Name = $name; Pass = (($me -eq 0) -and ($actual -eq $expected)); Expected = $expected; Actual = $actual }
}

# --- keyboard device: injected-scancode round trip (needs a known piped stdin) --------
# Invoke-Test gives no stdin, so this bespoke runner pipes a known four-byte Set-1
# scancode sequence (make codes for A, B, C, D) and runs maize with --input=keyboard so
# the keyboard is the sole stdin consumer. The guest installs an IRQ-34 handler, collects
# the four scancodes, verifies them, and prints "keyboard: PASS". Same raw Process API +
# byte-stream stdin idiom as Invoke-SysreadTest.
function Invoke-KeyboardTest {
    $name = 'keyboard'
    $expected = 'keyboard: PASS'
    $srcPath = Join-Path $AsmDir 'test_keyboard.mazm'
    $asmPath = Join-Path $TestRunDir 'test_keyboard.mazm'
    Copy-Item -Path $srcPath -Destination $asmPath -Force
    $binPath = [System.IO.Path]::ChangeExtension($asmPath, 'mzb')

    & $MazmExe $asmPath *> $null
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $binPath)) {
        return [pscustomobject]@{ Name = $name; Pass = $false; Expected = $expected; Actual = 'mazm failed to assemble' }
    }

    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $MaizeExe
    $psi.Arguments = "--input=keyboard `"$binPath`""
    $psi.RedirectStandardInput = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.UseShellExecute = $false
    $proc = [System.Diagnostics.Process]::Start($psi)
    # Inject scancodes 0x1E 0x30 0x2E 0x20 (make codes for A, B, C, D).
    $bytes = [byte[]](0x1E, 0x30, 0x2E, 0x20)
    $stdin = $proc.StandardInput.BaseStream
    $stdin.Write($bytes, 0, $bytes.Length)
    $stdin.Close()
    $stdout = $proc.StandardOutput.ReadToEnd()
    $null = $proc.StandardError.ReadToEnd()
    $proc.WaitForExit()
    $me = $proc.ExitCode

    $actual = Trim-TrailingNewlines $stdout
    $pass = ($me -eq 0) -and ($actual -eq $expected)
    return [pscustomobject]@{ Name = $name; Pass = $pass; Expected = $expected; Actual = $actual }
}

# --- maize-240: a masked-window IRQ collision must deliver BOTH vectors ----------------
# A periodic timer (vector 32) and the console readiness IRQ (vector 33) raised inside the
# same masked window must both survive delivery; the old single-slot latch dropped one, and
# a lost timer vector left its tick-pending bit unacked so the timer wedged permanently. The
# guest prints PASS only after >= 3 serviced timer ticks AND >= 1 serviced console IRQ, so a
# dropped-vector regression livelocks and trips the WaitForExit deadline instead of hanging
# the suite. One byte is piped to stdin with the DEFAULT invocation (the console is the
# default input source, maize-238). Same Process API + bounded-wait idiom as
# Invoke-TimerPeriod1Test; mirrors run-tests.sh's run_irq_collision_test (maize-215).
function Invoke-IrqCollisionTest {
    $name = 'irq_collision'
    $expected = 'irqcoll: PASS'
    $srcPath = Join-Path $AsmDir 'test_irq_collision.mazm'
    $asmPath = Join-Path $TestRunDir 'test_irq_collision.mazm'
    Copy-Item -Path $srcPath -Destination $asmPath -Force
    $binPath = [System.IO.Path]::ChangeExtension($asmPath, 'mzb')

    & $MazmExe $asmPath *> $null
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $binPath)) {
        return [pscustomobject]@{ Name = $name; Pass = $false; Expected = $expected; Actual = 'mazm failed to assemble' }
    }

    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $MaizeExe
    $psi.Arguments = "`"$binPath`""
    $psi.RedirectStandardInput = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.UseShellExecute = $false
    $proc = [System.Diagnostics.Process]::Start($psi)
    # One byte drives the console's rising-edge readiness IRQ inside the masked window.
    $bytes = [byte[]](0x58)
    $stdin = $proc.StandardInput.BaseStream
    $stdin.Write($bytes, 0, $bytes.Length)
    $stdin.Close()
    if (-not $proc.WaitForExit(10000)) {
        try { $proc.Kill() } catch { }
        return [pscustomobject]@{ Name = $name; Pass = $false; Expected = $expected; Actual = 'timed out (dropped-IRQ wedge?)' }
    }
    $me = $proc.ExitCode
    $actual = Trim-TrailingNewlines $proc.StandardOutput.ReadToEnd()
    $null = $proc.StandardError.ReadToEnd()
    return [pscustomobject]@{ Name = $name; Pass = (($me -eq 0) -and ($actual -eq $expected)); Expected = $expected; Actual = $actual }
}

# --- maize-236: framebuffer registration table, the two display-less branches ---------
# The same test_fb_registration fixture self-adapts to the host display policy. Under
# --fb-no-display the device rejects the claim (STATUS bit2) and the fixture takes its
# per-exec-rejection branch, still printing "fbreg: PASS" (AC6: VM keeps running). Under
# --fb-no-display --fb-stop-on-claim the claim powers the VM off (the maize-221 bare
# diagnostic path, AC7): the fixture never reaches its print, so stdout carries neither
# PASS nor FAIL. Both assemble their own copy, mirroring Invoke-KeyboardTest.
function Invoke-FbRejectTest {
    $name = 'fb_reject'
    $expected = 'fbreg: PASS'
    $srcPath = Join-Path $AsmDir 'test_fb_registration.mazm'
    $asmPath = Join-Path $TestRunDir 'test_fb_registration.mazm'
    Copy-Item -Path $srcPath -Destination $asmPath -Force
    $binPath = [System.IO.Path]::ChangeExtension($asmPath, 'mzb')

    & $MazmExe $asmPath *> $null
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $binPath)) {
        return [pscustomobject]@{ Name = $name; Pass = $false; Expected = $expected; Actual = 'mazm failed to assemble' }
    }

    # Pipe $null so the child gets a non-interactive (closed) stdin, matching the main
    # run_test at line 297: on an interactive console _isatty(0)=true otherwise arms the
    # maize-221 fb trap (src/maize.cpp) and the console binary exits 3 before the fixture
    # runs. --fb-no-display already forces the rejection path this test wants.
    $out = $null | & $MaizeExe --fb-no-display $binPath 2>$null
    $me = $LASTEXITCODE
    $actual = Trim-TrailingNewlines ($out -join "`n")
    $pass = ($me -eq 0) -and ($actual -eq $expected)
    return [pscustomobject]@{ Name = $name; Pass = $pass; Expected = $expected; Actual = $actual }
}

function Invoke-FbStopTest {
    $name = 'fb_stop_on_claim'
    $expected = 'no PASS/FAIL (VM powers off on the claim)'
    $srcPath = Join-Path $AsmDir 'test_fb_registration.mazm'
    $asmPath = Join-Path $TestRunDir 'test_fb_registration.mazm'
    Copy-Item -Path $srcPath -Destination $asmPath -Force
    $binPath = [System.IO.Path]::ChangeExtension($asmPath, 'mzb')

    & $MazmExe $asmPath *> $null
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $binPath)) {
        return [pscustomobject]@{ Name = $name; Pass = $false; Expected = $expected; Actual = 'mazm failed to assemble' }
    }

    # Pipe $null for a closed stdin (see Invoke-FbRejectTest / line 297): an interactive
    # console would otherwise arm the maize-221 fb trap and exit 3 before this fixture's own
    # --fb-stop-on-claim power-off path is reached.
    $out = $null | & $MaizeExe --fb-no-display --fb-stop-on-claim $binPath 2>$null
    $me = $LASTEXITCODE
    $actual = Trim-TrailingNewlines ($out -join "`n")
    $pass = ($me -eq 0) -and ($actual -notmatch 'PASS') -and ($actual -notmatch 'FAIL')
    return [pscustomobject]@{ Name = $name; Pass = $pass; Expected = $expected; Actual = $actual }
}

$results += Invoke-UndefMultirefTest
$results += Invoke-TrapTest 'brk_trap'        'test_brk.mazm'        'breakpoint'
$results += Invoke-TrapTest 'priv_fault_trap' 'test_priv_fault.mazm' 'privileg'
# maize-180: the four new privileged instructions + the previously-ungated ops each raise
# cause-4 in user mode (MOVTCR/TLBINV, the forged-RF IRET escalation, HALT/SETINT/SETSYSG).
$results += Invoke-TrapTest 'mmu_priv_movtcr'        'test_mmu_priv_movtcr.mazm'          'privileg'
$results += Invoke-TrapTest 'mmu_priv_tlbinv'        'test_mmu_priv_tlbinv.mazm'          'privileg'
$results += Invoke-TrapTest 'mmu_priv_iret_escalate' 'test_mmu_priv_iret_escalation.mazm' 'privileg'
$results += Invoke-TrapTest 'mmu_priv_halt'          'test_mmu_priv_halt.mazm'            'privileg'
$results += Invoke-TrapTest 'mmu_priv_setint'        'test_mmu_priv_setint.mazm'          'privileg'
$results += Invoke-TrapTest 'mmu_priv_setsysg'       'test_mmu_priv_setsysg.mazm'         'privileg'
$results += Invoke-TrapTest 'mmu_priv_rf_write'      'test_mmu_priv_rf_write.mazm'        'privileg'
$results += Invoke-TimerPeriod1Test
$results += Invoke-SysreadTest
$results += Invoke-KeyboardTest
$results += Invoke-IrqCollisionTest
$results += Invoke-FbRejectTest
$results += Invoke-FbStopTest

# --- maize-264: cross-process presentation transport (test-runner-sync note) -----------
# The presenter-transport acceptance fixtures (scripts/pty_presenter_check.py: cross-process
# checksum, doorbell, D16 auto-respawn + storm guard, D15 stale-steal, teardown, input ring)
# drive the console session under a REAL pty, which the Windows console has no stdlib
# equivalent for; they run on the Linux legs of run-tests.sh only, exactly like the
# pty_oksh_* fixtures. The Windows CreateFileMapping shared-memory backend is exercised by
# the Merge-stage CI gate. Listed here per the maize-215 test-runner-sync rule so this twin
# stays visibly aware of the fixtures rather than silently omitting them.
Write-Host "[SKIP] presenter_* (pty transport harness is POSIX-only; runs on the Linux legs; Windows shm rides Merge CI)"

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

# --- maize-89: single-object assemble -> link -> run for DREF / ALIGN ---------------
# Each fixture assembles with -c, links to a .mzx, and runs under maize, proving that
# DREF references resolve to a symbol's linked address (plus a signed addend) and that
# a datum after ALIGN lands on the aligned boundary.
function Invoke-ObjPipelineTest($name, $src, $expected) {
    Emit-Object $src
    $obj = Join-Path $TestRunDir ([System.IO.Path]::ChangeExtension($src, 'mzo'))
    $mzx = Join-Path $TestRunDir ([System.IO.Path]::ChangeExtension($src, 'mzx'))
    if (-not (Test-Path $obj)) {
        return [pscustomobject]@{ Name = $name; Pass = $false; Expected = $expected; Actual = 'mazm -c produced no .mzo' }
    }
    $log = [System.IO.Path]::GetTempFileName()
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    & $MzldExe -o $mzx $obj *> $log
    $linkExit = $LASTEXITCODE
    $ErrorActionPreference = $prevEap
    if ($linkExit -ne 0) {
        $out = Get-Content -Raw -Path $log -ErrorAction SilentlyContinue
        Remove-Item -Force $log -ErrorAction SilentlyContinue
        return [pscustomobject]@{ Name = $name; Pass = $false; Expected = $expected; Actual = "link failed: $(Trim-TrailingNewlines $out)" }
    }
    Remove-Item -Force $log -ErrorAction SilentlyContinue

    $stdoutFile = [System.IO.Path]::GetTempFileName()
    & $MaizeExe $mzx > $stdoutFile 2> $null
    $me = $LASTEXITCODE
    $actualRaw = Get-Content -Raw -Path $stdoutFile -ErrorAction SilentlyContinue
    Remove-Item -Force $stdoutFile -ErrorAction SilentlyContinue
    $actual = Trim-TrailingNewlines $actualRaw
    return [pscustomobject]@{ Name = $name; Pass = (($me -eq 0) -and ($actual -eq $expected)); Expected = $expected; Actual = $actual }
}

# Object-mode reject: mazm -c must exit nonzero with a diagnostic containing $expected.
function Invoke-ObjRejectTest($name, $src, $expected) {
    $srcPath = Join-Path $AsmDir $src
    $dst = Join-Path $TestRunDir $src
    Copy-Item -Path $srcPath -Destination $dst -Force
    $log = [System.IO.Path]::GetTempFileName()
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    & $MazmExe -c $dst *> $log
    $ec = $LASTEXITCODE
    $ErrorActionPreference = $prevEap
    $out = Get-Content -Raw -Path $log -ErrorAction SilentlyContinue
    Remove-Item -Force $log -ErrorAction SilentlyContinue
    $rejected = ($ec -ne 0) -and ($null -ne $out) -and ($out -like "*$expected*")
    return [pscustomobject]@{ Name = $name; Pass = $rejected; Expected = "assembler rejects with: $expected"; Actual = (Trim-TrailingNewlines $out) }
}

# maize-95: a BACKWARD local-label JMP must be relocated in object mode. The main
# fixture is linked AFTER a leading pad object so mzld gives it a nonzero vaddr; if
# regimm_compiler emits no relocation for the backward JMP the jump mis-targets (or
# spins) and the PASS line never prints. Start-Process with a bounded WaitForExit
# both sidesteps the PS 5.1 stderr NativeCommandError artifact and guards a mis-
# target that spins; exceeding the deadline counts as a failure.
function Invoke-ObjBackjmpTest {
    $name = 'obj_backjmp'
    $expected = 'backjmp: PASS'
    Emit-Object 'test_obj_backjmp_pad.mazm'
    Emit-Object 'test_obj_backjmp.mazm'
    $pad  = Join-Path $TestRunDir 'test_obj_backjmp_pad.mzo'
    $main = Join-Path $TestRunDir 'test_obj_backjmp.mzo'
    $mzx  = Join-Path $TestRunDir 'test_obj_backjmp.mzx'
    if ((-not (Test-Path $pad)) -or (-not (Test-Path $main))) {
        return [pscustomobject]@{ Name = $name; Pass = $false; Expected = $expected; Actual = 'mazm -c produced no .mzo' }
    }
    $log = [System.IO.Path]::GetTempFileName()
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    # Pad object FIRST so the main object's CODE section lands at a nonzero vaddr.
    & $MzldExe -o $mzx $pad $main *> $log
    $linkExit = $LASTEXITCODE
    $ErrorActionPreference = $prevEap
    if ($linkExit -ne 0) {
        $out = Get-Content -Raw -Path $log -ErrorAction SilentlyContinue
        Remove-Item -Force $log -ErrorAction SilentlyContinue
        return [pscustomobject]@{ Name = $name; Pass = $false; Expected = $expected; Actual = "link failed: $(Trim-TrailingNewlines $out)" }
    }
    Remove-Item -Force $log -ErrorAction SilentlyContinue

    # Use the raw .NET Process API rather than the Start-Process cmdlet: under
    # Windows PowerShell 5.1, Start-Process -PassThru combined with
    # -RedirectStandardOutput/-RedirectStandardError to FILE paths leaves
    # $proc.ExitCode empty even after WaitForExit(timeout) returns true (pwsh 7
    # is unaffected). Process.Start with stream redirection does not have this
    # problem under either edition; the bounded WaitForExit still guards a
    # mis-targeted backward JMP that spins instead of printing and exiting.
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $MaizeExe
    $psi.Arguments = "`"$mzx`""
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.UseShellExecute = $false
    $proc = [System.Diagnostics.Process]::Start($psi)
    if (-not $proc.WaitForExit(10000)) {
        try { $proc.Kill() } catch { }
        return [pscustomobject]@{ Name = $name; Pass = $false; Expected = $expected; Actual = 'timed out (mis-targeted backward JMP?)' }
    }
    $me = $proc.ExitCode
    $actualRaw = $proc.StandardOutput.ReadToEnd()
    $actual = Trim-TrailingNewlines $actualRaw
    return [pscustomobject]@{ Name = $name; Pass = (($me -eq 0) -and ($actual -eq $expected)); Expected = $expected; Actual = $actual }
}

$results += Invoke-ObjPipelineTest 'obj_dref'         'test_obj_dref.mazm'         'dref: PASS'
$results += Invoke-ObjPipelineTest 'obj_dref_addend'  'test_obj_dref_addend.mazm'  'dref-addend: PASS'
$results += Invoke-ObjPipelineTest 'obj_align'        'test_obj_align.mazm'        'align: PASS'
$results += Invoke-ObjRejectTest   'obj_align_reject' 'test_reject_align.mazm'     'power of two'
$results += Invoke-ObjBackjmpTest

# --- maize-71: EXTERN / PUBLIC declared module interfaces ----------------------------
# --check accepts a fragment that declares EXTERN for its cross-module references
# (the editor must not squiggle a valid fragment) but still errors on an
# UNDECLARED undefined reference. $expectOk = $true means --check must exit 0;
# $false means it must exit nonzero with $expected.
function Invoke-CheckTest($name, $src, $expectOk, $expected) {
    $srcPath = Join-Path $AsmDir $src
    $dst = Join-Path $TestRunDir $src
    Copy-Item -Path $srcPath -Destination $dst -Force
    $log = [System.IO.Path]::GetTempFileName()
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    & $MazmExe --check $dst *> $log
    $ec = $LASTEXITCODE
    $ErrorActionPreference = $prevEap
    $out = Get-Content -Raw -Path $log -ErrorAction SilentlyContinue
    Remove-Item -Force $log -ErrorAction SilentlyContinue
    if ($expectOk) {
        return [pscustomobject]@{ Name = $name; Pass = ($ec -eq 0); Expected = '--check accepts (exit 0)'; Actual = if ($ec -eq 0) { 'accepted' } else { "exit ${ec}: $(Trim-TrailingNewlines $out)" } }
    }
    $rejected = ($ec -ne 0) -and ($null -ne $out) -and ($out -like "*$expected*")
    return [pscustomobject]@{ Name = $name; Pass = $rejected; Expected = "--check rejects with: $expected"; Actual = (Trim-TrailingNewlines $out) }
}

# PUBLIC is a co-equal alias of GLOBAL: two fixtures that differ ONLY in the
# export directive keyword must assemble to byte-identical .mzo objects.
function Invoke-PublicAliasTest {
    $name = 'public_global_identical'
    Emit-Object 'test_export_global.mazm'
    Emit-Object 'test_export_public.mazm'
    $g = Join-Path $TestRunDir 'test_export_global.mzo'
    $p = Join-Path $TestRunDir 'test_export_public.mzo'
    if ((-not (Test-Path $g)) -or (-not (Test-Path $p))) {
        return [pscustomobject]@{ Name = $name; Pass = $false; Expected = 'byte-identical .mzo'; Actual = 'mazm -c produced no .mzo' }
    }
    $gb = [System.IO.File]::ReadAllBytes($g)
    $pb = [System.IO.File]::ReadAllBytes($p)
    $same = ($gb.Length -eq $pb.Length)
    if ($same) {
        for ($i = 0; $i -lt $gb.Length; $i++) { if ($gb[$i] -ne $pb[$i]) { $same = $false; break } }
    }
    return [pscustomobject]@{ Name = $name; Pass = $same; Expected = 'byte-identical .mzo'; Actual = if ($same) { 'identical' } else { 'GLOBAL and PUBLIC .mzo differ' } }
}

$results += Invoke-ObjRejectTest 'obj_undeclared_ref' 'test_reject_undeclared_obj.mazm' "undefined symbol 'mystery'"
$results += Invoke-CheckTest 'check_extern_ok'  'test_check_extern_ok.mazm'  $true  ''
$results += Invoke-CheckTest 'check_undeclared' 'test_check_undeclared.mazm' $false "undefined label 'ghost'"
$results += Invoke-PublicAliasTest

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

# Reserved-opcode resync + round-trip (AC6481, AC7278): two reserved bytes decode
# as DATA $XX / unknown opcode (D-DATA: DB is not a mazm keyword and would break
# round-trip; DATA $XX reassembles to the same byte), advancing exactly one byte
# each, decoding resumes correctly afterward, and mzdis's own output reassembles
# back to the original .mzb byte-for-byte.
# maize-180: mzdis decodes the four new instructions (six encodings $26/$66/$A6 and
# $28/$68). Assemble a code-only fixture, disassemble it, assert every new mnemonic
# decoded (no unknown/malformed lines), then reassemble mzdis's own output and diff the
# .mzb byte-for-byte (the four-surface sync check, same shape as Invoke-MzdisRoundtripTest).
function Invoke-MmuMzdisTest {
    $name = 'mmu_mzdis_roundtrip'
    $srcPath = Join-Path $AsmDir 'test_mmu_mzdis.mazm'
    $asmPath = Join-Path $TestRunDir 'test_mmu_mzdis.mazm'
    Copy-Item -Path $srcPath -Destination $asmPath -Force
    $binPath = [System.IO.Path]::ChangeExtension($asmPath, 'mzb')

    & $MazmExe $asmPath *> $null
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $binPath)) {
        return [pscustomobject]@{ Name = $name; Pass = $false; Expected = 'fixture assembles cleanly'; Actual = 'mazm failed to produce a .mzb' }
    }

    $disPath = Join-Path $TestRunDir 'test_mmu_mzdis.dis.mazm'
    & $MzdisExe -o $disPath $binPath
    $disExit = $LASTEXITCODE
    if ($disExit -ne 0 -or -not (Test-Path $disPath)) {
        return [pscustomobject]@{ Name = $name; Pass = $false; Expected = 'mzdis exits 0 with clean decode'; Actual = "mzdis exit $disExit" }
    }
    $disText = Get-Content -Raw -Path $disPath
    if ($disText -match '(?i)unknown opcode|malformed|TRUNCATED') {
        return [pscustomobject]@{ Name = $name; Pass = $false; Expected = 'no unknown/malformed/truncated lines'; Actual = 'decode diagnostic found in a code-only fixture' }
    }
    if (($disText -notmatch 'MOVTCR R5 \$00') -or ($disText -notmatch 'MOVTCR \$ABCD \$01') -or `
        ($disText -notmatch 'MOVFCR \$02 R6') -or ($disText -notmatch '(?m)^\s+TLBINV\b') -or `
        ($disText -notmatch 'TLBINVA R7')) {
        return [pscustomobject]@{ Name = $name; Pass = $false; Expected = 'all six new encodings decode to their mnemonics'; Actual = 'one or more missing' }
    }

    $reasmBin = [System.IO.Path]::ChangeExtension($disPath, 'mzb')
    & $MazmExe $disPath *> $null
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $reasmBin)) {
        return [pscustomobject]@{ Name = $name; Pass = $false; Expected = "mzdis's own output reassembles cleanly"; Actual = 'mazm failed to reassemble mzdis output' }
    }

    $origBytes = [System.IO.File]::ReadAllBytes($binPath)
    $reasmBytes = [System.IO.File]::ReadAllBytes($reasmBin)
    $identical = ($origBytes.Length -eq $reasmBytes.Length) -and (-not (Compare-Object $origBytes $reasmBytes))
    return [pscustomobject]@{ Name = $name; Pass = $identical; Expected = 'reassembled .mzb byte-identical to original'; Actual = if ($identical) { 'byte-identical' } else { 'content differs' } }
}

# maize-261 profiler smoke: --profile prints a report on stderr at exit; require the
# header plus at least one sample row against the committed hello baseline.
function Invoke-ProfileSmokeTest {
    $name = 'profile_smoke'
    $binPath = Join-Path $AsmDir 'hello.mzb'
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $out = (& $MaizeExe --profile=64 --no-root $binPath 2>&1) | Out-String
    $ErrorActionPreference = $prevEap
    $pass = ($out -match 'profile report \(') -and ($out -match '%\s+\d')
    return [pscustomobject]@{ Name = $name; Pass = $pass; Expected = 'profile report header + at least one sample row'; Actual = if ($pass) { 'report present' } else { ($out -split "`n" | Select-Object -Last 2) -join ' | ' } }
}

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

    $disPath = Join-Path $TestRunDir 'test_mzdis_reserved.dis.mazm'
    & $MzdisExe -o $disPath $binPath
    $disExit = $LASTEXITCODE
    $text = if (Test-Path $disPath) { Get-Content -Raw -Path $disPath } else { '' }

    # Reserved-byte resyncs alone must NOT force exit 1 (spec: "Exit codes").
    $decodePass = ($disExit -eq 0) `
        -and ($text -match [regex]::Escape('DATA $37') + '.*unknown opcode') `
        -and ($text -match [regex]::Escape('DATA $38') + '.*unknown opcode') `
        -and ($text -match '(?m)^\s+NOP\b') `
        -and ($text -notmatch '(?i)TRUNCATED')
    if (-not $decodePass) {
        return [pscustomobject]@{ Name = $name; Pass = $false; Expected = 'DATA $37/$38 unknown-opcode lines, NOP decodes correctly after, exit 0'; Actual = "exit $disExit; see $disPath" }
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

# Symbolic round trip (AC7275, AC7276): a code-only fixture whose in-image
# 32-bit CALL/JMP/Jcc targets (forward and backward) all qualify for label
# synthesis. Asserts symbolization actually fired (fn_/loc_ declaration lines
# AND symbolic operands present -- not a silent fallback to literals), then
# reassembles mzdis's own output and diffs the resulting .mzb against the
# original byte-for-byte, same as Invoke-MzdisRoundtripTest.
function Invoke-MzdisRtSymbolicTest {
    $name = 'mzdis_rt_symbolic'
    $srcPath = Join-Path $AsmDir 'test_mzdis_rt_symbolic.mazm'
    $asmPath = Join-Path $TestRunDir 'test_mzdis_rt_symbolic.mazm'
    Copy-Item -Path $srcPath -Destination $asmPath -Force
    $binPath = [System.IO.Path]::ChangeExtension($asmPath, 'mzb')

    & $MazmExe $asmPath *> $null
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $binPath)) {
        return [pscustomobject]@{ Name = $name; Pass = $false; Expected = 'fixture assembles cleanly'; Actual = 'mazm failed to produce a .mzb' }
    }

    $disPath = Join-Path $TestRunDir 'test_mzdis_rt_symbolic.dis.mazm'
    & $MzdisExe -o $disPath $binPath
    $disExit = $LASTEXITCODE
    if ($disExit -ne 0 -or -not (Test-Path $disPath)) {
        return [pscustomobject]@{ Name = $name; Pass = $false; Expected = 'mzdis exits 0 with clean decode'; Actual = "mzdis exit $disExit" }
    }
    $disText = Get-Content -Raw -Path $disPath
    if ($disText -match '(?i)unknown opcode|malformed|TRUNCATED') {
        return [pscustomobject]@{ Name = $name; Pass = $false; Expected = 'no unknown/malformed/truncated lines (code-only fixture)'; Actual = 'decode diagnostic found' }
    }

    $symbolized = ($disText -match '(?m)^fn_[0-9a-f]+:') `
        -and ($disText -match '(?m)^loc_[0-9a-f]+:') `
        -and ($disText -match 'CALL fn_[0-9a-f]+') `
        -and ($disText -match '(JMP|JNZ) loc_[0-9a-f]+')
    if (-not $symbolized) {
        return [pscustomobject]@{ Name = $name; Pass = $false; Expected = 'synthesized fn_/loc_ declarations AND symbolic operands (symbolization fired)'; Actual = "missing one or more; see $disPath" }
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
    # mzdis writes its rejection diagnostic to stderr on purpose here (exit 1 is
    # expected). Under Windows PowerShell 5.1, ErrorActionPreference = Stop turns
    # that redirected native stderr write into a terminating NativeCommandError even
    # though the 2> redirect still captures it fine; pwsh 7 is unaffected either way.
    # Relax it for exactly this invocation, then restore (same idiom as the mazm and
    # maize calls above).
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    & $MzdisExe $mzoPath > $stdoutFile 2> $stderrFile
    $disExit = $LASTEXITCODE
    $ErrorActionPreference = $prevEap
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
$results += Invoke-MmuMzdisTest
$results += Invoke-MzdisRtSymbolicTest
$results += Invoke-MzdisReservedTest
$results += Invoke-ProfileSmokeTest
$results += Invoke-MzdisTruncatedTest
$results += Invoke-MzdisMzoRejectTest
$results += Invoke-MzdisMzxTest

# --- maize-253: SYS $F6 (sys_ttysize) reports the bound console device's geometry ------
# One fixture (test_ttysize_console.mazm) issues SYS $F6 and prints
# "ttysize: rv=<rv> rows=<ws_row> cols=<ws_col>". The SAME binary is run in two modes so a
# green result in one cannot mask a failure in the other (mirrors run-tests.sh's
# run_ttysize_console_test):
#   1. --console-dump --console-size 100x30: a text_console is bound, so $F6 reports its
#      cell grid. The line lands on the grid, which --console-dump dumps at exit; assert it
#      contains "ttysize: rv=0 rows=30 cols=100".
#   2. plain (no console bound, non-interactive stdio): $F6 returns -ENOTTY (rv=-25) and the
#      pre-zeroed buffer is left untouched.
function Invoke-TtysizeConsoleTest {
    $srcPath = Join-Path $AsmDir 'test_ttysize_console.mazm'
    $asmPath = Join-Path $TestRunDir 'test_ttysize_console.mazm'
    Copy-Item -Path $srcPath -Destination $asmPath -Force
    $binPath = [System.IO.Path]::ChangeExtension($asmPath, 'mzb')

    & $MazmExe $asmPath *> $null
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $binPath)) {
        return [pscustomobject]@{ Name = 'ttysize_console'; Pass = $false; Expected = 'assembles'; Actual = 'mazm failed to assemble' }
    }

    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'

    # Mode 1: console device bound via --console-dump; expect its cell grid on the dump.
    $dumpFile = [System.IO.Path]::GetTempFileName()
    $null | & $MaizeExe --console-dump --console-size 100x30 $binPath > $dumpFile 2> $null
    $me1 = $LASTEXITCODE
    $raw1 = Get-Content -Raw -Path $dumpFile -ErrorAction SilentlyContinue
    Remove-Item -Force $dumpFile -ErrorAction SilentlyContinue
    $pass1 = ($me1 -eq 0) -and ($raw1 -like '*ttysize: rv=0 rows=30 cols=100*')
    $r1 = [pscustomobject]@{
        Name     = 'ttysize_console_console_dump'
        Pass     = $pass1
        Expected = 'grid contains "ttysize: rv=0 rows=30 cols=100"'
        Actual   = if ($pass1) { 'as expected' } else { "exit $me1; $(Trim-TrailingNewlines $raw1)" }
    }

    # Mode 2: no console bound, non-interactive stdio; expect -ENOTTY (rv=-25).
    $outFile = [System.IO.Path]::GetTempFileName()
    $null | & $MaizeExe $binPath > $outFile 2> $null
    $me2 = $LASTEXITCODE
    $raw2 = Get-Content -Raw -Path $outFile -ErrorAction SilentlyContinue
    Remove-Item -Force $outFile -ErrorAction SilentlyContinue
    $actual2 = Trim-TrailingNewlines $raw2
    $expected2 = 'ttysize: rv=-25 rows=0 cols=0'
    $pass2 = ($me2 -eq 0) -and ($actual2 -eq $expected2)
    $r2 = [pscustomobject]@{
        Name     = 'ttysize_console_enotty'
        Pass     = $pass2
        Expected = $expected2
        Actual   = if ($pass2) { $actual2 } else { "exit $me2; $actual2" }
    }

    $ErrorActionPreference = $prevEap
    return $r1, $r2
}

$results += Invoke-TtysizeConsoleTest

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
