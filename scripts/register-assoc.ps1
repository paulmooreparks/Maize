<#
.SYNOPSIS
    Register (or unregister) .mzb / .mzx file associations so Maize images run
    directly from Explorer or a prompt, via maize.exe.

.DESCRIPTION
    maize-67. DOCUMENTED, user-run glue. This is NOT wired into the build or the
    install task; it mutates OS state and must be run (and reversed) by a human.

    All changes are made under HKCU (HKEY_CURRENT_USER), so NO administrator
    rights are needed and nothing machine-wide is touched. .mzb and .mzx are new
    extensions introduced by Maize (maize-65), so this overrides no existing
    association.

    A single ProgID 'Maize.Image' is created with an open command of:

        "<maize.exe>" "%1"

    so double-clicking hello.mzb in Explorer, or typing its path at a prompt,
    launches it through maize.exe. maize.exe receives the image path as argv[1],
    which is exactly what it expects; extra arguments are tolerated (they are not
    yet delivered to the guest program -- see maize-60).

    Both .mzb (flat) and .mzx (linked) point at the same ProgID because the maize
    loader dispatches on header magic internally (MZX magic -> linked load; else
    flat load-at-0), so one handler covers both formats.

    PATHEXT: to run 'prog' and have the shell find 'prog.mzb'/'prog.mzx', the
    extensions must be listed in PATHEXT. By default this script only PRINTS
    guidance. Pass -UpdatePathext to append .MZB;.MZX to your USER PATHEXT
    (reversible: unregister strips them again and restores the original state).

.PARAMETER Action
    register (default) | unregister | status

.PARAMETER MaizeExe
    Path to maize.exe. Defaults to 'maize' resolved on PATH.

.PARAMETER UpdatePathext
    Also add .MZB;.MZX to the user PATHEXT (reversible). Register only.

.EXAMPLE
    pwsh ./scripts/register-assoc.ps1 register
.EXAMPLE
    pwsh ./scripts/register-assoc.ps1 register -UpdatePathext
.EXAMPLE
    pwsh ./scripts/register-assoc.ps1 unregister
.EXAMPLE
    pwsh ./scripts/register-assoc.ps1 status
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [ValidateSet('register', 'unregister', 'status')]
    [string]$Action = 'register',

    [string]$MaizeExe,

    [switch]$UpdatePathext
)

$ErrorActionPreference = 'Stop'

$ProgId       = 'Maize.Image'
$Extensions   = @('.mzb', '.mzx')
$PathextAdds  = @('.MZB', '.MZX')
$ClassesRoot  = 'HKCU:\Software\Classes'
$ProgIdKey    = Join-Path $ClassesRoot $ProgId

function Resolve-MaizeExe {
    param([string]$Explicit)
    if ($Explicit) {
        if (-not (Test-Path -LiteralPath $Explicit)) {
            throw "maize.exe not found at '$Explicit'."
        }
        return (Resolve-Path -LiteralPath $Explicit).Path
    }
    $cmd = Get-Command 'maize' -ErrorAction SilentlyContinue
    if ($null -eq $cmd) {
        $cmd = Get-Command 'maize.exe' -ErrorAction SilentlyContinue
    }
    if ($null -eq $cmd) {
        throw "could not find 'maize' on PATH. Pass -MaizeExe C:\path\to\maize.exe."
    }
    return $cmd.Source
}

# Ask Explorer to re-read associations (SHChangeNotify / SHCNE_ASSOCCHANGED).
function Send-AssocChangeNotification {
    try {
        if (-not ('Maize.Shell32' -as [type])) {
            Add-Type -Namespace 'Maize' -Name 'Shell32' -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("shell32.dll")]
public static extern void SHChangeNotify(int eventId, int flags, System.IntPtr item1, System.IntPtr item2);
'@ | Out-Null
        }
        # SHCNE_ASSOCCHANGED = 0x08000000, SHCNF_IDLIST = 0x0000
        [Maize.Shell32]::SHChangeNotify(0x08000000, 0x0000, [IntPtr]::Zero, [IntPtr]::Zero)
    } catch {
        Write-Verbose "SHChangeNotify unavailable: $($_.Exception.Message)"
    }
}

function Invoke-Register {
    $exe = Resolve-MaizeExe -Explicit $MaizeExe
    Write-Host "maize.exe: $exe"

    # ProgID + open command. New-Item -Force is idempotent (overwrites cleanly).
    $cmdKey = Join-Path $ProgIdKey 'shell\open\command'
    New-Item -Path $ProgIdKey -Force | Out-Null
    Set-ItemProperty -Path $ProgIdKey -Name '(default)' -Value 'Maize executable image'
    New-Item -Path (Join-Path $ProgIdKey 'DefaultIcon') -Force | Out-Null
    Set-ItemProperty -Path (Join-Path $ProgIdKey 'DefaultIcon') -Name '(default)' -Value ('"{0}",0' -f $exe)
    New-Item -Path $cmdKey -Force | Out-Null
    Set-ItemProperty -Path $cmdKey -Name '(default)' -Value ('"{0}" "%1"' -f $exe)

    foreach ($ext in $Extensions) {
        $extKey = Join-Path $ClassesRoot $ext
        New-Item -Path $extKey -Force | Out-Null
        Set-ItemProperty -Path $extKey -Name '(default)' -Value $ProgId
        Write-Host "associated $ext -> $ProgId"
    }

    if ($UpdatePathext) {
        Update-Pathext -Add
    } else {
        Write-Host ''
        Write-Host 'PATHEXT guidance (optional): to run "prog" and have the shell find'
        Write-Host 'prog.mzb / prog.mzx, add .MZB;.MZX to PATHEXT. Re-run with -UpdatePathext'
        Write-Host 'to do this to your user PATHEXT automatically (reversible), or add it by hand.'
    }

    Send-AssocChangeNotification
    Write-Host ''
    Write-Host 'done. Double-click a .mzb/.mzx in Explorer, or run its path at a prompt.'
    Write-Host 'A new shell may be needed to pick up PATHEXT / association changes.'
}

function Invoke-Unregister {
    foreach ($ext in $Extensions) {
        $extKey = Join-Path $ClassesRoot $ext
        if (Test-Path -LiteralPath $extKey) {
            Remove-Item -LiteralPath $extKey -Recurse -Force
            Write-Host "removed association $ext"
        } else {
            Write-Host "$ext not associated (nothing to do)"
        }
    }
    if (Test-Path -LiteralPath $ProgIdKey) {
        Remove-Item -LiteralPath $ProgIdKey -Recurse -Force
        Write-Host "removed ProgID $ProgId"
    } else {
        Write-Host "ProgID $ProgId not present (nothing to do)"
    }

    # Always clean PATHEXT of our tokens so the machine is fully restored,
    # regardless of whether -UpdatePathext was used at register time.
    Update-Pathext -Remove

    Send-AssocChangeNotification
    Write-Host 'done. Machine restored.'
}

function Invoke-Status {
    foreach ($ext in $Extensions) {
        $extKey = Join-Path $ClassesRoot $ext
        if (Test-Path -LiteralPath $extKey) {
            $val = (Get-ItemProperty -LiteralPath $extKey).'(default)'
            Write-Host "$ext -> $val"
        } else {
            Write-Host "$ext -> (not associated)"
        }
    }
    if (Test-Path -LiteralPath (Join-Path $ProgIdKey 'shell\open\command')) {
        $cmd = (Get-ItemProperty -LiteralPath (Join-Path $ProgIdKey 'shell\open\command')).'(default)'
        Write-Host "$ProgId open command: $cmd"
    } else {
        Write-Host "${ProgId}: (not present)"
    }
    $userPathext = [Environment]::GetEnvironmentVariable('PATHEXT', 'User')
    Write-Host "user PATHEXT: $(if ($userPathext) { $userPathext } else { '(unset; inherits machine PATHEXT)' })"
}

function Update-Pathext {
    param([switch]$Add, [switch]$Remove)
    $machine = [Environment]::GetEnvironmentVariable('PATHEXT', 'Machine')
    $user    = [Environment]::GetEnvironmentVariable('PATHEXT', 'User')

    if ($Add) {
        $base = if ($user) { $user } else { $machine }
        $exts = @($base -split ';' | Where-Object { $_ -ne '' })
        foreach ($e in $PathextAdds) {
            if ($exts -notcontains $e) { $exts += $e }
        }
        $new = ($exts -join ';')
        [Environment]::SetEnvironmentVariable('PATHEXT', $new, 'User')
        Write-Host "user PATHEXT set to: $new"
    }
    elseif ($Remove) {
        if (-not $user) { return }  # nothing user-level to clean
        $exts = @($user -split ';' | Where-Object { $_ -ne '' -and $PathextAdds -notcontains $_.ToUpperInvariant() })
        $new = ($exts -join ';')
        if ($new -eq '' -or $new -eq $machine) {
            # Restore original state: drop the user-level override entirely.
            # Delete the registry value directly; SetEnvironmentVariable(...,$null)
            # can leave an empty-string value behind rather than removing it.
            $envKey = 'HKCU:\Environment'
            if (Test-Path -LiteralPath $envKey) {
                $existing = Get-ItemProperty -LiteralPath $envKey -Name 'PATHEXT' -ErrorAction SilentlyContinue
                if ($null -ne $existing) {
                    Remove-ItemProperty -LiteralPath $envKey -Name 'PATHEXT' -ErrorAction SilentlyContinue
                }
            }
            Write-Host 'user PATHEXT restored (override removed)'
        } else {
            [Environment]::SetEnvironmentVariable('PATHEXT', $new, 'User')
            Write-Host "user PATHEXT set to: $new"
        }
    }
}

switch ($Action) {
    'register'   { Invoke-Register }
    'unregister' { Invoke-Unregister }
    'status'     { Invoke-Status }
}
