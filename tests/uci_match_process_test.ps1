param(
    [Parameter(Mandatory = $true)]
    [string]$EnginePath
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $EnginePath -PathType Leaf)) {
    throw "engine executable is missing: $EnginePath"
}

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$matchScript = Join-Path $repositoryRoot 'tools\uci_match.ps1'
if (-not (Test-Path -LiteralPath $matchScript -PathType Leaf)) {
    throw "UCI match script is missing: $matchScript"
}

$outputDirectory = Join-Path ([System.IO.Path]::GetTempPath()) ("koi-match-test-" + [guid]::NewGuid().ToString('N'))
try {
    $output = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $matchScript `
        -KoiPath $EnginePath -OpponentPath $EnginePath -Depth 1 -Games 1 `
        -OutputDirectory $outputDirectory
    if ($LASTEXITCODE -ne 0) {
        throw "UCI match script exited with ${LASTEXITCODE}: $($output -join ' | ')"
    }

    $jsonFiles = @(Get-ChildItem -LiteralPath $outputDirectory -Filter '*.json' -File)
    $pgnFiles = @(Get-ChildItem -LiteralPath $outputDirectory -Filter '*.pgn' -File)
    if ($jsonFiles.Count -ne 1 -or $pgnFiles.Count -ne 1) {
        throw "UCI match script must create one JSON and one PGN artifact. Output: $($output -join ' | ')"
    }

    $report = Get-Content -LiteralPath $jsonFiles[0].FullName -Raw | ConvertFrom-Json
    if ($report.schema -ne 'koi-uci-match-v1' -or $report.games.Count -ne 1) {
        throw 'UCI match JSON must identify its schema and contain one game.'
    }
    if ($report.games[0].moves.Count -lt 1 -or
        $report.games[0].moves[0].move -notmatch '^[a-h][1-8][a-h][1-8][nbrq]?$') {
        throw 'UCI match JSON must record at least one legal-looking coordinate move.'
    }
    if ($report.games[0].moves[0].evaluation -eq $null) {
        throw 'UCI match JSON must record the final per-move evaluation.'
    }
    $pgn = Get-Content -LiteralPath $pgnFiles[0].FullName -Raw
    if ($pgn -notmatch '\[Event "Koi Engine UCI match"\]' -or
        $pgn -notmatch '\[MoveFormat "UCI coordinate notation"\]') {
        throw 'UCI match PGN must contain reproducibility headers.'
    }
}
finally {
    if (Test-Path -LiteralPath $outputDirectory) {
        Remove-Item -LiteralPath $outputDirectory -Recurse -Force
    }
}
