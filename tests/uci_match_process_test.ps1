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

$replayPath = Join-Path (Split-Path -Parent $EnginePath) 'koi-replay.exe'
if (-not (Test-Path -LiteralPath $replayPath -PathType Leaf)) {
    throw "replay executable is missing: $replayPath"
}

$fixturePath = Join-Path (Split-Path -Parent $EnginePath) 'uci_match_fixture.exe'
if (-not (Test-Path -LiteralPath $fixturePath -PathType Leaf)) {
    throw "UCI match fixture executable is missing: $fixturePath"
}

function New-ScriptedUciEngine([string]$Directory, [string]$Name) {
    $enginePath = Join-Path $Directory "$Name.exe"
    Copy-Item -LiteralPath $fixturePath -Destination $enginePath
    return [pscustomobject]@{
        path = $enginePath
        log = Join-Path $Directory "$Name.log"
    }
}

function Invoke-ScriptedMatch([string]$KoiPath, [string]$OpponentPath, [string]$OutputDirectory,
                               [int]$Games, [int]$MaxPlies, [int]$TimeoutMilliseconds = 5000) {
    $output = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $matchScript `
        -KoiPath $KoiPath -OpponentPath $OpponentPath -ReplayPath $replayPath `
        -KoiColor white -Depth 1 -Games $Games -MaxPlies $MaxPlies `
        -TimeoutMilliseconds $TimeoutMilliseconds -OutputDirectory $OutputDirectory
    if ($LASTEXITCODE -ne 0) {
        throw "scripted UCI match exited with ${LASTEXITCODE}: $($output -join ' | ')"
    }
    $jsonFiles = @(Get-ChildItem -LiteralPath $OutputDirectory -Filter '*.json' -File)
    $pgnFiles = @(Get-ChildItem -LiteralPath $OutputDirectory -Filter '*.pgn' -File)
    if ($jsonFiles.Count -ne 1 -or $pgnFiles.Count -ne 1) {
        throw 'scripted UCI match must create one JSON and one PGN artifact.'
    }
    return [pscustomobject]@{
        report = Get-Content -LiteralPath $jsonFiles[0].FullName -Raw | ConvertFrom-Json
        pgn = Get-Content -LiteralPath $pgnFiles[0].FullName -Raw
    }
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

    $repetitionDirectory = Join-Path $outputDirectory 'repetition'
    New-Item -ItemType Directory -Path $repetitionDirectory -Force | Out-Null
    $whiteRepeater = New-ScriptedUciEngine $repetitionDirectory 'white-repeater'
    $blackRepeater = New-ScriptedUciEngine $repetitionDirectory 'black-repeater'
    $repetitionMatch = Invoke-ScriptedMatch $whiteRepeater.path $blackRepeater.path $repetitionDirectory 1 8
    $repetitionGame = $repetitionMatch.report.games[0]
    if ($repetitionGame.result -ne '1/2-1/2' -or $repetitionGame.termination -ne 'rule draw' -or
        $repetitionGame.moves.Count -ne 8) {
        throw 'Replay validation must retain the full move history when adjudicating threefold repetition.'
    }

    $illegalDirectory = Join-Path $outputDirectory 'illegal-move'
    New-Item -ItemType Directory -Path $illegalDirectory -Force | Out-Null
    $illegalKoi = New-ScriptedUciEngine $illegalDirectory 'illegal-koi'
    $unusedOpponent = New-ScriptedUciEngine $illegalDirectory 'unused-opponent'
    $illegalMatch = Invoke-ScriptedMatch $illegalKoi.path $unusedOpponent.path $illegalDirectory 1 2
    $illegalGame = $illegalMatch.report.games[0]
    if ($illegalGame.termination -ne 'illegal move' -or $illegalGame.moves.Count -ne 1 -or
        $illegalGame.moves[0].move -ne '0000' -or $illegalGame.moves[0].replay_legal -ne $false) {
        throw 'An illegal bestmove must remain in the JSON diagnostic record.'
    }
    if ($illegalMatch.pgn -match '\b0000\b') {
        throw 'Rejected moves must not be serialized into PGN movetext.'
    }

    $timeoutDirectory = Join-Path $outputDirectory 'timeout'
    New-Item -ItemType Directory -Path $timeoutDirectory -Force | Out-Null
    $slowKoi = New-ScriptedUciEngine $timeoutDirectory 'slow-koi'
    $idleOpponent = New-ScriptedUciEngine $timeoutDirectory 'idle-opponent'
    $timeoutMatch = Invoke-ScriptedMatch $slowKoi.path $idleOpponent.path $timeoutDirectory 2 2 1000
    $timeoutGame = $timeoutMatch.report.games[0]
    if ($timeoutMatch.report.games.Count -ne 1 -or $timeoutGame.termination -ne 'timeout' -or
        $timeoutGame.process_status.koi -ne 'timeout') {
        throw 'A timed-out engine must be retired before subsequent games can reuse it.'
    }
    $newGameCommands = @(Get-Content -LiteralPath $slowKoi.log | Where-Object { $_ -ceq 'ucinewgame' })
    if ($newGameCommands.Count -ne 1) {
        throw 'A timed-out engine must not receive a later-game ucinewgame command.'
    }
}
finally {
    if (Test-Path -LiteralPath $outputDirectory) {
        Remove-Item -LiteralPath $outputDirectory -Recurse -Force
    }
}
