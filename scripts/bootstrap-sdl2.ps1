#Requires -Version 5.1
<#
.SYNOPSIS
    Fetch and verify the pinned SDL2 mingw development libraries for the Windows
    window backend.

    maize-450: no binary links SDL2 at the moment. v1's maizeg carried the window
    backend and is archived, and mzvmg has no display device to attach a window to
    until maize-456 lands. The fetch stays wired into install-mzasm.ps1 so the pinned
    SDL2 is present and verified on the machine when that port arrives, and so the
    SDL2.dll beside mzvmg.exe is the right one rather than whatever a later scramble
    produced.

.DESCRIPTION
    Downloads the pinned SDL2 mingw development archive, verifies it against a
    pinned SHA256 checksum, and extracts the x86_64-w64-mingw32 subtree into the
    per-user, version-keyed toolchain location (maize-439):

        %LOCALAPPDATA%\Maize\toolchains\sdl2\<pinned-version>\x86_64-w64-mingw32\

    or, when MAIZE_TOOLCHAIN_ROOT is set, under that root instead. No admin
    rights, no PATH mutation, no installer, and nothing is ever written inside the
    repository. The version and checksum come from
    scripts/toolchain-pins/sdl2.pin, which every resolver reads.

    The tool-name segment is lowercase 'sdl2' in the per-user layout, for
    filesystem-name consistency with 'llvm-mingw'. The in-repo fallback keeps its
    historical uppercase '.toolchains/SDL2/' name, because an existing checkout is
    not being renamed; scripts/lib/ToolchainRoot.ps1 holds both spellings.

    This provides the SDL2Config / sdl2-config.cmake that CMake's
    find_package(SDL2) resolves (CMakeLists.txt, guarded by MAIZE_DISPLAY=ON) and
    the SDL2.dll that install-mzasm.ps1 copies next to mzvmg.exe. Without it the
    -DMAIZE_DISPLAY=ON configure hard-fails (find_package(SDL2 REQUIRED)); this is
    the counterpart to scripts/bootstrap-toolchain.ps1 (llvm-mingw), and
    install-mzasm.ps1 auto-invokes it when the SDL2 dir is missing.

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

# --- Paths resolved relative to THIS script, not the caller's CWD ----------------
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path $ScriptDir 'lib/ToolchainRoot.ps1')

# --- Pinned constants, read from the shared pin file -----------------------------
# The pin lives at scripts/toolchain-pins/sdl2.pin, alongside llvm-mingw's, so a bump
# is a one-file two-line edit and every resolver reads the same value (decision D-2).
# The asset name is derived from the version rather than pinned separately.
$Version = Get-MaizePinnedVersion -Tool 'sdl2'
$Sha256  = Get-MaizePinnedSha256  -Tool 'sdl2'
$Asset   = "SDL2-devel-$Version-mingw.zip"
$Url     = "https://github.com/libsdl-org/SDL/releases/download/release-$Version/$Asset"

# The install dir already carries the arch leaf, matching the layout the in-repo
# fallback has always had, so the probe path below is the same under either.
$ArchDest  = Get-MaizeToolchainInstallDir -Tool 'sdl2'
$Dest      = Split-Path -Parent $ArchDest
# Written LAST, after the probe files are in place, so a run interrupted mid-extraction
# leaves a versioned directory that does not claim to be complete. The version is part
# of the path now, so the marker no longer needs to carry it.
$Marker    = Join-Path $Dest '.bootstrap-complete'
$CmakeCfg  = Join-Path $ArchDest 'lib/cmake/SDL2/sdl2-config.cmake'
$Dll       = Join-Path $ArchDest 'bin/SDL2.dll'

# --- Idempotency check -----------------------------------------------------------
if (-not $Force -and (Test-Path $Marker) -and (Test-Path $CmakeCfg) -and (Test-Path $Dll)) {
    Write-Host "SDL2 $Version already up to date at $Dest"
    Write-Host "  cmake config: $CmakeCfg"
    Write-Host "  runtime dll:  $Dll"
    exit 0
}

# --- Remove any stale/partial destination ----------------------------------------
# $Dest is the versioned directory, so this only ever removes an install of THIS
# pinned version; a different version lives in its own directory and is left alone.
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
        # -ErrorAction Continue so the explicit exit code below is the one the caller
        # sees: under the script-level EAP=Stop a plain Write-Error is a TERMINATING
        # error and `exit 1` never runs (Convention counterexamples, Entry 10).
        Write-Error "Extraction completed but $inner is missing; the archive layout may have changed." -ErrorAction Continue
        exit 1
    }
    New-Item -ItemType Directory -Force $Dest | Out-Null
    Move-Item -Path $inner -Destination $ArchDest
}
finally {
    if (Test-Path $TmpExtract) { Remove-Item -Recurse -Force $TmpExtract }
    if (Test-Path $TmpZip)     { Remove-Item -Force $TmpZip }
}

if (-not (Test-Path $CmakeCfg) -or -not (Test-Path $Dll)) {
    Write-Error "Extraction completed but expected files are missing ($CmakeCfg / $Dll)." -ErrorAction Continue
    exit 1
}

# --- Mark the install complete, last ----------------------------------------------
Set-Content -Path $Marker -Value $Version -NoNewline

Write-Host ""
Write-Host "SDL2 $Version installed at $Dest"
Write-Host "  cmake config: $CmakeCfg"
Write-Host "  runtime dll:  $Dll"
Write-Host ""
Write-Host "Next: scripts/install-mzasm.ps1 (or Ctrl+Shift+B) installs SDL2.dll beside mzvmg.exe. No binary links it until the display device lands (maize-456)."
exit 0
