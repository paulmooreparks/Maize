#Requires -Version 5.1
# scripts/lib/gitbash.ps1 (maize-258 Decision 3): shared PowerShell helper, dot-sourced
# by scripts/build-quesos.ps1, scripts/build-userland.ps1, scripts/build-demos.ps1 and
# scripts/build-toolchain.ps1. maize-450 dropped the last non-v1 consumer when
# install-mzasm.ps1 stopped building the C cross-toolchain, so every caller today is a v1
# builder that runs only on the `v1` branch. This is the SOLE definition site for
# Resolve-GitBash; do not
# copy the function body into another script, dot-source this file instead
# (scripts/lib/harness-env.sh is the sh-side precedent for exactly this shape).
#
# This file defines a function and sets no top-level state beyond the house
# EAP/StrictMode hardening below; it must be dot-sourced (". <path>\lib\gitbash.ps1"),
# never executed directly.

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# Resolve Git Bash (maize-257): the v1 guest builders and the C cross-toolchain build all
# need bash.exe, not WSL. `git --exec-path` gives the mingw64/libexec/
# git-core dir inside the Git install; bash.exe sits at <gitroot>/bin/bash.exe three
# levels up. Standard install locations are the fallback. Returns $null (never
# throws) so callers can warn-and-skip.
function Resolve-GitBash {
    # Get-Command -ErrorAction SilentlyContinue is a non-terminating probe regardless
    # of the script's EAP=Stop; invoking `git` directly when it is not on PATH throws
    # a terminating CommandNotFoundException before $LASTEXITCODE is ever set, which
    # 2>$null cannot catch (see the Convention counterexamples doc, Entry 12).
    $gitCmd = Get-Command git -ErrorAction SilentlyContinue
    if ($gitCmd) {
        $execPath = (git --exec-path 2>$null)
        if ($LASTEXITCODE -eq 0 -and $execPath) {
            $gitRoot = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $execPath))
            $candidate = Join-Path $gitRoot 'bin\bash.exe'
            if (Test-Path $candidate) { return $candidate }
        }
    }
    $candidates = @((Join-Path $env:ProgramFiles 'Git\bin\bash.exe'))
    if (Test-Path 'env:ProgramFiles(x86)') {
        $candidates += (Join-Path ${env:ProgramFiles(x86)} 'Git\bin\bash.exe')
    }
    foreach ($p in $candidates) {
        if ($p -and (Test-Path $p)) { return $p }
    }
    return $null
}
