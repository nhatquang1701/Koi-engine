param(
    [Parameter(Mandatory = $true)]
    [string]$EnginePath
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $EnginePath -PathType Leaf)) {
    throw "engine executable is missing: $EnginePath"
}

$repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
$matchScript = Join-Path $repositoryRoot 'tools/stability/uci_match.ps1'
$openingFile = Join-Path $repositoryRoot 'tests/data/openings/openings-basic.txt'
$binarySuffix = if ($env:OS -eq 'Windows_NT') { '.exe' } else { '' }
if (-not (Test-Path -LiteralPath $matchScript -PathType Leaf)) {
    throw "UCI match script is missing: $matchScript"
}
if (-not (Test-Path -LiteralPath $openingFile -PathType Leaf)) {
    throw "Opening suite is missing: $openingFile"
}

$replayPath = Join-Path (Split-Path -Parent $EnginePath) "koi-replay$binarySuffix"
if (-not (Test-Path -LiteralPath $replayPath -PathType Leaf)) {
    throw "replay executable is missing: $replayPath"
}

$fixturePath = Join-Path (Split-Path -Parent $EnginePath) "uci_match_fixture$binarySuffix"
if (-not (Test-Path -LiteralPath $fixturePath -PathType Leaf)) {
    throw "UCI match fixture executable is missing: $fixturePath"
}

Import-Module ([System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../support/MatchSupport.psm1'))) -Force

$PowerShellExecutable = Get-PowerShellExecutable

function Invoke-ScriptedMatch([string]$KoiPath, [string]$OpponentPath, [string]$OutputDirectory,
                               [int]$Games, [int]$MaxPlies, [int]$TimeoutMilliseconds = 5000,
                               [string]$OpeningFile = '', [string]$TimeControl = '',
                               [string]$KoiColor = 'white', [uint64]$KoiRandomSeed = 1,
                               [bool]$KoiOwnBook = $false, [string]$KoiBookFile = 'book.bin',
                               [int]$KoiBookDepth = 16, [string]$OpponentOwnBook = '',
                                [switch]$Sprt, [int]$SprtMinGames = 20, [int]$SprtMaxGames = 2000,
                                [double]$SprtElo1 = 5.0) {
    $optionalArguments = @()
    if (-not [string]::IsNullOrWhiteSpace($OpeningFile)) {
        $optionalArguments += @('-OpeningFile', $OpeningFile)
    }
    if (-not [string]::IsNullOrWhiteSpace($TimeControl)) {
        $optionalArguments += @('-TimeControl', $TimeControl)
    }
    if ($Sprt) {
        $optionalArguments += @('-Sprt', '-SprtMinGames', $SprtMinGames, '-SprtMaxGames', $SprtMaxGames,
            '-SprtElo1', $SprtElo1)
    }
    if (-not [string]::IsNullOrWhiteSpace($OpponentOwnBook)) {
        $optionalArguments += @('-OpponentOwnBook', $OpponentOwnBook)
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

function Invoke-EvaluatorMatch([string]$Mode, [string]$EvalFile, [string]$OutputDirectory,
                              [int]$Threads = 1) {
    $arguments = @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $matchScript,
        '-KoiPath', $EnginePath, '-OpponentPath', $EnginePath, '-ReplayPath', $replayPath,
        '-KoiEvaluatorMode', $Mode, '-Threads', "$Threads", '-Depth', '1', '-Games', '1',
        '-MaxPlies', '1', '-TimeoutMilliseconds', '5000', '-OutputDirectory', $OutputDirectory
    )
    if (-not [string]::IsNullOrWhiteSpace($EvalFile)) {
        $arguments += @('-KoiEvalFile', $EvalFile)
    }
    $output = @(& $PowerShellExecutable @arguments 2>&1 | ForEach-Object { $_.ToString() })
    $exitCode = $LASTEXITCODE
    $reports = @(Get-ChildItem -LiteralPath $OutputDirectory -Filter '*.json' -File -ErrorAction SilentlyContinue)
    $report = if ($reports.Count -eq 1) {
        Get-Content -LiteralPath $reports[0].FullName -Raw | ConvertFrom-Json
    } else {
        $null
    }
    return [pscustomobject]@{ exit_code = $exitCode; output = $output; report = $report }
}

$outputDirectory = Join-Path ([System.IO.Path]::GetTempPath()) ("koi-match-test-" + [guid]::NewGuid().ToString('N'))
$fenFile = Join-Path $outputDirectory 'terminal.fen'
try {
    $output = & $PowerShellExecutable -NoProfile -ExecutionPolicy Bypass -File $matchScript `
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
    if ($report.configuration.koi_own_book -ne $false -or
        $report.measurement.network.state -ne 'disabled' -or
        $report.measurement.book.state -ne 'disabled' -or
        $report.measurement.tablebase.state -ne 'disabled' -or
        $report.configuration.koi_evaluator_mode -ne 'classical' -or
        $null -ne $report.configuration.koi_evalfile_sha256 -or
        $report.hardware.cpu_count -lt 1 -or
        $report.configuration.run_label -ne 'measurement') {
        throw 'UCI match JSON must preserve classical defaults and record disabled measurement state and run metadata.'
    }
    $koiEngine = @($report.engines | Where-Object { $_.label -eq 'Koi' })[0]
    if ($koiEngine.options -notcontains 'setoption name OwnBook value false') {
        throw 'The default match harness must send OwnBook=false to Koi.'
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

    $nnueDirectory = Join-Path $outputDirectory 'nnue-attestation'
    New-Item -ItemType Directory -Path $nnueDirectory -Force | Out-Null
    $nnueFixturePath = Join-Path (Split-Path -Parent $EnginePath) "koi-nnue-fixture$binarySuffix"
    if (-not (Test-Path -LiteralPath $nnueFixturePath -PathType Leaf)) {
        throw "NNUE fixture generator is missing: $nnueFixturePath"
    }
    $v4Path = Join-Path $nnueDirectory 'fixture-v4.nnue'
    $v5Path = Join-Path $nnueDirectory 'fixture-v5.nnue'
    & $nnueFixturePath --arch v4 --output $v4Path
    if ($LASTEXITCODE -ne 0) {
        throw 'Could not generate the v4 NNUE process-test fixture.'
    }
    & $nnueFixturePath --arch v5 --output $v5Path
    if ($LASTEXITCODE -ne 0) {
        throw 'Could not generate the v5 NNUE process-test fixture.'
    }

    $missingEvalFile = Invoke-EvaluatorMatch 'nnue-v4' '' (Join-Path $nnueDirectory 'missing-file')
    if ($missingEvalFile.exit_code -eq 0 -or
        ($missingEvalFile.output -join ' ') -notmatch 'requires -KoiEvalFile') {
        throw 'A nonclassical evaluator mode must require an EvalFile argument.'
    }
    $absentEvalFile = Invoke-EvaluatorMatch 'nnue-v4' `
        (Join-Path $nnueDirectory 'absent.nnue') (Join-Path $nnueDirectory 'absent-file')
    if ($absentEvalFile.exit_code -eq 0 -or
        ($absentEvalFile.output -join ' ') -notmatch 'Koi EvalFile is missing') {
        throw 'A nonclassical evaluator mode must reject a missing EvalFile path.'
    }

    foreach ($modeCase in @(
        @{ mode = 'nnue-v4'; path = $v4Path },
        @{ mode = 'nnue-v5'; path = $v5Path }
    )) {
        $modeDirectory = Join-Path $nnueDirectory $modeCase.mode
        $modeMatch = Invoke-EvaluatorMatch $modeCase.mode $modeCase.path $modeDirectory
        if ($modeMatch.exit_code -ne 0 -or $null -eq $modeMatch.report) {
            throw "Attested $($modeCase.mode) match failed: $($modeMatch.output -join ' | ')"
        }
        $configuration = $modeMatch.report.configuration
        $evidence = $configuration.koi_evaluator_attestation
        $expectedHash = (Get-FileHash -LiteralPath $modeCase.path -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($configuration.koi_evaluator_mode -ne $modeCase.mode -or
            $configuration.koi_evalfile_sha256 -ne $expectedHash -or
            $evidence.nnue_enabled_confirmation -notmatch '^info string NNUE enabled from ' -or
            @($evidence.evalfile_rejections).Count -ne 0) {
            throw "Attested $($modeCase.mode) report must bind the selected mode and EvalFile hash to engine confirmation."
        }
    }

    $wrongModeDirectory = Join-Path $nnueDirectory 'wrong-mode'
    $wrongModeMatch = Invoke-EvaluatorMatch 'nnue-v5' $v4Path $wrongModeDirectory
    if ($wrongModeMatch.exit_code -eq 0 -or
        ($wrongModeMatch.output -join ' ') -notmatch 'does not contain NNUE v5') {
        throw 'An NNUE v4 file must be rejected when v5 attestation was requested.'
    }

    $truncatedPath = Join-Path $nnueDirectory 'truncated-v4.nnue'
    $v4Bytes = [System.IO.File]::ReadAllBytes($v4Path)
    [System.IO.File]::WriteAllBytes($truncatedPath, [byte[]]$v4Bytes[0..11])
    $rejectedDirectory = Join-Path $nnueDirectory 'rejected-file'
    $rejectedMatch = Invoke-EvaluatorMatch 'nnue-v4' $truncatedPath $rejectedDirectory
    if ($rejectedMatch.exit_code -eq 0 -or
        ($rejectedMatch.output -join ' ') -notmatch 'EvalFile rejected') {
        throw 'An EvalFile rejection must fail the attested match instead of silently falling back.'
    }

    $previousGpuSetting = $env:KOI_GPU_NNUE
    try {
        $env:KOI_GPU_NNUE = '1'
        $singleThreadGpu = Invoke-EvaluatorMatch 'gpu-v5' $v5Path `
            (Join-Path $nnueDirectory 'gpu-single-thread') 1
        if ($singleThreadGpu.exit_code -eq 0 -or
            ($singleThreadGpu.output -join ' ') -notmatch 'requires -Threads of at least 2') {
            throw 'GPU v5 attestation must reject a single-thread configuration.'
        }

        Remove-Item Env:\KOI_GPU_NNUE -ErrorAction SilentlyContinue
        $missingGpuRequest = Invoke-EvaluatorMatch 'gpu-v5' $v5Path `
            (Join-Path $nnueDirectory 'gpu-not-requested') 2
        if ($missingGpuRequest.exit_code -eq 0 -or
            ($missingGpuRequest.output -join ' ') -notmatch 'requires KOI_GPU_NNUE=1') {
            throw 'GPU v5 attestation must reject a missing GPU request environment variable.'
        }

        $env:KOI_GPU_NNUE = '1'
        $gpuMatch = Invoke-EvaluatorMatch 'gpu-v5' $v5Path `
            (Join-Path $nnueDirectory 'gpu-marker') 2
        if ($gpuMatch.exit_code -eq 0) {
            $gpuEvidence = $gpuMatch.report.configuration.koi_evaluator_attestation
            if ($gpuEvidence.gpu_enabled_marker -ne 'koi-engine: GPU NNUE inference enabled.' -or
                @($gpuEvidence.gpu_unavailable_fallbacks).Count -ne 0) {
                throw 'A successful GPU v5 report must contain the enabled marker without CPU fallback.'
            }
        } elseif (($gpuMatch.output -join ' ') -notmatch 'GPU NNUE unavailable|GPU NNUE inference enabled marker') {
            throw "GPU v5 must fail only when actual GPU inference could not be attested: $($gpuMatch.output -join ' | ')"
        }
    } finally {
        if ($null -eq $previousGpuSetting) {
            Remove-Item Env:\KOI_GPU_NNUE -ErrorAction SilentlyContinue
        } else {
            $env:KOI_GPU_NNUE = $previousGpuSetting
        }
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
    $whiteRepeater = New-ScriptedUciEngine -FixturePath $fixturePath -Directory $repetitionDirectory 'white-repeater'
    $blackRepeater = New-ScriptedUciEngine -FixturePath $fixturePath -Directory $repetitionDirectory 'black-repeater'
    $repetitionMatch = Invoke-ScriptedMatch $whiteRepeater.path $blackRepeater.path $repetitionDirectory 1 8
    $repetitionGame = $repetitionMatch.report.games[0]
    if ($repetitionGame.result -ne '1/2-1/2' -or $repetitionGame.termination -ne 'rule draw' -or
        $repetitionGame.moves.Count -ne 8) {
        throw 'Replay validation must retain the full move history when adjudicating threefold repetition.'
    }

    $illegalDirectory = Join-Path $outputDirectory 'illegal-move'
    New-Item -ItemType Directory -Path $illegalDirectory -Force | Out-Null
    $illegalKoi = New-ScriptedUciEngine -FixturePath $fixturePath -Directory $illegalDirectory 'illegal-koi'
    $unusedOpponent = New-ScriptedUciEngine -FixturePath $fixturePath -Directory $illegalDirectory 'unused-opponent'
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
    $bookKoi = New-ScriptedUciEngine -FixturePath $fixturePath -Directory $openingDirectory 'book-koi'
    $openingOpponent = New-ScriptedUciEngine -FixturePath $fixturePath -Directory $openingDirectory 'opening-opponent'
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
    $equalOptionsDirectory = Join-Path $outputDirectory 'equal-baseline-options'
    New-Item -ItemType Directory -Path $equalOptionsDirectory -Force | Out-Null
    $equalOptionsMatch = Invoke-ScriptedMatch $EnginePath $EnginePath $equalOptionsDirectory 1 2 `
        -KoiRandomSeed 0 -KoiOwnBook $false -OpponentOwnBook 'false'
    $equalOpponentOptions = @($equalOptionsMatch.report.engines | Where-Object { $_.label -eq 'Opponent' })[0].options
    if ($equalOpponentOptions -notcontains 'setoption name OwnBook value false' -or
        $equalOptionsMatch.report.configuration.opponent_own_book -ne $false) {
        throw 'Explicit baseline book setting must be sent to the opponent.'
    }
    if ($e4Game.moves[1].book_used -ne $true -or $e4Game.moves[1].book_move -ne 'e7e5' -or
        $e4Game.moves[1].final_info -ne $null -or
        $e4Game.moves[1].all_info_lines -notcontains 'info string book move e7e5 depth 1') {
        throw 'Book diagnostics must be recorded separately from search PV information.'
    }

    $clockDirectory = Join-Path $outputDirectory 'five-plus-three-clock'
    New-Item -ItemType Directory -Path $clockDirectory -Force | Out-Null
    $clockKoi = New-ScriptedUciEngine -FixturePath $fixturePath -Directory $clockDirectory 'clock-koi'
    $clockOpponent = New-ScriptedUciEngine -FixturePath $fixturePath -Directory $clockDirectory 'clock-opponent'
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
    $preflightKoi = New-ScriptedUciEngine -FixturePath $fixturePath -Directory $invalidOpeningDirectory 'preflight-koi'
    $preflightOpponent = New-ScriptedUciEngine -FixturePath $fixturePath -Directory $invalidOpeningDirectory 'preflight-opponent'
    $invalidOutput = & $PowerShellExecutable -NoProfile -ExecutionPolicy Bypass -File $matchScript `
        -KoiPath $preflightKoi.path -OpponentPath $preflightOpponent.path -ReplayPath $replayPath `
        -OpeningFile $invalidOpeningFile -OutputDirectory $invalidOpeningDirectory
    if ($LASTEXITCODE -eq 0 -or (Test-Path -LiteralPath $preflightKoi.log) -or
        (Test-Path -LiteralPath $preflightOpponent.log)) {
        throw 'Illegal opening moves must fail replay validation before either engine begins a game.'
    }

    $timeoutDirectory = Join-Path $outputDirectory 'timeout'
    New-Item -ItemType Directory -Path $timeoutDirectory -Force | Out-Null
    $slowKoi = New-ScriptedUciEngine -FixturePath $fixturePath -Directory $timeoutDirectory 'slow-koi'
    $idleOpponent = New-ScriptedUciEngine -FixturePath $fixturePath -Directory $timeoutDirectory 'idle-opponent'
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

    # The forced-result cases use elo1=200 so a 100% score produces a decisive
    # LLR quickly; with the default elo1=5 a perfect score only gains about
    # 0.014 per game, which is correct SPRT behaviour but would not stop inside
    # a 100-game cap.
    $sprtWinDirectory = Join-Path $outputDirectory 'sprt-win'
    New-Item -ItemType Directory -Path $sprtWinDirectory -Force | Out-Null
    $mateWhite = New-ScriptedUciEngine -FixturePath $fixturePath -Directory $sprtWinDirectory 'mate-white'
    $passiveBlack = New-ScriptedUciEngine -FixturePath $fixturePath -Directory $sprtWinDirectory 'passive-black'
    $sprtWinMatch = Invoke-ScriptedMatch $mateWhite.path $passiveBlack.path $sprtWinDirectory `
        200 16 5000 '' '' 'white' 1 $false 'book.bin' 16 -Sprt -SprtMinGames 10 -SprtMaxGames 100 `
        -SprtElo1 200
    $sprtWinReport = $sprtWinMatch.report
    if ($sprtWinReport.sprt.enabled -ne $true -or $sprtWinReport.sprt.decision -ne 'accept' -or
        $sprtWinReport.sprt.games -lt 10 -or $sprtWinReport.sprt.games -ge 100 -or
        $sprtWinReport.sprt.llr -lt $sprtWinReport.sprt.upper -or
        $sprtWinReport.sprt.wins -ne $sprtWinReport.sprt.games -or
        $sprtWinReport.games.Count -ne $sprtWinReport.sprt.games) {
        throw 'An always-winning candidate must accept the SPRT alternative at the first legal stop.'
    }

    $sprtLoseDirectory = Join-Path $outputDirectory 'sprt-lose'
    New-Item -ItemType Directory -Path $sprtLoseDirectory -Force | Out-Null
    $passiveBlackLose = New-ScriptedUciEngine -FixturePath $fixturePath -Directory $sprtLoseDirectory 'passive-black'
    $mateWhiteLose = New-ScriptedUciEngine -FixturePath $fixturePath -Directory $sprtLoseDirectory 'mate-white'
    $sprtLoseMatch = Invoke-ScriptedMatch $passiveBlackLose.path $mateWhiteLose.path $sprtLoseDirectory `
        200 16 5000 '' '' 'black' 1 $false 'book.bin' 16 -Sprt -SprtMinGames 10 -SprtMaxGames 100 `
        -SprtElo1 200
    $sprtLoseReport = $sprtLoseMatch.report
    if ($sprtLoseReport.sprt.decision -ne 'reject' -or
        $sprtLoseReport.sprt.games -lt 10 -or $sprtLoseReport.sprt.games -ge 100 -or
        $sprtLoseReport.sprt.llr -gt $sprtLoseReport.sprt.lower -or
        $sprtLoseReport.sprt.losses -ne $sprtLoseReport.sprt.games) {
        throw 'An always-losing candidate must reject the SPRT alternative at the first legal stop.'
    }

    $sprtBalancedDirectory = Join-Path $outputDirectory 'sprt-balanced'
    New-Item -ItemType Directory -Path $sprtBalancedDirectory -Force | Out-Null
    $alternatorWhite = New-ScriptedUciEngine -FixturePath $fixturePath -Directory $sprtBalancedDirectory 'sprt-alternator-white'
    $alternatorBlack = New-ScriptedUciEngine -FixturePath $fixturePath -Directory $sprtBalancedDirectory 'sprt-alternator-black'
    $sprtBalancedMatch = Invoke-ScriptedMatch $alternatorWhite.path $alternatorBlack.path $sprtBalancedDirectory `
        40 16 5000 '' '' 'white' 1 $false 'book.bin' 16 -Sprt -SprtMinGames 10 -SprtMaxGames 30
    $sprtBalancedReport = $sprtBalancedMatch.report
    if ($sprtBalancedReport.sprt.decision -ne 'inconclusive' -or
        $sprtBalancedReport.sprt.games -ne 30 -or
        $sprtBalancedReport.sprt.wins -ne 15 -or $sprtBalancedReport.sprt.losses -ne 15 -or
        $sprtBalancedReport.sprt.draws -ne 0 -or
        [Math]::Abs([double]$sprtBalancedReport.sprt.llr) -gt 0.5 -or
        $sprtBalancedReport.games.Count -ne 30) {
        throw 'A balanced candidate must run to the SPRT game cap and report inconclusive.'
    }
}
finally {
    if (Test-Path -LiteralPath $outputDirectory) {
        Remove-Item -LiteralPath $outputDirectory -Recurse -Force
    }
}
