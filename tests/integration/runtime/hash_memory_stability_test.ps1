[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$EnginePath
)

$ErrorActionPreference = 'Stop'
$repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..'))
$outputDirectory = Join-Path $repositoryRoot 'artifacts\stability\hash-memory-test'
$soakScript = Join-Path $repositoryRoot 'tools\stability\hash_memory_soak.ps1'

& $soakScript -EnginePath $EnginePath -HashValues '1,2' -Cycles 1 -OutputDirectory $outputDirectory
if (-not $?) {
    throw 'Hash memory stability smoke failed'
}

$manifest = Get-ChildItem -LiteralPath $outputDirectory -Filter 'manifest.json' -Recurse -File |
    Sort-Object LastWriteTimeUtc | Select-Object -Last 1
if ($null -eq $manifest) {
    throw 'Hash memory stability smoke did not produce a manifest'
}

$report = Get-Content -LiteralPath $manifest.FullName -Raw | ConvertFrom-Json
foreach ($record in @($report.records)) {
    if ($record.exit_code -ne 0 -or $record.bestmove_count -lt 1) {
        throw "Hash memory record failed for requested hash $($record.hash_requested_mb) MB"
    }
    if ($record.hash_effective_mb -lt 1 -or $record.hash_effective_mb -gt $record.hash_requested_mb) {
        throw "Hash memory record has invalid effective hash for requested hash $($record.hash_requested_mb) MB"
    }
    if ($record.peak_working_set_bytes -le 0 -or $record.peak_commit_bytes -le 0) {
        throw "Hash memory record has incomplete memory telemetry for requested hash $($record.hash_requested_mb) MB"
    }
    if (-not (Test-Path -LiteralPath $record.diagnostics -PathType Leaf)) {
        throw "Hash memory record is missing diagnostics for requested hash $($record.hash_requested_mb) MB"
    }
}
