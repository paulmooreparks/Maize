#Requires -Version 5.1

<#
.SYNOPSIS
    Build the vendored userland programs (the wave-1 /bin set) into Maize images.

.DESCRIPTION
    Compiles the vendored userland (the sbase coreutils plus the oksh shell) to Maize
    images, one per program, using the Maize C toolchain. The compile runs natively via
    Git Bash (maize-258; the cproc/QBE C toolchain is POSIX-only but runs under the
    vendored llvm-mingw toolchain through Git Bash, no WSL involved), but you drive it
    entirely from PowerShell.

    With no program names, the full default set is built. Name one or more programs to
    build just those. By default the images are staged into %USERPROFILE%\.maize\root\bin,
    which is the guest's /bin under Maize's default sandbox root, so the programs appear
    at /bin when you run maize without a custom root.

    Requires Git for Windows (ships Git Bash) with the Maize C cross-toolchain
    provisioned (run scripts\install-mazm.ps1 once to set that up). If Git Bash is not
    found the script stops with a clear message.

.PARAMETER Preset
    Build preset to compile against. When omitted, the underlying build picks the
    default preset for the platform.

.PARAMETER Out
    Directory to stage the built images into. Defaults to %USERPROFILE%\.maize\root\bin
    (the guest /bin under the default sandbox root).

.PARAMETER Prog
    Zero or more program names to build (for example: ls cat oksh). With none named, the
    full default userland set is built.

.EXAMPLE
    .\scripts\build-userland.ps1
    Builds the full default set into %USERPROFILE%\.maize\root\bin.

.EXAMPLE
    .\scripts\build-userland.ps1 -Out C:\tmp\bin true false ls
    Builds just true, false, and ls into the given directory.
#>
# PositionalBinding=$false: -Preset and -Out are named-only, so bare positional args
# (the program names) all flow to -Prog rather than the first one binding to -Preset.
[CmdletBinding(PositionalBinding = $false)]
param(
    [string]$Preset = '',
    [string]$Out = '',
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$Prog = @()
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$RepoRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'lib\gitbash.ps1')

# Track whether the caller overrode -Out: the default-destination reminder and the
# missing-root warning below only apply when the default guest-/bin staging is in effect.
$OutOverridden = ($Out -ne '')
if ($OutOverridden) {
    $OutDir = $Out
}
else {
    $OutDir = Join-Path $HOME '.maize\root\bin'
}
if (-not [System.IO.Path]::IsPathRooted($OutDir)) {
    $OutDir = Join-Path (Get-Location).Path $OutDir
}
$OutDir = [System.IO.Path]::GetFullPath($OutDir)

# Snapshot whether the default sandbox root exists BEFORE the build. The build creates
# <root>\bin (and thus <root>) via mkdir -p, so a post-build check would always find it;
# capturing the first-run case up front is what lets the OQ-9246 warning fire. Only
# meaningful when the default -Out is in effect.
$RootDir = Join-Path $HOME '.maize\root'
$RootMissingBefore = (-not $OutOverridden) -and (-not (Test-Path $RootDir))

# Git Bash is required: the cproc/QBE C toolchain is POSIX-only. maize-258 repoints
# this forwarder from WSL to Git Bash (mzcc.cmd's already-shipped pattern,
# install-mazm.ps1:250-292): the WSL-native-mirror the underlying .sh script applies
# on a /mnt/* repo excludes /build, so cc-maize.sh could not find mazm inside the
# mirror (maize-265's blast radius). A Git-Bash-resolved repo root is never /mnt/*,
# so the mirror guard never engages. Fail fast with a clear message rather than a
# raw .NET exception when bash.exe cannot be found.
$BashExe = Resolve-GitBash
if (-not $BashExe) {
    # -ErrorAction Continue: Set-StrictMode/EAP='Stop' would otherwise make Write-Error
    # terminating, so the process would die on the throw (exit 1) before reaching exit 2.
    Write-Error 'Git Bash (bash.exe) not found; install Git for Windows (ships Git Bash) to build the userland (the C toolchain is POSIX-only).' -ErrorAction Continue
    exit 2
}

# No path translation is needed: mzcc.cmd already proves a forward-slashed absolute
# Windows path (C:/Users/.../repo) resolves natively under Git Bash / MSYS without
# cygpath, because MSYS bash understands drive-letter paths directly.
$repoPosix = ($RepoRoot -replace '\\', '/')
$outPosix = ($OutDir -replace '\\', '/')

# One bash statement, nothing chained after it, so $LASTEXITCODE is exactly the build
# script's own exit code. The program names (if any) forward through as trailing
# positional args; with none, build-userland.sh applies its own default set.
$cmd = "'$repoPosix/userland/build-userland.sh'"
if ($Preset -ne '') {
    $cmd += " --preset '$Preset'"
}
$cmd += " --out '$outPosix'"
foreach ($p in $Prog) {
    $cmd += " '$p'"
}

# Windows PowerShell 5.1 turns any native-command stderr into a terminating
# NativeCommandError while ErrorActionPreference is 'Stop'; the build writes progress to
# stderr, so relax it for exactly the Git Bash call and restore it after (pwsh 7 is
# unaffected either way).
#
# INTERIM DEPENDENCY NOTE (maize-266 tracks the end state): Git Bash ships with Git
# for Windows itself, so this call adds no dependency beyond Git plus what the repo
# vendors, but calling out to bash at all is a tolerated interim shape, not the
# target; maize-266 designs that dependency out.
$prevEap = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
& $BashExe -lc $cmd
$code = $LASTEXITCODE
$ErrorActionPreference = $prevEap

if ($code -eq 0) {
    Write-Host "Staged userland images in $OutDir"
    if (-not $OutOverridden) {
        Write-Host 'These appear at guest /bin when you run maize with the default sandbox root.'
        if ($RootMissingBefore) {
            Write-Warning "The default sandbox root $RootDir did not exist, so the guest /bin is not set up yet. Run ``maize`` once with the default sandbox root (no --no-root) to auto-create it, or create the directory yourself."
        }
    }
}
exit $code
