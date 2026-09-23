# MatchSupport.psm1
#
# Shared helpers for the scripted UCI match integration tests.  The match
# scripts import this module instead of re-declaring the PowerShell discovery
# and fixture-engine plumbing:
#
#   Import-Module ([System.IO.Path]::GetFullPath(
#       (Join-Path $PSScriptRoot '../../support/MatchSupport.psm1'))) -Force

function Get-PowerShellExecutable {
    <#
    .SYNOPSIS
        Return the path of the PowerShell host running the current script.
    #>
    if ($PSVersionTable.PSEdition -eq 'Core') {
        return (Get-Process -Id $PID -ErrorAction Stop).Path
    }

    $pwsh = Get-Command pwsh -ErrorAction SilentlyContinue
    if ($null -ne $pwsh) {
        return $pwsh.Source
    }

    return (Get-Command powershell.exe -ErrorAction Stop).Source
}

function New-ScriptedUciEngine {
    <#
    .SYNOPSIS
        Copy the scripted UCI fixture next to a match and name it.
    .DESCRIPTION
        Places $FixturePath in $Directory as "$Name.exe" on Windows and
        "$Name" elsewhere, then returns the engine descriptor ({ path, log })
        consumed by the match invocations.
    #>
    param(
        [Parameter(Mandatory = $true, Position = 0)]
        [string]$FixturePath,
        [Parameter(Mandatory = $true, Position = 1)]
        [string]$Directory,
        [Parameter(Mandatory = $true, Position = 2)]
        [string]$Name
    )

    $binarySuffix = if ($env:OS -eq 'Windows_NT') { '.exe' } else { '' }
    $path = Join-Path $Directory "$Name$binarySuffix"
    Copy-Item -LiteralPath $FixturePath -Destination $path
    return [pscustomobject]@{
        path = $path
        log = Join-Path $Directory "$Name.log"
    }
}

Export-ModuleMember -Function Get-PowerShellExecutable, New-ScriptedUciEngine
