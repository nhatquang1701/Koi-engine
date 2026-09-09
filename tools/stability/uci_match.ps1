[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$KoiPath,

    [Parameter(Mandatory = $true)]
    [string]$OpponentPath,

    [string]$ReplayPath,

    [string]$FenFile,

    [string]$OpeningFile,

    [ValidatePattern('^[1-9][0-9]*\+[0-9]+$')]
    [string]$TimeControl,

    [string]$OutputDirectory = '',

    [ValidateRange(1, 64)]
    [int]$Depth = 4,

    [ValidateRange(1, 100)]
    [int]$Games = 1,

    [ValidateRange(0, 600000)]
    [int]$MovetimeMs = 0,

    [uint64]$Nodes = 0,

    [uint64]$KoiRandomSeed = 1,

    [ValidateSet('true', 'false', '1', '0')]
    [string]$KoiOwnBook = 'true',

    [string]$KoiBookFile = 'book.bin',

    [ValidateRange(0, 40)]
    [int]$KoiBookDepth = 16,

    [ValidateSet('true', 'false', '1', '0')]
    [string]$KoiBookRandom = 'false',

    [ValidateRange(1, 64)]
    [int]$Threads = 1,

    [ValidateRange(1, 100)]
    [int]$Speed = 100,

    [ValidateRange(1, 4096)]
    [int]$Hash = 16,

    [Alias('StockfishElo')]
    [Nullable[int]]$OpponentElo,

    [ValidateRange(1, 512)]
    [int]$MaxPlies = 512,

    [ValidateRange(1000, 60000)]
    [int]$TimeoutMilliseconds = 5000,

    [ValidateSet('white', 'black')]
    [string]$KoiColor = 'white',

    [ValidatePattern('^[^\s]+$')]
    [string]$BatchId = 'batch-00',

    [ValidateSet('measurement', 'before', 'after')]
    [string]$RunLabel = 'measurement'
)

$ErrorActionPreference = 'Stop'
$repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $repositoryRoot ('artifacts\matches\run-' +
        (Get-Date).ToUniversalTime().ToString('yyyyMMdd-HHmmss-fff'))
}
$KoiOwnBookEnabled = $KoiOwnBook -in @('true', '1')
$KoiBookRandomEnabled = $KoiBookRandom -in @('true', '1')
$OpponentEloAnchor = $null
if ($null -ne $OpponentElo) {
    $OpponentEloAnchor = [Math]::Max(1320, [Math]::Min(3190, $OpponentElo))
}

if ($MovetimeMs -gt 0 -and $Nodes -gt 0) {
    throw 'Choose at most one of -MovetimeMs and -Nodes.'
}
if (-not [string]::IsNullOrWhiteSpace($TimeControl) -and ($MovetimeMs -gt 0 -or $Nodes -gt 0)) {
    throw 'Choose -TimeControl or one of -MovetimeMs and -Nodes.'
}
if (-not [string]::IsNullOrWhiteSpace($FenFile) -and -not [string]::IsNullOrWhiteSpace($OpeningFile)) {
    throw 'Choose at most one of -FenFile and -OpeningFile.'
}

function Resolve-Executable([string]$Path, [string]$Label) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "${Label} executable is missing: $Path"
    }
    return (Resolve-Path -LiteralPath $Path).Path
}

function Read-PositionSpecs {
    if ([string]::IsNullOrWhiteSpace($FenFile)) {
        return @([pscustomobject]@{ Name = 'startpos'; Fen = 'startpos'; Moves = @() })
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
        $specs.Add([pscustomobject]@{ Name = $name; Fen = $fen; Moves = @() })
    }

    if ($specs.Count -eq 0) {
        throw "FEN file contains no positions: $FenFile"
    }
    return @($specs)
}

function Read-OpeningSpecs {
    if ([string]::IsNullOrWhiteSpace($OpeningFile)) {
        return $null
    }
    if (-not (Test-Path -LiteralPath $OpeningFile -PathType Leaf)) {
        throw "Opening file is missing: $OpeningFile"
    }

    $specs = [System.Collections.Generic.List[object]]::new()
    foreach ($line in Get-Content -LiteralPath $OpeningFile) {
        $trimmed = $line.Trim()
        if ($trimmed.Length -eq 0 -or $trimmed.StartsWith('#')) {
            continue
        }
        $match = [regex]::Match($trimmed, '^(?<name>[^|#\s]+)\s*\|\s*(?<moves>[a-h][1-8][a-h][1-8][nbrq]?(?:\s+[a-h][1-8][a-h][1-8][nbrq]?)*?)\s*$')
        if (-not $match.Success) {
            throw "Invalid opening file line. Expected name | UCI move UCI move: $trimmed"
        }
        $specs.Add([pscustomobject]@{
            Name = $match.Groups['name'].Value
            Fen = 'startpos'
            Moves = @($match.Groups['moves'].Value -split '\s+' | ForEach-Object { $_.ToLowerInvariant() })
        })
    }
    if ($specs.Count -eq 0) {
        throw "Opening file contains no positions: $OpeningFile"
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

function Read-UciLine($Engine, [string]$Description, [int]$WaitMilliseconds = $TimeoutMilliseconds) {
    if ($WaitMilliseconds -le 0) {
        throw [System.TimeoutException]::new("Timed out waiting for ${Engine.Label} ${Description}.")
    }
    $readTask = $Engine.Process.StandardOutput.ReadLineAsync()
    if (-not $readTask.Wait($WaitMilliseconds)) {
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
    if ($Engine.Label -ceq 'Koi') {
        foreach ($option in @(
            "setoption name RandomSeed value $KoiRandomSeed",
            "setoption name OwnBook value $($KoiOwnBookEnabled.ToString().ToLowerInvariant())",
            "setoption name BookFile value $KoiBookFile",
            "setoption name BookDepth value $KoiBookDepth",
            "setoption name BookRandom value $($KoiBookRandomEnabled.ToString().ToLowerInvariant())"
        )) {
            Send-UciLine $Engine $option
            $Engine.SentOptions.Add($option)
        }
    }
    if ($Engine.Label -ceq 'Opponent' -and $null -ne $OpponentEloAnchor) {
        foreach ($option in @(
            'setoption name UCI_LimitStrength value true',
            "setoption name UCI_Elo value $OpponentEloAnchor"
        )) {
            Send-UciLine $Engine $option
            $Engine.SentOptions.Add($option)
        }
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

function Get-GoCommand($Clock) {
    if ($null -ne $Clock) {
        return "go wtime $($Clock.white_ms) btime $($Clock.black_ms) winc $($Clock.increment_ms) binc $($Clock.increment_ms)"
    }
    if ($MovetimeMs -gt 0) {
        return "go movetime $MovetimeMs"
    }
    if ($Nodes -gt 0) {
        return "go nodes $Nodes"
    }
    return "go depth $Depth"
}

function Get-ClockRemainingMilliseconds($Clock, [string]$Side) {
    if ($null -eq $Clock) {
        return $null
    }
    $property = if ($Side -ceq 'w') { 'white_ms' } else { 'black_ms' }
    return [int64]$Clock.$property
}

function Update-Clock($Clock, [string]$Side, [int64]$ElapsedMilliseconds) {
    if ($null -eq $Clock) {
        return
    }
    $property = if ($Side -ceq 'w') { 'white_ms' } else { 'black_ms' }
    $remaining = [int64]$Clock.$property
    if ($ElapsedMilliseconds -gt $remaining) {
        throw [System.TimeoutException]::new("${Side} exceeded its chess clock by $($ElapsedMilliseconds - $remaining) ms.")
    }
    $Clock.$property = ($remaining - $ElapsedMilliseconds) + $Clock.increment_ms
}

function Parse-InfoLine([string]$Line) {
    if ($Line -notmatch '^info(?:\s|$)' -or $Line -match '^info string book move\s+') {
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

function Search-UciEngine($Engine, $Position, $Moves, $Clock, [string]$Side) {
    $positionCommand = Format-PositionCommand $Position $Moves
    Send-UciLine $Engine $positionCommand
    $goCommand = Get-GoCommand $Clock
    $clockDeadlineMilliseconds = Get-ClockRemainingMilliseconds $Clock $Side
    $started = [System.Diagnostics.Stopwatch]::StartNew()
    Send-UciLine $Engine $goCommand

    $infos = [System.Collections.Generic.List[object]]::new()
    $allInfoLines = [System.Collections.Generic.List[string]]::new()
    $bestMove = $null
    $bestMoveLine = $null
    $bookUsed = $false
    $bookMove = $null
    while ($null -eq $bestMove) {
        # Handshakes/readiness remain bounded by the protocol timeout. A clocked
        # search instead waits through its actual chess-clock deadline, which
        # prevents a short protocol timeout from becoming a false move deadline.
        $waitMilliseconds = $TimeoutMilliseconds
        if ($null -ne $clockDeadlineMilliseconds) {
            $remainingMilliseconds = $clockDeadlineMilliseconds - [int64]$started.ElapsedMilliseconds
            if ($remainingMilliseconds -le 0) {
                throw [System.TimeoutException]::new("${Engine.Label} exceeded its chess clock before bestmove.")
            }
            $waitMilliseconds = [int][Math]::Min([int64][int]::MaxValue, $remainingMilliseconds)
        }
        $line = Read-UciLine $Engine 'bestmove' $waitMilliseconds
        $bookMatch = [regex]::Match($line, '^info string book move\s+([a-h][1-8][a-h][1-8][nbrq]?)\s+depth\s+(\d+)\s*$')
        if ($bookMatch.Success) {
            if ($bookUsed) {
                throw "Engine emitted more than one book marker before bestmove: $line"
            }
            $bookUsed = $true
            $bookMove = $bookMatch.Groups[1].Value.ToLowerInvariant()
            $allInfoLines.Add($line)
            continue
        }
        $info = Parse-InfoLine $line
        if ($null -ne $info) {
            $infos.Add($info)
            $allInfoLines.Add($line)
            continue
        }
        $bestMatch = [regex]::Match($line, '^bestmove\s+(\S+)')
        if ($bestMatch.Success) {
            $bestMove = $bestMatch.Groups[1].Value.ToLowerInvariant()
            if ($null -ne $clockDeadlineMilliseconds -and [int64]$started.ElapsedMilliseconds -gt $clockDeadlineMilliseconds) {
                throw [System.TimeoutException]::new("${Engine.Label} exceeded its chess clock before bestmove.")
            }
            if ($bookUsed -and $bestMove -ne $bookMove) {
                throw "Book marker '$bookMove' does not match bestmove '$bestMove'."
            }
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
        all_info_lines = @($allInfoLines)
        bestmove_line = $bestMoveLine
        book_used = $bookUsed
        book_move = $bookMove
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

function Get-EngineVersion([string]$Name) {
    $match = [regex]::Match($Name, '\b(?:v)?(\d+(?:\.\d+)+[-+._A-Za-z0-9]*)\b')
    if ($match.Success) {
        return $match.Groups[1].Value
    }
    return 'unknown'
}

function Get-EngineSha256([string]$Path) {
    $hashAlgorithm = [System.Security.Cryptography.SHA256]::Create()
    $stream = [System.IO.File]::OpenRead($Path)
    try {
        return (($hashAlgorithm.ComputeHash($stream) | ForEach-Object { $_.ToString('x2') }) -join '')
    } finally {
        $stream.Dispose()
        $hashAlgorithm.Dispose()
    }
}

function Get-HardwareMetadata {
    return [ordered]@{
        os = [Environment]::OSVersion.VersionString
        machine = if ([string]::IsNullOrWhiteSpace($env:PROCESSOR_ARCHITECTURE)) { 'unknown' } else { $env:PROCESSOR_ARCHITECTURE }
        processor = if ([string]::IsNullOrWhiteSpace($env:PROCESSOR_IDENTIFIER)) { 'unknown' } else { $env:PROCESSOR_IDENTIFIER }
        cpu_count = [Environment]::ProcessorCount
        powershell_version = $PSVersionTable.PSVersion.ToString()
    }
}

function Get-PositionClassification([string]$Fen, [int]$Ply) {
    $fields = if ($Fen -ceq 'startpos') { @('start', 'w', 'KQkq', '-') } else { $Fen -split '\s+' }
    $placement = [string]$fields[0]
    $pieceCount = 0
    $nonPawnNonKingCount = 0
    foreach ($character in $placement.ToCharArray()) {
        if ($character -match '[prnbqkPRNBQK]') {
            ++$pieceCount
        }
        if ($character -match '[rnbqRNBQ]') {
            ++$nonPawnNonKingCount
        }
    }
    $phase = if ($Ply -le 20) { 'opening' } elseif ($pieceCount -le 10 -or $nonPawnNonKingCount -le 4) { 'endgame' } else { 'middlegame' }
    return [ordered]@{
        phase = $phase
        side_to_move = if ([string]$fields[1] -ceq 'w') { 'white' } else { 'black' }
        piece_count = $pieceCount
        non_pawn_non_king_count = $nonPawnNonKingCount
        castling_rights = [string]$fields[2]
        en_passant = [string]$fields[3]
    }
}

function Get-TimeControlMetadata {
    if (-not [string]::IsNullOrWhiteSpace($TimeControl)) {
        $parts = $TimeControl -split '\+'
        return [ordered]@{ kind = 'clock'; value = $TimeControl; initial_minutes = [int]$parts[0]; increment_seconds = [int]$parts[1] }
    }
    if ($MovetimeMs -gt 0) {
        return [ordered]@{ kind = 'movetime'; value = "movetime $MovetimeMs"; movetime_ms = $MovetimeMs }
    }
    if ($Nodes -gt 0) {
        return [ordered]@{ kind = 'nodes'; value = "nodes $Nodes"; nodes = [uint64]$Nodes }
    }
    return [ordered]@{ kind = 'depth'; value = "depth $Depth"; depth = $Depth }
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
$positions = Read-OpeningSpecs
if ($null -eq $positions) {
    $positions = Read-PositionSpecs
}
$KoiColor = $KoiColor.ToLowerInvariant()
$maximumThreads = [Math]::Max(1, [Math]::Min(64, [Environment]::ProcessorCount))
$Threads = [Math]::Max(1, [Math]::Min($Threads, $maximumThreads))
$pgnTimeControl = if (-not [string]::IsNullOrWhiteSpace($TimeControl)) { $TimeControl } elseif ($MovetimeMs -gt 0) { "movetime $MovetimeMs" } elseif ($Nodes -gt 0) {
    "nodes $Nodes"
} else {
    "depth $Depth"
}
$clockParts = if (-not [string]::IsNullOrWhiteSpace($TimeControl)) { $TimeControl -split '\+' } else { $null }
$clockInitialMilliseconds = if ($null -ne $clockParts) { [int64]$clockParts[0] * 60000 } else { [int64]0 }
$clockIncrementMilliseconds = if ($null -ne $clockParts) { [int64]$clockParts[1] * 1000 } else { [int64]0 }

foreach ($position in $positions) {
    if ($position.Moves.Count -gt 0) {
        $openingReplay = Invoke-KoiReplay $position.Fen $position.Moves
        if (-not $openingReplay.legal) {
            throw "Opening '$($position.Name)' contains an illegal move sequence."
        }
    }
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

            $moveRecords = [System.Collections.Generic.List[object]]::new()
            $moves = [System.Collections.Generic.List[string]]::new()
            $replay = Invoke-KoiReplay $position.Fen
            $rootFen = $replay.fen
            $result = $replay.result
            $termination = if ($result -ne '*') { $replay.termination } else { $null }
            $processStatus = [pscustomobject][ordered]@{ koi = 'running'; opponent = 'running' }
            $clock = if ($null -ne $clockParts) {
                [pscustomobject]@{ white_ms = $clockInitialMilliseconds; black_ms = $clockInitialMilliseconds; increment_ms = $clockIncrementMilliseconds }
            } else { $null }

            foreach ($openingMove in $position.Moves) {
                $side = Get-SideFromFen $rootFen
                [string[]]$candidateMoves = @($moves) + @($openingMove)
                $openingMoveReplay = Invoke-KoiReplay $position.Fen $candidateMoves
                $moveRecords.Add([pscustomobject][ordered]@{
                    ply = $moveRecords.Count + 1
                    root_fen = $rootFen
                    side = $side
                    engine = 'opening'
                    engine_label = 'opening'
                    position_command = Format-PositionCommand $position $moves
                    go_command = $null
                    move = $openingMove
                    replay_legal = $openingMoveReplay.legal
                    elapsed_ms = [int64]0
                    final_info = $null
                    evaluation = $null
                    all_info_lines = @()
                    infos = @()
                    bestmove_line = $null
                    book_used = $false
                    book_move = $null
                    position_classification = Get-PositionClassification $rootFen ($moveRecords.Count + 1)
                })
                $moves.Add($openingMove)
                $rootFen = $openingMoveReplay.fen
                $result = $openingMoveReplay.result
                if ($result -ne '*') {
                    $termination = $openingMoveReplay.termination
                }
            }

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
                    $search = Search-UciEngine $engine $position $moves $clock $side
                    Update-Clock $clock $side $search.elapsed_ms
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
                    ply = $moveRecords.Count + 1
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
                    all_info_lines = $search.all_info_lines
                    infos = $search.infos
                    bestmove_line = $search.bestmove_line
                    book_used = $search.book_used
                    book_move = $search.book_move
                    position_classification = Get-PositionClassification $rootFen ($moveRecords.Count + 1)
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
            $pgnGames.Add((Format-Pgn $position $gameObject $whiteName $blackName $pgnTimeControl))
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
        version = Get-EngineVersion $koiEngine.Name
        hashes = [ordered]@{ executable_sha256 = Get-EngineSha256 $koiExecutable }
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
        version = Get-EngineVersion $opponentEngine.Name
        hashes = [ordered]@{ executable_sha256 = Get-EngineSha256 $opponentExecutable }
        identity = @($opponentEngine.Handshake | Where-Object { $_ -match '^id ' })
        handshake = @($opponentEngine.Handshake)
        options = @($opponentEngine.SentOptions)
        process_status = $opponentStop.status
        exit_code = $opponentStop.exit_code
        stderr = $opponentStop.stderr
    }
)

$measurement = [ordered]@{
    network = [ordered]@{
        enabled = $false
        state = 'disabled'
        reason = 'the local UCI match harness performs no network access'
    }
    book = [ordered]@{
        enabled = $KoiOwnBookEnabled
        state = if ($KoiOwnBookEnabled) { 'active' } else { 'disabled' }
        path = if ($KoiOwnBookEnabled) { $KoiBookFile } else { $null }
        random = $KoiBookRandomEnabled
    }
    tablebase = [ordered]@{
        enabled = $false
        state = 'disabled'
        path = $null
    }
    hardware = Get-HardwareMetadata
    time_control = Get-TimeControlMetadata
}

$report = [ordered]@{
    schema = 'koi-uci-match-v2'
    generated_utc = (Get-Date).ToUniversalTime().ToString('o')
    hardware = $measurement.hardware
    measurement = $measurement
    configuration = [ordered]@{
        depth = $Depth
        movetime_ms = $MovetimeMs
        nodes = $Nodes
        threads = $Threads
        speed = $Speed
        hash_mb = $Hash
        max_plies = $MaxPlies
        timeout_ms = $TimeoutMilliseconds
        protocol_timeout_ms = $TimeoutMilliseconds
        games_per_position = $Games
        koi_color = $KoiColor
        time_control = $pgnTimeControl
        koi_random_seed = $KoiRandomSeed
        koi_own_book = $KoiOwnBookEnabled
        koi_book_file = $KoiBookFile
        koi_book_depth = $KoiBookDepth
        koi_book_random = $KoiBookRandomEnabled
        opponent_limit_strength = ($null -ne $OpponentEloAnchor)
        opponent_elo = $OpponentEloAnchor
        batch_id = $BatchId
        run_label = $RunLabel
        time_control_metadata = $measurement.time_control
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
