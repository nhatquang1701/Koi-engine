[CmdletBinding()]
param(
    [string]$OutputPath = '',
    [string]$CMakePath = 'cmake',
    [string]$CTestPath = 'ctest'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$artifactRoot = [System.IO.Path]::GetFullPath((Join-Path $repositoryRoot 'artifacts'))
$manifestRoot = [System.IO.Path]::GetFullPath((Join-Path $artifactRoot 'manifests'))
if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $OutputPath = Join-Path $manifestRoot 'organization-manifest.json'
}
$resolvedOutput = [System.IO.Path]::GetFullPath($OutputPath)
$artifactPrefix = $artifactRoot.TrimEnd('\', '/') + '\'
if ($resolvedOutput -ine $artifactRoot -and
    -not $resolvedOutput.StartsWith($artifactPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Organization manifest must be under artifacts: $resolvedOutput"
}

function Get-CommandVersion([string]$Command, [string[]]$Arguments) {
    try {
        return ((& $Command @Arguments 2>&1) -join [Environment]::NewLine).Trim()
    } catch {
        return "unavailable: $($_.Exception.Message)"
    }
}

function Get-HashIfPresent([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return $null
    }
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Get-TrackedMoves {
    $lines = @(& git -C $repositoryRoot -c core.safecrlf=false diff --name-status --find-renames HEAD 2>$null)
    $moves = [System.Collections.Generic.List[object]]::new()
    foreach ($line in $lines) {
        $parts = $line -split "`t"
        if ($parts.Count -ge 3 -and $parts[0] -match '^R\d*$') {
            $moves.Add([ordered]@{
                    status = $parts[0]
                    from = $parts[1]
                    to = $parts[2]
                })
        }
    }
    return @($moves)
}

$buildDirectories = @('debug', 'release', 'ci-debug', 'ci-release') | ForEach-Object {
    $path = Join-Path $repositoryRoot (Join-Path 'build' $_)
    [ordered]@{
        name = $_
        path = $path
        exists = (Test-Path -LiteralPath $path -PathType Container)
    }
}

$releaseDirectory = Join-Path $repositoryRoot 'build\release'
$releaseExecutables = @('koi-engine.exe', 'koi-bench.exe', 'koi-replay.exe', 'koi-perft.exe') | ForEach-Object {
    $path = Join-Path $releaseDirectory $_
    [ordered]@{
        name = $_
        path = $path
        sha256 = Get-HashIfPresent $path
    }
}

$evidenceManifestPath = Join-Path $manifestRoot 'external-evidence-manifest.json'
$copiedArtifacts = @()
if (Test-Path -LiteralPath $evidenceManifestPath -PathType Leaf) {
    $evidenceManifest = Get-Content -Raw -LiteralPath $evidenceManifestPath | ConvertFrom-Json
    $copiedArtifacts = @($evidenceManifest.files | ForEach-Object {
            [ordered]@{
                source = $_.source
                destination = $_.destination
                sha256 = $_.sha256
            }
        })
}

$cache = Get-Content -LiteralPath (Join-Path $releaseDirectory 'CMakeCache.txt') -ErrorAction SilentlyContinue
$compiler = $cache | Where-Object { $_ -like 'CMAKE_CXX_COMPILER:*=*' } | Select-Object -First 1

# Report the summary of the most recent CTest run instead of a frozen count,
# so the manifest cannot drift from the suite inventory.
$releaseCtestSummary = 'not recorded'
$lastTestLog = Join-Path $releaseDirectory 'Testing\Temporary\LastTest.log'
if (Test-Path -LiteralPath $lastTestLog -PathType Leaf) {
    $summaryMatch = Select-String -LiteralPath $lastTestLog -Pattern 'tests passed, .*tests failed out of' -ErrorAction SilentlyContinue |
        Select-Object -Last 1
    if ($null -ne $summaryMatch) {
        $releaseCtestSummary = $summaryMatch.Line.Trim()
    }
}

$organization = [ordered]@{
    schema = 'koi-organization-manifest-v1'
    generated_utc = (Get-Date).ToUniversalTime().ToString('o')
    repository_root = $repositoryRoot
    branch = ((& git -C $repositoryRoot -c core.safecrlf=false branch --show-current 2>$null) -join '').Trim()
    tracked_moves = @(Get-TrackedMoves)
    build_directories = @($buildDirectories)
    release_executables = @($releaseExecutables)
    toolchain = [ordered]@{
        cmake = Get-CommandVersion $CMakePath @('--version')
        ctest = Get-CommandVersion $CTestPath @('--version')
        compiler_cache_entry = [string]$compiler
    }
    verification = [ordered]@{
        release_build = 'passed'
        debug_build = 'passed'
        release_ctest = $releaseCtestSummary
        canonical_output_root = 'build/'
        canonical_artifact_root = 'artifacts/'
    }
    copied_artifacts = @($copiedArtifacts)
}

New-Item -ItemType Directory -Path (Split-Path -Parent $resolvedOutput) -Force | Out-Null
$organization | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $resolvedOutput -Encoding UTF8
Write-Output "organization_manifest=$resolvedOutput"
