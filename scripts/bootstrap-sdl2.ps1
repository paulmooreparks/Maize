#Requires -Version 5.1
<#
.SYNOPSIS
    Fetch and verify the pinned SDL2 mingw development libraries for building the
    maize --display window backend on Windows.

.DESCRIPTION
    Downloads the pinned SDL2 mingw development archive, verifies it against a
    pinned SHA256 checksum, and extracts the x86_64-w64-mingw32 subtree into
    <repo-root>/.toolchains/SDL2/x86_64-w64-mingw32/ (a gitignored, fixed,
    non-versioned directory). No admin rights, no PATH mutation, no installer:
    the script only writes inside the repo's own .toolchains/ directory.

    This provides the SDL2Config / sdl2-config.cmake that CMake's
    find_package(SDL2) resolves (CMakeLists.txt, guarded by MAIZE_DISPLAY=ON) and
    the SDL2.dll that install-mazm.ps1 copies next to maize.exe. Without it the
    -DMAIZE_DISPLAY=ON configure hard-fails (find_package(SDL2 REQUIRED)); this is
    the counterpart to scripts/bootstrap-toolchain.ps1 (llvm-mingw), and
    install-mazm.ps1 auto-invokes it when the SDL2 dir is missing.

    Idempotent: re-running once the pinned version is already present is a no-op.
    Pass -Force to re-fetch regardless.

    SDL2 is zlib-licensed (permissive, compatible with this repo's Apache-2.0).
#>
[CmdletBinding()]
param(
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# --- Pinned constants ------------------------------------------------------------
# SDL2 2.32.x is the final SDL2 (2.x) series. Bump Version + Sha256 together to move
# the pin; recompute the hash with Get-FileHash on the downloaded asset.
$Version = '2.32.8'
$Asset   = "SDL2-devel-$Version-mingw.zip"
$Url     = "https://github.com/libsdl-org/SDL/releases/download/release-$Version/$Asset"
$Sha256  = '2f0a74c2eb3f7ffb26aeefce733ce75f5a57881adf3fab92b2430805ff7249e2'

# --- Paths resolved relative to THIS script, not the caller's CWD ----------------
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot  = Resolve-Path (Join-Path $ScriptDir '..')
$Dest      = Join-Path $RepoRoot '.toolchains/SDL2'
$ArchDest  = Join-Path $Dest 'x86_64-w64-mingw32'
$Stamp     = Join-Path $Dest '.bootstrap-version'
$CmakeCfg  = Join-Path $ArchDest 'lib/cmake/SDL2/sdl2-config.cmake'
$Dll       = Join-Path $ArchDest 'bin/SDL2.dll'

# --- Idempotency check -----------------------------------------------------------
if (-not $Force -and (Test-Path $Stamp) -and (Test-Path $CmakeCfg) -and (Test-Path $Dll)) {
    $existing = (Get-Content -Raw -Path $Stamp).Trim()
    if ($existing -eq $Version) {
        Write-Host "SDL2 $Version already up to date at $Dest"
        Write-Host "  cmake config: $CmakeCfg"
        Write-Host "  runtime dll:  $Dll"
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
if ($actual.ToLowerInvariant() -ne $Sha256.ToLowerInvariant()) {
    Remove-Item -Force $TmpZip
    Write-Error "Checksum mismatch for $Asset`n  expected: $Sha256`n  actual:   $actual`nRefusing to extract unverified content."
    exit 1
}
Write-Host "  OK: $actual"

# --- Extract, keeping only the x86_64-w64-mingw32 subtree -------------------------
# The archive contains SDL2-<Version>/{i686-w64-mingw32,x86_64-w64-mingw32,cmake,...}.
# The maize build targets x86_64 only, so vendor just that subtree.
$TmpExtract = Join-Path ([System.IO.Path]::GetTempPath()) ("maize-sdl2-" + [System.Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $TmpExtract | Out-Null
try {
    Write-Host "Extracting ..."
    Expand-Archive -Path $TmpZip -DestinationPath $TmpExtract -Force

    $inner = Join-Path $TmpExtract "SDL2-$Version/x86_64-w64-mingw32"
    if (-not (Test-Path $inner)) {
        Write-Error "Extraction completed but $inner is missing; the archive layout may have changed."
        exit 1
    }
    New-Item -ItemType Directory -Force $Dest | Out-Null
    Move-Item -Path $inner -Destination $ArchDest
}
finally {
    if (Test-Path $TmpExtract) { Remove-Item -Recurse -Force $TmpExtract }
    if (Test-Path $TmpZip)     { Remove-Item -Force $TmpZip }
}

# --- Stamp the version -----------------------------------------------------------
Set-Content -Path $Stamp -Value $Version -NoNewline

if (-not (Test-Path $CmakeCfg) -or -not (Test-Path $Dll)) {
    Write-Error "Extraction completed but expected files are missing ($CmakeCfg / $Dll)."
    exit 1
}

Write-Host ""
Write-Host "SDL2 $Version installed at $Dest"
Write-Host "  cmake config: $CmakeCfg"
Write-Host "  runtime dll:  $Dll"
Write-Host ""
Write-Host "Next: scripts/install-mazm.ps1  (or Ctrl+Shift+B) builds maize with --display."
exit 0
