#Requires -Version 5.1
<#
.SYNOPSIS
    Fetch and verify the pinned llvm-mingw toolchain for building Maize on Windows.

.DESCRIPTION
    Downloads the pinned llvm-mingw release archive, verifies it against a pinned
    SHA256 checksum, and extracts it into the per-user, version-keyed toolchain
    location (maize-439):

        %LOCALAPPDATA%\Maize\toolchains\llvm-mingw\<pinned-version>\

    or, when MAIZE_TOOLCHAIN_ROOT is set, <that root>\llvm-mingw\<pinned-version>\.
    No admin rights, no PATH mutation, no registry writes, no installer, and
    nothing is ever written inside the repository. The version and checksum come
    from scripts/toolchain-pins/llvm-mingw.pin, which every resolver reads.

    Installing outside the repository is the point of maize-439. A gitignored
    directory at the repo root is invisible from every worktree, which is why
    agents started linking one back in, and a recursive delete that followed such
    a link emptied the operator's compiler on 2026-08-12. The shared location
    makes a worktree build with no link and no special handling.

    Keying the install path on the version means a pin bump installs ALONGSIDE its
    predecessor rather than over it, so a rollback is free and two branches on
    different pins coexist without re-downloading.

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

# --- Paths resolved relative to THIS script, not the caller's CWD ----------------
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path $ScriptDir 'lib/ToolchainRoot.ps1')

# --- Pinned constants, read from the shared pin file -----------------------------
# The pin lives at scripts/toolchain-pins/llvm-mingw.pin because CMakePresets.json
# cannot execute a PowerShell library and needs the same version this script installs
# (decision D-2). The provenance comment that used to sit here moved there with the
# values. The asset name is derived from the version rather than pinned separately,
# so a bump stays a one-file, two-line edit.
$Version  = Get-MaizePinnedVersion -Tool 'llvm-mingw'
$Sha256   = Get-MaizePinnedSha256  -Tool 'llvm-mingw'
$Asset    = "llvm-mingw-$Version-ucrt-x86_64.zip"
$Url      = "https://github.com/mstorsjo/llvm-mingw/releases/download/$Version/$Asset"

$Dest      = Get-MaizeToolchainInstallDir -Tool 'llvm-mingw'
# The marker is written LAST, after the probe files are in place, so a run
# interrupted mid-extraction leaves a versioned directory that does not claim to be
# complete. The version itself is now part of $Dest, so the marker no longer needs to
# carry it and two versions can never collide in one directory.
$Marker    = Join-Path $Dest '.bootstrap-complete'
$Clangxx   = Join-Path $Dest 'bin/x86_64-w64-mingw32-clang++.exe'
$ClangC    = Join-Path $Dest 'bin/x86_64-w64-mingw32-clang.exe'

# --- Idempotency check -----------------------------------------------------------
if (-not $Force -and (Test-Path $Marker) -and (Test-Path $Clangxx)) {
    Write-Host "llvm-mingw $Version already up to date at $Dest"
    Write-Host "  C compiler:   $ClangC"
    Write-Host "  C++ compiler: $Clangxx"
    exit 0
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
    # -ErrorAction Continue so the explicit exit code below is the one the caller
    # sees: under the script-level EAP=Stop a plain Write-Error is a TERMINATING
    # error and `exit 1` never runs (Convention counterexamples, Entry 10).
    Write-Error "Checksum mismatch for $Asset`n  expected: $Sha256`n  actual:   $actual`nRefusing to extract unverified content." -ErrorAction Continue
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

if (-not (Test-Path $Clangxx)) {
    Write-Error "Extraction completed but $Clangxx is missing; the archive layout may have changed." -ErrorAction Continue
    exit 1
}

# --- Mark the install complete, last ----------------------------------------------
Set-Content -Path $Marker -Value $Version -NoNewline

Write-Host ""
Write-Host "llvm-mingw $Version installed at $Dest"
Write-Host "  C compiler:   $ClangC"
Write-Host "  C++ compiler: $Clangxx"
Write-Host ""
Write-Host "Next: cmake --preset windows-llvm-mingw-debug"
exit 0
