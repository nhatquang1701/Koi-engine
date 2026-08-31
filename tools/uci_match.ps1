[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$KoiPath,

    [Parameter(Mandatory = $true)]
    [string]$OpponentPath,

    [string]$ReplayPath,

    [string]$FenFile,

    [string]$OutputDirectory = (Join-Path (Get-Location) 'match-results'),

    [ValidateRange(1, 64)]
    [int]$Depth = 4,

    [ValidateRange(1, 100)]
    [int]$Games = 1,

    [ValidateRange(0, 600000)]
    [int]$MovetimeMs = 0,

    [uint64]$Nodes = 0,

    [ValidateRange(1, 64)]
    [int]$Threads = 1,

    [ValidateRange(1, 100)]
    [int]$Speed = 100,

    [ValidateRange(1, 4096)]
    [int]$Hash = 16,

    [ValidateRange(1, 512)]
    [int]$MaxPlies = 512,

    [ValidateRange(1000, 60000)]
    [int]$TimeoutMilliseconds = 5000,

    [ValidateSet('white', 'black')]
    [string]$KoiColor = 'white'
)

$ErrorActionPreference = 'Stop'

if ($MovetimeMs -gt 0 -and $Nodes -gt 0) {
    throw 'Choose at most one of -MovetimeMs and -Nodes.'
}

function Resolve-Executable([string]$Path, [string]$Label) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "${Label} executable is missing: $Path"
    }
    return (Resolve-Path -LiteralPath $Path).Path
}

function Read-PositionSpecs {
    if ([string]::IsNullOrWhiteSpace($FenFile)) {
        return @([pscustomobject]@{ Name = 'startpos'; Fen = 'startpos' })
    }

    if (-not (Test-Path -LiteralPath $FenFile -PathType Leaf)) {
        throw "FEN file is missing: $FenFile"
    }

    $specs = [System.Collections.Generic.List[object]]::new()
    foreach ($line in Get-Content -LiteralPath $FenFile) {
        $trimmed = $line.Trim()
        if ($trimmed.Length -eq 0 -or $trimmed.StartsWith('#')) {
            continue
        }

        $match = [regex]::Match($trimmed, '^(?<name>[^|#\s]+)\s*\|\s*(?<fen>.+)$')
        if (-not $match.Success) {
            throw "Invalid FEN file line. Expected name | FEN: $trimmed"
        }

        $name = $match.Groups['name'].Value
        $fen = $match.Groups['fen'].Value.Trim()
        if ($fen -ne 'startpos' -and ($fen -split '\s+').Count -ne 6) {
            throw "FEN file entry '$name' must contain startpos or six FEN fields."
        }
        $specs.Add([pscustomobject]@{ Name = $name; Fen = $fen })
    }

    if ($specs.Count -eq 0) {
        throw "FEN file contains no positions: $FenFile"
    }
    return @($specs)
}

function Start-UciEngine([string]$Path, [string]$Label) {
    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $Path
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardInput = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true

    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    if (-not $process.Start()) {
        throw "Unable to start ${Label} engine."
    }

    return [pscustomobject]@{
        Label = $Label
        Path = $Path
        Process = $process
        StderrTask = $process.StandardError.ReadToEndAsync()
        Handshake = [System.Collections.Generic.List[string]]::new()
        Output = [System.Collections.Generic.List[string]]::new()
        SentOptions = [System.Collections.Generic.List[string]]::new()
        Name = $Label
        Retired = $false
        StopResult = $null
    }
}

function Send-UciLine($Engine, [string]$Command) {
    if ($Engine.Process.HasExited) {
        throw "${Engine.Label} engine exited before '$Command'."
    }
    $Engine.Process.StandardInput.WriteLine($Command)
    $Engine.Process.StandardInput.Flush()
}

function Stop-UciEngine($Engine, [bool]$KillImmediately = $false) {
    if ($null -eq $Engine) {
        return [pscustomobject]@{ status = 'process exit'; exit_code = $null; stderr = '' }
    }

    $status = 'clean shutdown'
    if (-not $Engine.Process.HasExited) {
        if ($KillImmediately) {
            $status = 'timeout'
            try {
                $Engine.Process.Kill()
                $Engine.Process.WaitForExit()
            } catch {
            }
        } else {
            try {
                Send-UciLine $Engine 'quit'
            } catch {
                $status = 'process exit'
            }
            try {
                $Engine.Process.StandardInput.Close()
            } catch {
            }
            if (-not $Engine.Process.WaitForExit($TimeoutMilliseconds)) {
                $status = 'timeout'
                try {
                    $Engine.Process.Kill()
                    $Engine.Process.WaitForExit()
                } catch {
                }
            }
        }
    }

    $exitCode = $Engine.Process.ExitCode
    if ($status -eq 'clean shutdown' -and $exitCode -ne 0) {
        $status = 'process exit'
    }
    $stderr = ''
    try {
        $stderr = $Engine.StderrTask.GetAwaiter().GetResult()
    } catch {
    }
    return [pscustomobject]@{ status = $status; exit_code = $exitCode; stderr = $stderr }
}

function Read-UciLine($Engine, [string]$Description) {
    $readTask = $Engine.Process.StandardOutput.ReadLineAsync()
    if (-not $readTask.Wait($TimeoutMilliseconds)) {
        throw [System.TimeoutException]::new("Timed out waiting for ${Engine.Label} ${Description}.")
    }

    $line = $readTask.GetAwaiter().GetResult()
    if ($null -eq $line) {
        throw "${Engine.Label} closed stdout while waiting for ${Description}."
    }
    $Engine.Output.Add($line)
    return $line
}

function Initialize-UciEngine($Engine) {
    Send-UciLine $Engine 'uci'
    while ($true) {
        $line = Read-UciLine $Engine 'uciok'
        $Engine.Handshake.Add($line)
        if ($line -ceq 'uciok') {
            break
        }
    }

    foreach ($option in @(
        "setoption name Hash value $Hash",
        "setoption name Threads value $Threads",
        "setoption name Speed value $Speed"
    )) {
        Send-UciLine $Engine $option
        $Engine.SentOptions.Add($option)
    }

    Send-UciLine $Engine 'isready'
    while ((Read-UciLine $Engine 'readyok') -cne 'readyok') {
    }

    $identity = @($Engine.Handshake | Where-Object { $_ -match '^id name (.+)$' } | Select-Object -First 1)
    if ($identity.Count -eq 1) {
        $Engine.Name = ($identity[0] -replace '^id name ', '').Trim()
    }
}

function Wait-UciReady($Engine, [string]$Description) {
    Send-UciLine $Engine 'isready'
    while ((Read-UciLine $Engine $Description) -cne 'readyok') {
    }
}

function Format-PositionCommand($Position, $Moves) {
    if ($Position.Fen -ceq 'startpos') {
        $command = 'position startpos'
    } else {
        $command = "position fen $($Position.Fen)"
    }
    if ($Moves.Count -gt 0) {
        $command += ' moves ' + ($Moves -join ' ')
    }
    return $command
}

function Get-GoCommand {
    if ($MovetimeMs -gt 0) {
        return "go movetime $MovetimeMs"
    }
    if ($Nodes -gt 0) {
        return "go nodes $Nodes"
    }
    return "go depth $Depth"
}

function Parse-InfoLine([string]$Line) {
    if ($Line -notmatch '^info(?:\s|$)') {
        return $null
    }

    $depthMatch = [regex]::Match($Line, '(?:^|\s)depth\s+(-?\d+)')
    $seldepthMatch = [regex]::Match($Line, '(?:^|\s)seldepth\s+(-?\d+)')
    $scoreMatch = [regex]::Match($Line, '(?:^|\s)score\s+(cp|mate)\s+(-?\d+)')
    $nodesMatch = [regex]::Match($Line, '(?:^|\s)nodes\s+(\d+)')
    $npsMatch = [regex]::Match($Line, '(?:^|\s)nps\s+(\d+)')
    $timeMatch = [regex]::Match($Line, '(?:^|\s)time\s+(\d+)')
    $pvMatch = [regex]::Match($Line, '(?:^|\s)pv\s+(.+)$')

    return [pscustomobject][ordered]@{
        depth = if ($depthMatch.Success) { [int]$depthMatch.Groups[1].Value } else { $null }
        seldepth = if ($seldepthMatch.Success) { [int]$seldepthMatch.Groups[1].Value } else { $null }
        score_type = if ($scoreMatch.Success) { $scoreMatch.Groups[1].Value } else { $null }
        score = if ($scoreMatch.Success) { [int]$scoreMatch.Groups[2].Value } else { $null }
        nodes = if ($nodesMatch.Success) { [uint64]$nodesMatch.Groups[1].Value } else { $null }
        nps = if ($npsMatch.Success) { [uint64]$npsMatch.Groups[1].Value } else { $null }
        time_ms = if ($timeMatch.Success) { [int64]$timeMatch.Groups[1].Value } else { $null }
        pv = if ($pvMatch.Success) { @($pvMatch.Groups[1].Value.Trim() -split '\s+') } else { @() }
        raw = $Line
    }
}

function Search-UciEngine($Engine, $Position, $Moves) {
    $positionCommand = Format-PositionCommand $Position $Moves
    Send-UciLine $Engine $positionCommand
    $goCommand = Get-GoCommand
    $started = [System.Diagnostics.Stopwatch]::StartNew()
    Send-UciLine $Engine $goCommand

    $infos = [System.Collections.Generic.List[object]]::new()
    $bestMove = $null
    $bestMoveLine = $null
    while ($null -eq $bestMove) {
        $line = Read-UciLine $Engine 'bestmove'
        $info = Parse-InfoLine $line
        if ($null -ne $info) {
            $infos.Add($info)
            continue
        }
        $bestMatch = [regex]::Match($line, '^bestmove\s+(\S+)')
        if ($bestMatch.Success) {
            $bestMove = $bestMatch.Groups[1].Value.ToLowerInvariant()
            $bestMoveLine = $line
        }
    }
    $started.Stop()

    $lastInfo = $null
    if ($infos.Count -gt 0) {
        $lastInfo = $infos[$infos.Count - 1]
    }
    return [pscustomobject]@{
        move = $bestMove
        go_command = $goCommand
        elapsed_ms = [int64]$started.ElapsedMilliseconds
        evaluation = $lastInfo
        infos = @($infos)
        bestmove_line = $bestMoveLine
    }
}

function Invoke-KoiReplay([string]$Fen, [string[]]$Moves = @()) {
    [string[]]$replayArguments = if ($Fen -ceq 'startpos') { @('startpos') } else { @('fen', $Fen) }
    if ($Moves.Count -gt 0) {
        $replayArguments += 'moves'
        $replayArguments += $Moves
    }
    $output = & $replayExecutable @replayArguments
    if ($LASTEXITCODE -ne 0) {
        throw "koi-replay exited with ${LASTEXITCODE}: $($output -join ' | ')"
    }

    $fields = @{}
    foreach ($line in @($output)) {
        $parts = $line -split ' ', 2
        if ($parts.Count -eq 2) {
            $fields[$parts[0]] = $parts[1]
        }
    }
    foreach ($field in @('legal', 'result', 'termination', 'fen')) {
        if (-not $fields.ContainsKey($field)) {
            throw "koi-replay omitted '$field': $($output -join ' | ')"
        }
    }
    if ($fields.legal -notin @('0', '1')) {
        throw "koi-replay returned invalid legality '$($fields.legal)'."
    }
    return [pscustomobject]@{
        legal = ($fields.legal -eq '1')
        result = $fields.result
        termination = $fields.termination
        fen = $fields.fen
    }
}

function Get-SideFromFen([string]$Fen) {
    return ($Fen -split '\s+')[1]
}

function Get-Winner([string]$Result) {
    if ($Result -ceq '1-0') {
        return 'white'
    }
    if ($Result -ceq '0-1') {
        return 'black'
    }
    return $null
}

function Set-EngineProcessStatus($Status, $Engine, [string]$Value) {
    if ($Engine.Label -ceq 'Koi') {
        $Status.koi = $Value
    } else {
        $Status.opponent = $Value
    }
}

function Get-InitialSide([string]$Fen) {
    if ($Fen -ceq 'startpos') {
        return 'w'
    }
    return ($Fen -split '\s+')[1]
}

function Get-SideForPly([string]$InitialSide, [int]$Ply) {
    if (($Ply % 2) -eq 0) {
        return $InitialSide
    }
    if ($InitialSide -ceq 'w') {
        return 'b'
    }
    return 'w'
}

function Get-PgnHeaderValue([string]$Value) {
    return $Value.Replace('"', '')
}

function Format-Pgn($Position, $Game, [string]$WhiteName, [string]$BlackName, [string]$TimeControl) {
    $headers = [System.Collections.Generic.List[string]]::new()
    $headers.Add('[Event "Koi Engine UCI match"]')
    $headers.Add('[Site "local"]')
    $headers.Add(('[Date "{0}"]' -f (Get-Date).ToUniversalTime().ToString('yyyy.MM.dd')))
    $headers.Add(('[Round "{0}"]' -f $Game.round))
    $headers.Add(('[White "{0}"]' -f (Get-PgnHeaderValue $WhiteName)))
    $headers.Add(('[Black "{0}"]' -f (Get-PgnHeaderValue $BlackName)))
    $headers.Add(('[Result "{0}"]' -f $Game.result))
    $headers.Add('[MoveFormat "UCI coordinate notation"]')
    $headers.Add(('[TimeControl "{0}"]' -f (Get-PgnHeaderValue $TimeControl)))
    if ($Position.Fen -cne 'startpos') {
        $headers.Add('[SetUp "1"]')
        $headers.Add(('[FEN "{0}"]' -f (Get-PgnHeaderValue $Position.Fen)))
    }

    $fenFields = if ($Position.Fen -ceq 'startpos') { @('start', 'w', '-', '-', '0', '1') } else {
        $Position.Fen -split '\s+'
    }
    $moveNumber = if ($Position.Fen -ceq 'startpos') { 1 } else { [int]$fenFields[5] }
    $previousSide = $null
    $moveTokens = [System.Collections.Generic.List[string]]::new()
    foreach ($move in @($Game.moves | Where-Object { $_.replay_legal })) {
        if ($move.side -ceq 'w') {
            $moveTokens.Add("$moveNumber. $($move.move)")
        } elseif ($previousSide -ne 'w') {
            $moveTokens.Add("$moveNumber... $($move.move)")
        } else {
            $moveTokens.Add($move.move)
        }
        if ($move.side -ceq 'b') {
            ++$moveNumber
        }
        $previousSide = $move.side
    }
    $moveTokens.Add($Game.result)
    return (($headers -join "`r`n") + "`r`n`r`n" + ($moveTokens -join ' ') + "`r`n")
}

$koiExecutable = Resolve-Executable $KoiPath 'Koi'
$opponentExecutable = Resolve-Executable $OpponentPath 'opponent'
$defaultReplayPath = Join-Path (Split-Path -Parent $koiExecutable) 'koi-replay.exe'
$replayExecutable = Resolve-Executable $(if ([string]::IsNullOrWhiteSpace($ReplayPath)) { $defaultReplayPath } else { $ReplayPath }) 'koi-replay'
$positions = Read-PositionSpecs
$KoiColor = $KoiColor.ToLowerInvariant()
$maximumThreads = [Math]::Max(1, [Math]::Min(64, [Environment]::ProcessorCount))
$Threads = [Math]::Max(1, [Math]::Min($Threads, $maximumThreads))
$timeControl = if ($MovetimeMs -gt 0) { "movetime $MovetimeMs" } elseif ($Nodes -gt 0) {
    "nodes $Nodes"
} else {
    "depth $Depth"
}

$koiEngine = $null
$opponentEngine = $null
$koiStop = [pscustomobject]@{ status = 'process exit'; exit_code = $null; stderr = '' }
$opponentStop = [pscustomobject]@{ status = 'process exit'; exit_code = $null; stderr = '' }
$gameRecords = [System.Collections.Generic.List[object]]::new()
$pgnGames = [System.Collections.Generic.List[string]]::new()

try {
    $koiEngine = Start-UciEngine $koiExecutable 'Koi'
    $opponentEngine = Start-UciEngine $opponentExecutable 'Opponent'
    Initialize-UciEngine $koiEngine
    Initialize-UciEngine $opponentEngine
    $stopMatches = $false

    foreach ($position in $positions) {
        if ($stopMatches) {
            break
        }
        for ($gameIndex = 1; $gameIndex -le $Games; ++$gameIndex) {
            if ($stopMatches) {
                break
            }
            Send-UciLine $koiEngine 'ucinewgame'
            Send-UciLine $opponentEngine 'ucinewgame'
            Wait-UciReady $koiEngine 'new-game readyok'
            Wait-UciReady $opponentEngine 'new-game readyok'

            $moves = [System.Collections.Generic.List[string]]::new()
            $moveRecords = [System.Collections.Generic.List[object]]::new()
            $replay = Invoke-KoiReplay $position.Fen
            $rootFen = $replay.fen
            $result = $replay.result
            $termination = if ($result -ne '*') { $replay.termination } else { $null }
            $processStatus = [pscustomobject][ordered]@{ koi = 'running'; opponent = 'running' }

            for ($ply = 0; $ply -lt $MaxPlies; ++$ply) {
                if ($null -ne $termination) {
                    break
                }
                $side = Get-SideFromFen $rootFen
                $koiTurn = ($side -ceq 'w' -and $KoiColor -ceq 'white') -or
                    ($side -ceq 'b' -and $KoiColor -ceq 'black')
                $engine = if ($koiTurn) { $koiEngine } else { $opponentEngine }
                $positionCommand = Format-PositionCommand $position $moves
                try {
                    $search = Search-UciEngine $engine $position $moves
                } catch {
                    $termination = if ($_.Exception -is [System.TimeoutException]) { 'timeout' } else { 'process exit' }
                    $result = '*'
                    Set-EngineProcessStatus $processStatus $engine $termination
                    $engine.StopResult = Stop-UciEngine $engine ($termination -eq 'timeout')
                    $engine.Retired = $true
                    $stopMatches = $true
                    break
                }
                $record = [ordered]@{
                    ply = $ply + 1
                    root_fen = $rootFen
                    side = $side
                    engine = $engine.Name
                    engine_label = $engine.Label
                    position_command = $positionCommand
                    go_command = $search.go_command
                    move = $search.move
                    replay_legal = $false
                    elapsed_ms = $search.elapsed_ms
                    final_info = $search.evaluation
                    evaluation = $search.evaluation
                    all_info_lines = @($search.infos | ForEach-Object { $_.raw })
                    infos = $search.infos
                    bestmove_line = $search.bestmove_line
                }

                if ($search.move -ceq '0000') {
                    $termination = 'illegal move'
                    $result = '*'
                    $moveRecords.Add([pscustomobject]$record)
                    break
                }
                try {
                    [string[]]$candidateMoves = @($moves) + @($search.move)
                    $moveReplay = Invoke-KoiReplay $position.Fen $candidateMoves
                } catch {
                    $termination = 'process exit'
                    $result = '*'
                    $moveRecords.Add([pscustomobject]$record)
                    break
                }
                $record.replay_legal = $moveReplay.legal
                $moveRecords.Add([pscustomobject]$record)
                if (-not $moveReplay.legal) {
                    $termination = 'illegal move'
                    $result = '*'
                    break
                }

                $moves.Add($search.move)
                $rootFen = $moveReplay.fen
                $result = $moveReplay.result
                if ($result -ne '*') {
                    $termination = $moveReplay.termination
                }
            }

            if ($null -eq $termination) {
                $termination = 'max plies'
                $result = '*'
            }

            $whiteName = if ($KoiColor -ceq 'white') { $koiEngine.Name } else { $opponentEngine.Name }
            $blackName = if ($KoiColor -ceq 'black') { $koiEngine.Name } else { $opponentEngine.Name }
            $game = [ordered]@{
                position = $position.Name
                initial_fen = $position.Fen
                round = $gameIndex
                koi_color = $KoiColor
                result = $result
                winner = Get-Winner $result
                termination = $termination
                process_status = $processStatus
                moves = @($moveRecords)
            }
            $gameObject = [pscustomobject]$game
            $gameRecords.Add($gameObject)
            $pgnGames.Add((Format-Pgn $position $gameObject $whiteName $blackName $timeControl))
        }
    }
}
finally {
    $koiStop = if ($null -ne $koiEngine -and $koiEngine.Retired -and $null -ne $koiEngine.StopResult) {
        $koiEngine.StopResult
    } else {
        Stop-UciEngine $koiEngine
    }
    $opponentStop = if ($null -ne $opponentEngine -and $opponentEngine.Retired -and $null -ne $opponentEngine.StopResult) {
        $opponentEngine.StopResult
    } else {
        Stop-UciEngine $opponentEngine
    }
    foreach ($game in $gameRecords) {
        if ($game.process_status.koi -eq 'running') {
            $game.process_status.koi = $koiStop.status
        }
        if ($game.process_status.opponent -eq 'running') {
            $game.process_status.opponent = $opponentStop.status
        }
    }
}

New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$resolvedOutputDirectory = (Resolve-Path -LiteralPath $OutputDirectory).Path
$stamp = (Get-Date).ToUniversalTime().ToString('yyyyMMdd-HHmmss-fff')
$basePath = Join-Path $resolvedOutputDirectory "koi-uci-match-$stamp"

$engineSummary = @(
    [ordered]@{
        label = 'Koi'
        path = $koiExecutable
        name = $koiEngine.Name
        identity = @($koiEngine.Handshake | Where-Object { $_ -match '^id ' })
        handshake = @($koiEngine.Handshake)
        options = @($koiEngine.SentOptions)
        process_status = $koiStop.status
        exit_code = $koiStop.exit_code
        stderr = $koiStop.stderr
    },
    [ordered]@{
        label = 'Opponent'
        path = $opponentExecutable
        name = $opponentEngine.Name
        identity = @($opponentEngine.Handshake | Where-Object { $_ -match '^id ' })
        handshake = @($opponentEngine.Handshake)
        options = @($opponentEngine.SentOptions)
        process_status = $opponentStop.status
        exit_code = $opponentStop.exit_code
        stderr = $opponentStop.stderr
    }
)

$report = [ordered]@{
    schema = 'koi-uci-match-v2'
    generated_utc = (Get-Date).ToUniversalTime().ToString('o')
    configuration = [ordered]@{
        depth = $Depth
        movetime_ms = $MovetimeMs
        nodes = $Nodes
        threads = $Threads
        speed = $Speed
        hash_mb = $Hash
        max_plies = $MaxPlies
        timeout_ms = $TimeoutMilliseconds
        games_per_position = $Games
        koi_color = $KoiColor
        time_control = $timeControl
    }
    engines = $engineSummary
    positions = @($positions)
    games = @($gameRecords)
}

$jsonPath = "$basePath.json"
$pgnPath = "$basePath.pgn"
$report | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $jsonPath -Encoding UTF8
($pgnGames -join "`r`n") | Set-Content -LiteralPath $pgnPath -Encoding UTF8

Write-Output "json $jsonPath"
Write-Output "pgn $pgnPath"
