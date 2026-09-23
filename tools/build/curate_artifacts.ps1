[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$SourceRoot,
    [string]$DestinationRoot = '',
    [switch]$PruneSource
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$artifactRoot = [System.IO.Path]::GetFullPath((Join-Path $repositoryRoot 'artifacts'))
if ([string]::IsNullOrWhiteSpace($DestinationRoot)) {
    $DestinationRoot = $artifactRoot
}
$source = (Resolve-Path -LiteralPath $SourceRoot -ErrorAction Stop).Path
$destination = [System.IO.Path]::GetFullPath($DestinationRoot)
$separator = [System.IO.Path]::DirectorySeparatorChar
$artifactPrefix = $artifactRoot.TrimEnd('\', '/') + $separator
if ($destination -ine $artifactRoot -and
    -not $destination.StartsWith($artifactPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Destination must be inside the repository artifacts directory: $destination"
}
if ($source -ieq $destination -or $source.StartsWith($destination.TrimEnd('\', '/') + $separator, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Source and destination may not overlap: $source -> $destination"
}

$selections = @(
    [ordered]@{ source = 'koi-24h-baseline'; destination = 'baselines/2026-09-06-24h' },
    [ordered]@{ source = 'koi-24h-current'; destination = 'baselines/2026-09-06-current' },
    [ordered]@{ source = 'stability-baseline'; destination = 'stability/2026-09-08-baseline' },
    [ordered]@{ source = 'stability-incident'; destination = 'stability/2026-09-08-incident' },
    [ordered]@{ source = 'koi-stability-100-no-book-final'; destination = 'stability/2026-09-08-no-book-100' },
    [ordered]@{ source = 'koi-stability-20-book-final'; destination = 'stability/2026-09-08-book-20' },
    [ordered]@{ source = 'koi-stability-20-koi-vs-koi-t1'; destination = 'stability/2026-09-08-koi-vs-koi/threads-1' },
    [ordered]@{ source = 'koi-stability-20-koi-vs-koi-t2'; destination = 'stability/2026-09-08-koi-vs-koi/threads-2' },
    [ordered]@{ source = 'koi-stability-20-koi-vs-koi-t4'; destination = 'stability/2026-09-08-koi-vs-koi/threads-4' },
    [ordered]@{ source = 'koi-match-repro'; destination = 'matches/2026-09-07-match-repro' }
)

$standaloneFiles = @(
    'koi-v1.0-baseline-threads1.json',
    'koi-v1.0-baseline-threads1.txt',
    'koi-v1.1-native-timed.json',
    'v1.1-thread4-direct.txt',
    'koi-next-leap-search-fix-report.md',
    'koi-next-leap-search-fix-baseline.index'
)

$blockedExtensions = @('.exe', '.dll', '.pdb', '.ilk', '.lib', '.obj', '.db', '.db3', '.ecsi', '.zip')
$allowedExtensions = @('.json', '.jsonl', '.pgn', '.log', '.txt', '.epd', '.fen', '.md', '.index')
$records = [System.Collections.Generic.List[object]]::new()
$pruneTargets = [System.Collections.Generic.List[string]]::new()

function Get-Hash([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Assert-SafeEvidenceFile([string]$Path) {
    $extension = [System.IO.Path]::GetExtension($Path).ToLowerInvariant()
    if ($blockedExtensions -contains $extension) {
        throw "Refusing to curate blocked generated or binary file: $Path"
    }
    if ($allowedExtensions -notcontains $extension) {
        throw "Refusing to curate unknown evidence file type: $Path"
    }
}

function Copy-EvidenceFile([string]$SourcePath, [string]$DestinationPath) {
    Assert-SafeEvidenceFile $SourcePath
    $destinationDirectory = Split-Path -Parent $DestinationPath
    New-Item -ItemType Directory -Path $destinationDirectory -Force | Out-Null
    Copy-Item -LiteralPath $SourcePath -Destination $DestinationPath -Force
    $sourceHash = Get-Hash $SourcePath
    $destinationHash = Get-Hash $DestinationPath
    if ($sourceHash -ne $destinationHash) {
        throw "Hash mismatch after copying evidence: $SourcePath -> $DestinationPath"
    }
    $records.Add([ordered]@{
        source = $SourcePath
        destination = $DestinationPath
        size = (Get-Item -LiteralPath $SourcePath).Length
        last_write_time_utc = (Get-Item -LiteralPath $SourcePath).LastWriteTimeUtc.ToString('o')
        sha256 = $sourceHash
    })
}

foreach ($selection in $selections) {
    $sourcePath = Join-Path $source $selection.source
    if (-not (Test-Path -LiteralPath $sourcePath)) {
        throw "Approved evidence source is missing: $sourcePath"
    }
    $sourceItem = Get-Item -LiteralPath $sourcePath
    $destinationPath = Join-Path $destination $selection.destination
    if ($sourceItem.PSIsContainer) {
        $pruneTargets.Add($sourceItem.FullName)
        foreach ($file in Get-ChildItem -LiteralPath $sourceItem.FullName -Recurse -File) {
            $relative = $file.FullName.Substring($sourceItem.FullName.Length).TrimStart('\', '/')
            Copy-EvidenceFile $file.FullName (Join-Path $destinationPath $relative)
        }
    } else {
        $pruneTargets.Add($sourceItem.FullName)
        Copy-EvidenceFile $sourceItem.FullName (Join-Path $destinationPath $sourceItem.Name)
    }
}

$standaloneDestination = Join-Path $destination 'baselines/standalone'
foreach ($name in $standaloneFiles) {
    $sourcePath = Join-Path $source $name
    if (-not (Test-Path -LiteralPath $sourcePath -PathType Leaf)) {
        continue
    }
    $pruneTargets.Add((Get-Item -LiteralPath $sourcePath).FullName)
    Copy-EvidenceFile $sourcePath (Join-Path $standaloneDestination $name)
}

$manifest = [ordered]@{
    schema = 'koi-artifact-curation-v1'
    created_utc = (Get-Date).ToUniversalTime().ToString('o')
    source_root = $source
    destination_root = $destination
    selections = $selections
    files = @($records)
    prune_requested = [bool]$PruneSource
}
$manifestPath = Join-Path $destination 'manifests/external-evidence-manifest.json'
New-Item -ItemType Directory -Path (Split-Path -Parent $manifestPath) -Force | Out-Null
$manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $manifestPath -Encoding UTF8

if ($PruneSource) {
    $sourceRootResolved = (Resolve-Path -LiteralPath $source).Path.TrimEnd('\', '/')
    foreach ($target in $pruneTargets | Sort-Object -Unique) {
        $targetResolved = (Resolve-Path -LiteralPath $target).Path.TrimEnd('\', '/')
        if ($targetResolved -ieq $sourceRootResolved -or
            -not $targetResolved.StartsWith($sourceRootResolved + [System.IO.Path]::DirectorySeparatorChar, [System.StringComparison]::OrdinalIgnoreCase)) {
            throw "Refusing to prune outside the selected source root: $targetResolved"
        }
        Remove-Item -LiteralPath $targetResolved -Recurse -Force
    }
}

Write-Output "curated_files=$($records.Count)"
Write-Output "manifest=$manifestPath"
