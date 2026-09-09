param(
    [Parameter(Mandatory = $true)]
    [string]$EnginePath
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $EnginePath -PathType Leaf)) {
    throw "engine executable is missing: $EnginePath"
}

$repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..'))
$matchScript = Join-Path $repositoryRoot 'tools\stability\uci_match.ps1'
$openingFile = Join-Path $repositoryRoot 'tests\data\openings\openings-basic.txt'
if (-not (Test-Path -LiteralPath $matchScript -PathType Leaf)) {
    throw "UCI match script is missing: $matchScript"
}
if (-not (Test-Path -LiteralPath $openingFile -PathType Leaf)) {
    throw "Opening suite is missing: $openingFile"
}

$replayPath = Join-Path (Split-Path -Parent $EnginePath) 'koi-replay.exe'
if (-not (Test-Path -LiteralPath $replayPath -PathType Leaf)) {
    throw "replay executable is missing: $replayPath"
}

$fixturePath = Join-Path (Split-Path -Parent $EnginePath) 'uci_match_fixture.exe'
if (-not (Test-Path -LiteralPath $fixturePath -PathType Leaf)) {
    throw "UCI match fixture executable is missing: $fixturePath"
}

function Get-PowerShellExecutable {
    if ($PSVersionTable.PSEdition -eq 'Core') {
        return (Get-Process -Id $PID -ErrorAction Stop).Path
    }

    $pwsh = Get-Command pwsh -ErrorAction SilentlyContinue
    if ($null -ne $pwsh) {
        return $pwsh.Source
    }

    return (Get-Command powershell.exe -ErrorAction Stop).Source
}

$PowerShellExecutable = Get-PowerShellExecutable

function New-ScriptedUciEngine([string]$Directory, [string]$Name) {
    $enginePath = Join-Path $Directory "$Name.exe"
    Copy-Item -LiteralPath $fixturePath -Destination $enginePath
    return [pscustomobject]@{
        path = $enginePath
        log = Join-Path $Directory "$Name.log"
    }
}

function Invoke-ScriptedMatch([string]$KoiPath, [string]$OpponentPath, [string]$OutputDirectory,
                               [int]$Games, [int]$MaxPlies, [int]$TimeoutMilliseconds = 5000,
                               [string]$OpeningFile = '', [string]$TimeControl = '',
                               [string]$KoiColor = 'white', [uint64]$KoiRandomSeed = 1,
                               [bool]$KoiOwnBook = $true, [string]$KoiBookFile = 'book.bin',
                               [int]$KoiBookDepth = 16) {
    $optionalArguments = @()
    if (-not [string]::IsNullOrWhiteSpace($OpeningFile)) {
        $optionalArguments += @('-OpeningFile', $OpeningFile)
    }
    if (-not [string]::IsNullOrWhiteSpace($TimeControl)) {
        $optionalArguments += @('-TimeControl', $TimeControl)
    }
    $output = & $PowerShellExecutable -NoProfile -ExecutionPolicy Bypass -File $matchScript `
        -KoiPath $KoiPath -OpponentPath $OpponentPath -ReplayPath $replayPath `
        -KoiColor $KoiColor -Depth 1 -Games $Games -MaxPlies $MaxPlies `
        -TimeoutMilliseconds $TimeoutMilliseconds -OutputDirectory $OutputDirectory `
        -KoiRandomSeed $KoiRandomSeed -KoiOwnBook $KoiOwnBook.ToString().ToLowerInvariant() `
        -KoiBookFile $KoiBookFile -KoiBookDepth $KoiBookDepth @optionalArguments
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
    $output = & $PowerShellExecutable -NoProfile -ExecutionPolicy Bypass -File $matchScript `
        -KoiPath $EnginePath -OpponentPath $EnginePath -Depth 1 -Games 1 `
        -MaxPlies 2 -KoiOwnBook false -OutputDirectory $outputDirectory
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
    if ($report.measurement.network.state -ne 'disabled' -or
        $report.measurement.book.state -ne 'disabled' -or
        $report.measurement.tablebase.state -ne 'disabled' -or
        $report.hardware.cpu_count -lt 1 -or
        $report.configuration.run_label -ne 'measurement') {
        throw 'UCI match JSON must record disabled network/book/tablebase state and hardware/run metadata.'
    }
    foreach ($engine in @($report.engines)) {
        if ([string]::IsNullOrWhiteSpace($engine.version) -or
            [string]::IsNullOrWhiteSpace($engine.hashes.executable_sha256)) {
            throw 'UCI match JSON must record engine version and executable hash metadata.'
        }
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
            @($ply.all_info_lines).Count -lt 1) {
            throw 'UCI match JSON must preserve reproducible v2 per-ply command and engine fields.'
        }
        if ($null -eq $ply.position_classification -or
            [string]::IsNullOrWhiteSpace($ply.position_classification.phase) -or
            $ply.position_classification.side_to_move -notin @('white', 'black')) {
            throw 'UCI match JSON must classify every recorded root position.'
        }
        if ($ply.book_used) {
            if ([string]::IsNullOrWhiteSpace($ply.book_move) -or
                @($ply.infos).Count -ne 0 -or $null -ne $ply.final_info -or
                $ply.bestmove_line -notmatch ('^bestmove ' + [regex]::Escape($ply.book_move) + '$')) {
                throw 'UCI match JSON must keep book-backed plies separate from search PV fields.'
            }
        } elseif (@($ply.infos).Count -lt 1 -or $null -eq $ply.final_info -or
                  @($ply.final_info.pv).Count -lt 1) {
            throw 'UCI match JSON must preserve search info and a final PV for non-book plies.'
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
    $terminalOutput = & $PowerShellExecutable -NoProfile -ExecutionPolicy Bypass -File $matchScript `
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

    $openingDirectory = Join-Path $outputDirectory 'opening-clock-book'
    New-Item -ItemType Directory -Path $openingDirectory -Force | Out-Null
    $bookKoi = New-ScriptedUciEngine $openingDirectory 'book-koi'
    $openingOpponent = New-ScriptedUciEngine $openingDirectory 'opening-opponent'
    $openingMatch = Invoke-ScriptedMatch $bookKoi.path $openingOpponent.path $openingDirectory `
        1 1 5000 $openingFile '1+0' 'black' 29 $false 'fixtures\elo-book.bin' 7
    $openingReport = $openingMatch.report
    if ($openingReport.schema -ne 'koi-uci-match-v2' -or $openingReport.games.Count -ne 8 -or
        $openingReport.configuration.time_control -ne '1+0' -or
        $openingReport.configuration.koi_color -ne 'black' -or
        $openingReport.configuration.koi_random_seed -ne 29 -or
        $openingReport.configuration.koi_own_book -ne $false -or
        $openingReport.configuration.koi_book_file -ne 'fixtures\elo-book.bin' -or
        $openingReport.configuration.koi_book_depth -ne 7) {
        throw 'Opening matches must preserve the v2 report while replaying every named opening with Koi as Black.'
    }
    $expectedOpenings = @{
        e4 = @{ moves = @('e2e4'); side = 'b'; engine = 'Koi' }
        d4 = @{ moves = @('d2d4'); side = 'b'; engine = 'Koi' }
        english = @{ moves = @('c2c4'); side = 'b'; engine = 'Koi' }
        scandinavian = @{ moves = @('e2e4', 'd7d5'); side = 'w'; engine = 'Opponent' }
        french = @{ moves = @('e2e4', 'e7e6'); side = 'w'; engine = 'Opponent' }
        'caro-kann' = @{ moves = @('e2e4', 'c7c6'); side = 'w'; engine = 'Opponent' }
        sicilian = @{ moves = @('e2e4', 'c7c5'); side = 'w'; engine = 'Opponent' }
        'queens-gambit' = @{ moves = @('d2d4', 'd7d5', 'c2c4'); side = 'b'; engine = 'Koi' }
    }
    foreach ($name in $expectedOpenings.Keys) {
        $expected = $expectedOpenings[$name]
        $game = @($openingReport.games | Where-Object { $_.position -eq $name })[0]
        $expectedCommand = 'position startpos moves ' + ($expected.moves -join ' ')
        $openingMoves = @($game.moves | Select-Object -First $expected.moves.Count)
        if ($null -eq $game -or $game.koi_color -ne 'black' -or
            $game.moves.Count -ne ($expected.moves.Count + 1) -or
            (@($openingMoves.move) -join ' ') -ne ($expected.moves -join ' ') -or
            @($openingMoves | Where-Object { $_.replay_legal -ne $true }).Count -ne 0 -or
            $game.moves[$expected.moves.Count].side -ne $expected.side -or
            $game.moves[$expected.moves.Count].engine_label -ne $expected.engine -or
            $game.moves[$expected.moves.Count].position_command -ne $expectedCommand) {
            throw "Opening '$name' must replay its exact legal sequence and assign the next side to the correct engine."
        }
    }
    $e4Game = @($openingReport.games | Where-Object { $_.position -eq 'e4' })[0]
    $clockCommand = $e4Game.moves[1].go_command
    if ($clockCommand -notmatch '^go wtime 60000 btime 60000 winc 0 binc 0$') {
        throw "Clock matches must send both clocks and increments on every search. Actual: $clockCommand"
    }
    $koiOptions = @($openingReport.engines | Where-Object { $_.label -eq 'Koi' })[0].options
    foreach ($option in @(
        'setoption name RandomSeed value 29',
        'setoption name OwnBook value false',
        'setoption name BookFile value fixtures\elo-book.bin',
        'setoption name BookDepth value 7'
    )) {
        if ($koiOptions -notcontains $option) {
            throw "Koi opening-book match configuration must send '$option'."
        }
    }
    $opponentOptions = @($openingReport.engines | Where-Object { $_.label -eq 'Opponent' })[0].options
    if (@($opponentOptions | Where-Object { $_ -match '^setoption name (RandomSeed|OwnBook|BookFile|BookDepth) value ' }).Count -ne 0) {
        throw 'Koi-specific seed and book options must not be sent to the opponent.'
    }
    if ($e4Game.moves[1].book_used -ne $true -or $e4Game.moves[1].book_move -ne 'e7e5' -or
        $e4Game.moves[1].final_info -ne $null -or
        $e4Game.moves[1].all_info_lines -notcontains 'info string book move e7e5 depth 1') {
        throw 'Book diagnostics must be recorded separately from search PV information.'
    }

    $clockDirectory = Join-Path $outputDirectory 'five-plus-three-clock'
    New-Item -ItemType Directory -Path $clockDirectory -Force | Out-Null
    $clockKoi = New-ScriptedUciEngine $clockDirectory 'clock-koi'
    $clockOpponent = New-ScriptedUciEngine $clockDirectory 'clock-opponent'
    $clockMatch = Invoke-ScriptedMatch $clockKoi.path $clockOpponent.path $clockDirectory 1 3 5000 '' '5+3' 'white'
    $clockGame = $clockMatch.report.games[0]
    if ($clockMatch.report.configuration.time_control -ne '5+3' -or $clockGame.moves.Count -ne 3) {
        throw 'Clock matches must accept 5+3 and play enough plies to observe post-move accounting.'
    }
    $firstClockPly = $clockGame.moves[0]
    $secondClockPly = $clockGame.moves[1]
    $thirdClockPly = $clockGame.moves[2]
    if ($firstClockPly.elapsed_ms -le 0 -or $secondClockPly.elapsed_ms -le 0 -or
        $firstClockPly.go_command -ne 'go wtime 300000 btime 300000 winc 3000 binc 3000' -or
        $thirdClockPly.go_command -ne ("go wtime {0} btime {1} winc 3000 binc 3000" -f
            (303000 - $firstClockPly.elapsed_ms), (303000 - $secondClockPly.elapsed_ms))) {
        throw 'A 5+3 match must subtract measured move time and then add the increment before the next clock command.'
    }

    $invalidOpeningDirectory = Join-Path $outputDirectory 'invalid-opening'
    New-Item -ItemType Directory -Path $invalidOpeningDirectory -Force | Out-Null
    $invalidOpeningFile = Join-Path $invalidOpeningDirectory 'invalid-openings.txt'
    Set-Content -LiteralPath $invalidOpeningFile -Value 'illegal | e2e5' -Encoding UTF8
    $preflightKoi = New-ScriptedUciEngine $invalidOpeningDirectory 'preflight-koi'
    $preflightOpponent = New-ScriptedUciEngine $invalidOpeningDirectory 'preflight-opponent'
    $invalidOutput = & $PowerShellExecutable -NoProfile -ExecutionPolicy Bypass -File $matchScript `
        -KoiPath $preflightKoi.path -OpponentPath $preflightOpponent.path -ReplayPath $replayPath `
        -OpeningFile $invalidOpeningFile -OutputDirectory $invalidOpeningDirectory
    if ($LASTEXITCODE -eq 0 -or (Test-Path -LiteralPath $preflightKoi.log) -or
        (Test-Path -LiteralPath $preflightOpponent.log)) {
        throw 'Illegal opening moves must fail replay validation before either engine begins a game.'
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
