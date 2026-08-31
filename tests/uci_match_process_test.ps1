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
$fenFile = Join-Path $outputDirectory 'terminal.fen'
try {
    $output = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $matchScript `
        -KoiPath $EnginePath -OpponentPath $EnginePath -Depth 1 -Games 1 `
        -MaxPlies 2 -OutputDirectory $outputDirectory
    if ($LASTEXITCODE -ne 0) {
        throw "UCI match script exited with ${LASTEXITCODE}: $($output -join ' | ')"
    }

    $jsonFiles = @(Get-ChildItem -LiteralPath $outputDirectory -Filter '*.json' -File)
    $pgnFiles = @(Get-ChildItem -LiteralPath $outputDirectory -Filter '*.pgn' -File)
    if ($jsonFiles.Count -ne 1 -or $pgnFiles.Count -ne 1) {
        throw "UCI match script must create one JSON and one PGN artifact. Output: $($output -join ' | ')"
    }

    $report = Get-Content -LiteralPath $jsonFiles[0].FullName -Raw | ConvertFrom-Json
    if ($report.schema -ne 'koi-uci-match-v2' -or $report.games.Count -ne 1) {
        throw 'UCI match JSON must identify its v2 schema and contain one game.'
    }
    $game = $report.games[0]
    if ($game.moves.Count -ne 2 -or
        $report.games[0].moves[0].move -notmatch '^[a-h][1-8][a-h][1-8][nbrq]?$') {
        throw 'UCI match JSON must record at least one legal-looking coordinate move.'
    }
    if ($game.result -ne '*' -or $game.winner -ne $null -or $game.termination -ne 'max plies') {
        throw 'A non-terminal max-ply match must remain unfinished in the v2 report.'
    }
    if ($game.process_status.koi -ne 'clean shutdown' -or
        $game.process_status.opponent -ne 'clean shutdown') {
        throw 'UCI match JSON must record clean process shutdown for both engines.'
    }
    $firstPly = $game.moves[0]
    $secondPly = $game.moves[1]
    if ($firstPly.root_fen -eq $secondPly.root_fen -or
        $firstPly.root_fen -notmatch ' w KQkq - 0 1$' -or
        $secondPly.root_fen -notmatch ' [wb] ' -or
        $firstPly.replay_legal -ne $true -or $secondPly.replay_legal -ne $true) {
        throw 'UCI match JSON must record changing actual root FENs and replay legality for every ply.'
    }
    foreach ($ply in @($firstPly, $secondPly)) {
        if ([string]::IsNullOrWhiteSpace($ply.position_command) -or
            [string]::IsNullOrWhiteSpace($ply.go_command) -or
            [string]::IsNullOrWhiteSpace($ply.engine) -or
            [string]::IsNullOrWhiteSpace($ply.engine_label) -or
            [string]::IsNullOrWhiteSpace($ply.bestmove_line) -or
            $ply.infos.Count -lt 1 -or $ply.final_info.pv.Count -lt 1 -or
            $ply.all_info_lines.Count -lt 1) {
            throw 'UCI match JSON must preserve reproducible v2 per-ply command and engine fields.'
        }
    }
    if ($firstPly.evaluation -eq $null) {
        throw 'UCI match JSON must record the final per-move evaluation.'
    }
    $pgn = Get-Content -LiteralPath $pgnFiles[0].FullName -Raw
    if ($pgn -notmatch '\[Event "Koi Engine UCI match"\]' -or
        $pgn -notmatch '\[Result "\*"\]' -or
        $pgn -notmatch '\[MoveFormat "UCI coordinate notation"\]') {
        throw 'UCI match PGN must contain reproducibility headers.'
    }

    New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
    Set-Content -LiteralPath $fenFile -Value 'forced-mate | 7k/6Q1/5K2/8/8/8/8/8 b - - 0 1' -Encoding UTF8
    $terminalOutputDirectory = Join-Path $outputDirectory 'terminal'
    $terminalOutput = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $matchScript `
        -KoiPath $EnginePath -OpponentPath $EnginePath -Depth 1 -Games 1 `
        -FenFile $fenFile -OutputDirectory $terminalOutputDirectory
    if ($LASTEXITCODE -ne 0) {
        throw "terminal UCI match script exited with ${LASTEXITCODE}: $($terminalOutput -join ' | ')"
    }
    $terminalJson = @(Get-ChildItem -LiteralPath $terminalOutputDirectory -Filter '*.json' -File)
    $terminalPgn = @(Get-ChildItem -LiteralPath $terminalOutputDirectory -Filter '*.pgn' -File)
    if ($terminalJson.Count -ne 1 -or $terminalPgn.Count -ne 1) {
        throw 'Terminal UCI match must create one JSON and one PGN artifact.'
    }
    $terminalReport = Get-Content -LiteralPath $terminalJson[0].FullName -Raw | ConvertFrom-Json
    $terminalGame = $terminalReport.games[0]
    if ($terminalGame.result -ne '1-0' -or $terminalGame.winner -ne 'white' -or
        $terminalGame.termination -ne 'checkmate' -or $terminalGame.moves.Count -ne 0) {
        throw 'Terminal UCI match must classify checkmate without asking an engine for a move.'
    }
    $terminalPgnContents = Get-Content -LiteralPath $terminalPgn[0].FullName -Raw
    if ($terminalPgnContents -notmatch '\[Result "1-0"\]' -or $terminalPgnContents -notmatch '1-0\s*$') {
        throw 'Terminal UCI match PGN result must match the adjudicated checkmate result.'
    }
}
finally {
    if (Test-Path -LiteralPath $outputDirectory) {
        Remove-Item -LiteralPath $outputDirectory -Recurse -Force
    }
}
