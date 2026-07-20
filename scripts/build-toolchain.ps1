#Requires -Version 5.1

<#
.SYNOPSIS
    Build the vendored C cross-toolchain (cproc + QBE) that the guest-world builds need.

.DESCRIPTION
    Builds the C compiler and code generator (cproc and QBE with the Maize target) that
    the quesOS, userland, and demo builds compile through. The build runs natively via
    Git Bash (maize-258; the cproc/QBE C toolchain compiles on Windows through the
    vendored llvm-mingw toolchain, no WSL involved, maize-257), but you drive it
    entirely from PowerShell.

    Takes no arguments. The first run may take a while (a cold compiler build); later
    runs are faster once the result is cached. On success the tools are left in the repo
    where the other build scripts already look for them, so nothing needs installing.

    Requires Git for Windows (ships Git Bash) with the toolchain source submodules
    initialized (git submodule update --init --recursive). If Git Bash is not found the
    script stops with a clear message.

.EXAMPLE
    .\scripts\build-toolchain.ps1
    Builds cproc and QBE so the other guest-world build scripts can compile C.
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$RepoRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'lib\gitbash.ps1')

# Git Bash is required: this was the last shipped .ps1 still trampolining through WSL.
# maize-258 repoints it to Git Bash (mzcc.cmd's already-shipped pattern,
# install-mazm.ps1:250-292), same as build-quesos.ps1 / build-userland.ps1 /
# build-demos.ps1. Fail fast with a clear message rather than a raw .NET exception when
# bash.exe cannot be found.
$BashExe = Resolve-GitBash
if (-not $BashExe) {
    # -ErrorAction Continue: Set-StrictMode/EAP='Stop' would otherwise make Write-Error
    # terminating, so the process would die on the throw (exit 1) before reaching exit 2.
    Write-Error 'Git Bash (bash.exe) not found; install Git for Windows (ships Git Bash) to build the C toolchain.' -ErrorAction Continue
    exit 2
}

# No path translation is needed: mzcc.cmd already proves a forward-slashed absolute
# Windows path (C:/Users/.../repo) resolves natively under Git Bash / MSYS without
# cygpath, because MSYS bash understands drive-letter paths directly.
$repoPosix = ($RepoRoot -replace '\\', '/')

# One bash statement, nothing chained after it, so $LASTEXITCODE is exactly the build
# script's own exit code. No arguments: build-toolchain.sh takes none.
$cmd = "'$repoPosix/scripts/build-toolchain.sh'"

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
    Write-Host 'C toolchain (cproc + QBE) built.'
}
exit $code
