[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [switch]$Apply
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$buildRoot = [System.IO.Path]::GetFullPath((Join-Path $repositoryRoot 'build'))
$outRoot = [System.IO.Path]::GetFullPath((Join-Path $repositoryRoot 'out'))
$canonicalBuildDirectories = @('debug', 'release', 'ci-debug', 'ci-release')
$targets = [System.Collections.Generic.List[string]]::new()

function Assert-DirectChild([string]$Path, [string]$Parent) {
    $resolvedPath = [System.IO.Path]::GetFullPath($Path).TrimEnd('\', '/')
    $resolvedParent = [System.IO.Path]::GetFullPath($Parent).TrimEnd('\', '/')
    if ([System.IO.Path]::GetDirectoryName($resolvedPath).TrimEnd('\', '/') -ine $resolvedParent) {
        throw "Refusing to prune a non-direct child: $resolvedPath"
    }
    return $resolvedPath
}

if (Test-Path -LiteralPath $buildRoot -PathType Container) {
    foreach ($child in Get-ChildItem -LiteralPath $buildRoot -Force) {
        if ($child.PSIsContainer -and $canonicalBuildDirectories -contains $child.Name) {
            continue
        }
        $targets.Add((Assert-DirectChild $child.FullName $buildRoot))
    }
}

if (Test-Path -LiteralPath $outRoot) {
    $resolvedOut = [System.IO.Path]::GetFullPath((Resolve-Path -LiteralPath $outRoot).Path).TrimEnd('\', '/')
    if ($resolvedOut -ine $outRoot.TrimEnd('\', '/')) {
        throw "Refusing to prune an unexpected out path: $resolvedOut"
    }
    $targets.Add($outRoot)
}

$targets = @($targets | Sort-Object -Unique)
if ($targets.Count -eq 0) {
    Write-Output 'No stale generated trees found.'
    exit 0
}

Write-Output 'Prune targets:'
$targets | ForEach-Object { Write-Output ("  " + $_) }
if (-not $Apply) {
    Write-Output 'Preview only. Re-run with -Apply to remove these explicit generated targets.'
    exit 0
}

foreach ($target in $targets) {
    if ($PSCmdlet.ShouldProcess($target, 'Remove generated build tree')) {
        Remove-Item -LiteralPath $target -Recurse -Force
    }
}

Write-Output ("Removed targets: " + $targets.Count)
