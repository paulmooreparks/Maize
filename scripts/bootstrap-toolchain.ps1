#Requires -Version 5.1
<#
.SYNOPSIS
    Fetch and verify the pinned llvm-mingw toolchain for building Maize on Windows.

.DESCRIPTION
    Downloads the pinned llvm-mingw release archive, verifies it against a pinned
    SHA256 checksum, and extracts it into <repo-root>/.toolchains/llvm-mingw/ (a
    gitignored, fixed, non-versioned directory). No admin rights, no PATH mutation,
    no registry writes, no installer: the script only writes inside the repo's own
    .toolchains/ directory.

    Idempotent: re-running once the pinned version is already present is a no-op.
    Pass -Force to re-fetch regardless.

    This toolchain has no Microsoft Visual C++ Redistributable runtime dependency;
    clang++.exe imports only OS-native api-ms-win-crt-*.dll UCRT apisets plus the
    toolchain's own bundled DLLs (no vcruntime140.dll / msvcp140.dll).
#>
[CmdletBinding()]
param(
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# --- Pinned constants (see resolved open question 5943 on card maize-32) ---------
$Version  = '20260616'
$Asset    = 'llvm-mingw-20260616-ucrt-x86_64.zip'
$Url      = "https://github.com/mstorsjo/llvm-mingw/releases/download/$Version/$Asset"
$Sha256   = 'b9b68a4d276e16fa25802aaba458e4638f64b3884c290aaccdc2d87083b6ca35'

# --- Paths resolved relative to THIS script, not the caller's CWD ----------------
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot  = Resolve-Path (Join-Path $ScriptDir '..')
$Dest      = Join-Path $RepoRoot '.toolchains/llvm-mingw'
$Stamp     = Join-Path $Dest '.bootstrap-version'
$Clangxx   = Join-Path $Dest 'bin/x86_64-w64-mingw32-clang++.exe'
$ClangC    = Join-Path $Dest 'bin/x86_64-w64-mingw32-clang.exe'

# --- Idempotency check -----------------------------------------------------------
if (-not $Force -and (Test-Path $Stamp) -and (Test-Path $Clangxx)) {
    $existing = (Get-Content -Raw -Path $Stamp).Trim()
    if ($existing -eq $Version) {
        Write-Host "llvm-mingw $Version already up to date at $Dest"
        Write-Host "  C compiler:   $ClangC"
        Write-Host "  C++ compiler: $Clangxx"
        exit 0
    }
}

# --- Remove any stale/partial destination ----------------------------------------
if (Test-Path $Dest) {
    Write-Host "Removing existing $Dest ..."
    Remove-Item -Recurse -Force $Dest
}

# --- Download to a temp file -----------------------------------------------------
$TmpZip = Join-Path ([System.IO.Path]::GetTempPath()) ("maize-" + $Asset)
if (Test-Path $TmpZip) { Remove-Item -Force $TmpZip }

Write-Host "Downloading $Url ..."
$oldProgress = $ProgressPreference
$ProgressPreference = 'SilentlyContinue'   # Invoke-WebRequest is far faster with the progress bar off
try {
    Invoke-WebRequest -Uri $Url -OutFile $TmpZip -UseBasicParsing
}
finally {
    $ProgressPreference = $oldProgress
}

# --- Verify SHA256 before extracting ---------------------------------------------
Write-Host "Verifying SHA256 ..."
$actual = (Get-FileHash -Algorithm SHA256 -Path $TmpZip).Hash
if ($actual -ne $Sha256.ToUpperInvariant() -and $actual.ToLowerInvariant() -ne $Sha256.ToLowerInvariant()) {
    Remove-Item -Force $TmpZip
    Write-Error "Checksum mismatch for $Asset`n  expected: $Sha256`n  actual:   $actual`nRefusing to extract unverified content."
    exit 1
}
Write-Host "  OK: $actual"

# --- Extract, stripping the archive's top-level directory ------------------------
$TmpExtract = Join-Path ([System.IO.Path]::GetTempPath()) ("maize-extract-" + [System.Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $TmpExtract | Out-Null
try {
    Write-Host "Extracting ..."
    Expand-Archive -Path $TmpZip -DestinationPath $TmpExtract -Force

    # The archive contains a single top-level directory (llvm-mingw-<tag>-ucrt-x86_64/).
    # Strip it so bin/, lib/, etc. land directly under $Dest.
    $top = @(Get-ChildItem -Path $TmpExtract)
    if ($top.Count -eq 1 -and $top[0].PSIsContainer) {
        $inner = $top[0].FullName
    } else {
        $inner = $TmpExtract
    }

    $parent = Split-Path -Parent $Dest
    if (-not (Test-Path $parent)) { New-Item -ItemType Directory -Path $parent | Out-Null }
    Move-Item -Path $inner -Destination $Dest
}
finally {
    if (Test-Path $TmpExtract) { Remove-Item -Recurse -Force $TmpExtract }
    if (Test-Path $TmpZip)     { Remove-Item -Force $TmpZip }
}

# --- Stamp the version -----------------------------------------------------------
Set-Content -Path $Stamp -Value $Version -NoNewline

if (-not (Test-Path $Clangxx)) {
    Write-Error "Extraction completed but $Clangxx is missing; the archive layout may have changed."
    exit 1
}

Write-Host ""
Write-Host "llvm-mingw $Version installed at $Dest"
Write-Host "  C compiler:   $ClangC"
Write-Host "  C++ compiler: $Clangxx"
Write-Host ""
Write-Host "Next: cmake --preset windows-llvm-mingw-debug"
exit 0
