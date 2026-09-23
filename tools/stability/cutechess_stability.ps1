param(
    [Parameter(Mandatory = $true)]
    [string]$KoiPath,
    [Parameter(Mandatory = $true)]
    [string]$OpponentPath,
    [string]$CutechessPath = 'C:\Program Files (x86)\Cute Chess\cutechess-cli.exe',
    [string]$ReplayPath = '',
    [string]$OpeningFile = '',
    [string]$FenFile = '',
    [string]$OutputDirectory = '',
    [int]$Games = 100,
    [int]$MaxMoves = 200,
    [string]$TimeControl = '1+0',
    [int]$Hash = 512,
    [int]$Threads = 1,
    [int]$Speed = 100,
    [bool]$OwnBook = $false,
    [string]$BookFile = 'book.bin',
    [ValidateRange(0, 40)]
    [int]$BookDepth = 16,
    [ValidateSet('auto', 'white', 'black')]
    [string]$KoiColor = 'auto',
    [ValidateRange(0, 86400000)]
    [int]$TimeoutMilliseconds = 0,
    [switch]$EnableDebug
)

$ErrorActionPreference = 'Stop'
$repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))

function Get-ExecutableHash([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
}

function Assert-File([string]$Path, [string]$Description) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Description is missing: $Path"
    }
    return (Resolve-Path -LiteralPath $Path).Path
}

function Quote-ProcessArgument([string]$Argument) {
    if ($null -eq $Argument -or $Argument.Length -eq 0) {
        return '""'
    }

    $builder = [System.Text.StringBuilder]::new('"')
    $backslashes = 0
    foreach ($character in $Argument.ToCharArray()) {
        if ($character -eq '\') {
            $backslashes++
            continue
        }
        if ($character -eq '"') {
            [void]$builder.Append(('\' * (($backslashes * 2) + 1)))
            [void]$builder.Append('"')
            $backslashes = 0
            continue
        }
        if ($backslashes -gt 0) {
            [void]$builder.Append(('\' * $backslashes))
            $backslashes = 0
        }
        [void]$builder.Append($character)
    }
    if ($backslashes -gt 0) {
        [void]$builder.Append(('\' * ($backslashes * 2)))
    }
    [void]$builder.Append('"')
    return $builder.ToString()
}

function ConvertTo-ProcessCommandLine([string]$Executable, [string[]]$Arguments) {
    return ((@($Executable) + @($Arguments)) | ForEach-Object {
        Quote-ProcessArgument ([string]$_)
    }) -join ' '
}

function Get-SystemDescription {
    $os = $null
    $cpu = $null
    try {
        $os = [System.Runtime.InteropServices.RuntimeInformation]::OSDescription
    } catch {
        $os = [System.Environment]::OSVersion.VersionString
    }
    try {
        $cpu = (Get-CimInstance -ClassName Win32_Processor -ErrorAction Stop |
            Select-Object -First 1 -ExpandProperty Name)
    } catch {
        try {
            $cpu = [System.Runtime.InteropServices.RuntimeInformation]::ProcessArchitecture.ToString()
        } catch {
            $cpu = [System.Environment]::GetEnvironmentVariable('PROCESSOR_IDENTIFIER')
        }
    }
    if ([string]::IsNullOrWhiteSpace($cpu)) {
        $cpu = [System.Environment]::GetEnvironmentVariable('PROCESSOR_IDENTIFIER')
    }
    return [ordered]@{ os = [string]$os; cpu = [string]$cpu }
}

function Add-ArtifactLine([System.IO.StreamWriter]$Writer, [string]$Line) {
    $Writer.WriteLine($Line)
    $Writer.Flush()
}

function Add-CutechessLine($State, [string]$Stream, [string]$Line) {
    $protocolRecord = [ordered]@{
        event = 'cutechess_output'
        utc = (Get-Date).ToUniversalTime().ToString('o')
        stream = $Stream
        text = $Line
    }
    Add-ArtifactLine $State.ProtocolWriter ($protocolRecord | ConvertTo-Json -Compress)
    if ($Stream -eq 'stdout') {
        Add-ArtifactLine $State.StdoutWriter $Line
        Add-ArtifactLine $State.TranscriptWriter $Line
        [Console]::Out.WriteLine($Line)
    } else {
        Add-ArtifactLine $State.StderrWriter $Line
        Add-ArtifactLine $State.TranscriptWriter ("[stderr] " + $Line)
        [Console]::Error.WriteLine($Line)
    }
    [void]$State.Lines.Add($Line)
}

function Stop-CutechessProcess($Process) {
    if ($null -eq $Process) {
        return
    }
    try {
        if ([System.Environment]::OSVersion.Platform -eq [System.PlatformID]::Win32NT) {
            & taskkill.exe /PID $Process.Id /T /F 2>$null | Out-Null
        } else {
            $Process.Kill()
        }
    } catch {
        try {
            $Process.Kill()
        } catch {
        }
    }
}

function Invoke-Cutechess([string]$Executable, [string[]]$Arguments, [string]$TranscriptPath,
                           [string]$StdoutPath, [string]$StderrPath,
                           [string]$ProtocolPath, [int]$TimeoutMs) {
    $lines = [System.Collections.Generic.List[string]]::new()
    $transcriptWriter = [System.IO.StreamWriter]::new($TranscriptPath, $false, [System.Text.UTF8Encoding]::new($false))
    $stdoutWriter = [System.IO.StreamWriter]::new($StdoutPath, $false, [System.Text.UTF8Encoding]::new($false))
    $stderrWriter = [System.IO.StreamWriter]::new($StderrPath, $false, [System.Text.UTF8Encoding]::new($false))
    $protocolWriter = [System.IO.StreamWriter]::new($ProtocolPath, $false, [System.Text.UTF8Encoding]::new($false))
    $state = [pscustomobject]@{
        Lines = $lines
        TranscriptWriter = $transcriptWriter
        StdoutWriter = $stdoutWriter
        StderrWriter = $stderrWriter
        ProtocolWriter = $protocolWriter
    }
    $process = $null
    $timedOut = $false
    $startError = $null
    $exitCode = $null
    $processId = $null
    $started = Get-Date
    try {
        $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
        $startInfo.FileName = $Executable
        $startInfo.Arguments = (@($Arguments | ForEach-Object { Quote-ProcessArgument ([string]$_) }) -join ' ')
        $startInfo.UseShellExecute = $false
        $startInfo.CreateNoWindow = $true
        $startInfo.RedirectStandardOutput = $true
        $startInfo.RedirectStandardError = $true

        $process = [System.Diagnostics.Process]::new()
        $process.StartInfo = $startInfo
        if (-not $process.Start()) {
            throw "Unable to start Cutechess: $Executable"
        }
        $processId = $process.Id
        $stdoutTask = $process.StandardOutput.ReadLineAsync()
        $stderrTask = $process.StandardError.ReadLineAsync()
        $stdoutOpen = $true
        $stderrOpen = $true
        $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
        while ($stdoutOpen -or $stderrOpen -or -not $process.HasExited) {
            if ($stdoutOpen -and $stdoutTask.IsCompleted) {
                $line = $stdoutTask.GetAwaiter().GetResult()
                if ($null -eq $line) {
                    $stdoutOpen = $false
                } else {
                    Add-CutechessLine $state 'stdout' $line
                    $stdoutTask = $process.StandardOutput.ReadLineAsync()
                }
            }
            if ($stderrOpen -and $stderrTask.IsCompleted) {
                $line = $stderrTask.GetAwaiter().GetResult()
                if ($null -eq $line) {
                    $stderrOpen = $false
                } else {
                    Add-CutechessLine $state 'stderr' $line
                    $stderrTask = $process.StandardError.ReadLineAsync()
                }
            }
            if ($TimeoutMs -gt 0 -and $stopwatch.ElapsedMilliseconds -ge $TimeoutMs) {
                $timedOut = $true
                Stop-CutechessProcess $process
                break
            }
            Start-Sleep -Milliseconds 25
        }
        $process.WaitForExit()
        while ($stdoutOpen -or $stderrOpen) {
            if ($stdoutOpen -and $stdoutTask.IsCompleted) {
                $line = $stdoutTask.GetAwaiter().GetResult()
                if ($null -eq $line) {
                    $stdoutOpen = $false
                } else {
                    Add-CutechessLine $state 'stdout' $line
                    $stdoutTask = $process.StandardOutput.ReadLineAsync()
                }
            }
            if ($stderrOpen -and $stderrTask.IsCompleted) {
                $line = $stderrTask.GetAwaiter().GetResult()
                if ($null -eq $line) {
                    $stderrOpen = $false
                } else {
                    Add-CutechessLine $state 'stderr' $line
                    $stderrTask = $process.StandardError.ReadLineAsync()
                }
            }
            if ($stdoutOpen -or $stderrOpen) {
                Start-Sleep -Milliseconds 10
            }
        }
        $exitCode = $process.ExitCode
    } catch {
        $startError = $_.Exception.ToString()
        if ($null -ne $process -and -not $process.HasExited) {
            Stop-CutechessProcess $process
            try { $process.WaitForExit() } catch { }
        }
    } finally {
        if ($null -ne $process) {
            $process.Dispose()
        }
        $transcriptWriter.Dispose()
        $stdoutWriter.Dispose()
        $stderrWriter.Dispose()
        $protocolWriter.Dispose()
    }
    return [pscustomobject]@{
        lines = @($lines)
        exit_code = $exitCode
        process_id = $processId
        timed_out = $timedOut
        start_error = $startError
        started_utc = $started.ToUniversalTime().ToString('o')
    }
}

if ($Games -lt 1) {
    throw 'Games must be at least 1.'
}
if ($MaxMoves -lt 1) {
    throw 'MaxMoves must be at least 1.'
}
if ($Hash -lt 1 -or $Hash -gt 4096) {
    throw 'Hash must be between 1 and 4096 MB.'
}
if ($Threads -lt 1 -or $Threads -gt 64) {
    throw 'Threads must be between 1 and 64.'
}
if ($Speed -lt 1 -or $Speed -gt 100) {
    throw 'Speed must be between 1 and 100.'
}
if ($BookDepth -lt 0 -or $BookDepth -gt 40) {
    throw 'BookDepth must be between 0 and 40.'
}

$koi = Assert-File $KoiPath 'Koi executable'
$opponent = Assert-File $OpponentPath 'Opponent executable'
$cutechess = Assert-File $CutechessPath 'Cutechess executable'
$koiHash = Get-ExecutableHash $koi
$opponentHash = Get-ExecutableHash $opponent
$cutechessHash = Get-ExecutableHash $cutechess
$opponentIsKoi = $koiHash -eq $opponentHash
$bookOption = $BookFile
$bookResolved = $null
if ([System.IO.Path]::IsPathRooted($BookFile)) {
    $bookResolved = $BookFile
} elseif (Test-Path -LiteralPath $BookFile -PathType Leaf) {
    $bookResolved = (Resolve-Path -LiteralPath $BookFile).Path
    $bookOption = $bookResolved
} else {
    $bookResolved = Join-Path (Split-Path -Parent $koi) $BookFile
}
$replay = $null
if (-not [string]::IsNullOrWhiteSpace($ReplayPath)) {
    $replay = Assert-File $ReplayPath 'Replay executable'
} elseif (-not [string]::IsNullOrWhiteSpace($KoiPath)) {
    $binarySuffix = if ($env:OS -eq 'Windows_NT') { '.exe' } else { '' }
    $candidateReplay = Join-Path (Split-Path -Parent $koi) "koi-replay$binarySuffix"
    if (Test-Path -LiteralPath $candidateReplay -PathType Leaf) {
        $replay = (Resolve-Path -LiteralPath $candidateReplay).Path
    }
}
if (-not [string]::IsNullOrWhiteSpace($OpeningFile)) {
    $opening = Assert-File $OpeningFile 'Opening file'
    $extension = [System.IO.Path]::GetExtension($opening).ToLowerInvariant()
    if ($extension -notin @('.pgn', '.epd', '.txt')) {
        throw 'OpeningFile must use .pgn, .epd, or name|UCI-moves .txt format.'
    }
    if ($extension -eq '.txt' -and $null -eq $replay) {
        throw 'A replay executable is required to convert a .txt UCI opening file.'
    }
}
if (-not [string]::IsNullOrWhiteSpace($FenFile)) {
    $fen = Assert-File $FenFile 'FEN file'
}

if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $repositoryRoot ('artifacts/stability/run-' +
        (Get-Date).ToUniversalTime().ToString('yyyyMMdd-HHmmss-fff'))
}
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$OutputDirectory = (Resolve-Path -LiteralPath $OutputDirectory).Path

function Convert-FenToEpdRecord([string]$Name, [string]$FullFen) {
    $fields = @($FullFen.Trim() -split '\s+')
    if ($fields.Count -ne 6) {
        throw "FEN entry '$Name' must contain exactly six fields."
    }
    $escapedName = $Name.Replace('"', "'")
    return "$($fields[0]) $($fields[1]) $($fields[2]) $($fields[3]) hmvc $($fields[4]); fmvn $($fields[5]); id `"$escapedName`";"
}

function Convert-FenFileToEpd([string]$SourcePath, [string]$DestinationPath) {
    $records = [System.Collections.Generic.List[string]]::new()
    foreach ($line in Get-Content -LiteralPath $SourcePath) {
        $trimmed = $line.Trim()
        if ($trimmed.Length -eq 0 -or $trimmed.StartsWith('#')) {
            continue
        }
        $match = [regex]::Match($trimmed, '^(?<name>[^|#\s]+)\s*\|\s*(?<fen>.+)$')
        if (-not $match.Success) {
            throw "Invalid FEN file line. Expected name | six-field FEN: $trimmed"
        }
        $records.Add((Convert-FenToEpdRecord $match.Groups['name'].Value $match.Groups['fen'].Value))
    }
    if ($records.Count -eq 0) {
        throw "FEN file contains no positions: $SourcePath"
    }
    Set-Content -LiteralPath $DestinationPath -Value @($records) -Encoding ASCII
}

function Convert-UciOpeningFileToEpd([string]$SourcePath, [string]$DestinationPath,
                                     [string]$ReplayExecutable) {
    $records = [System.Collections.Generic.List[string]]::new()
    foreach ($line in Get-Content -LiteralPath $SourcePath) {
        $trimmed = $line.Trim()
        if ($trimmed.Length -eq 0 -or $trimmed.StartsWith('#')) {
            continue
        }
        $match = [regex]::Match($trimmed, '^(?<name>[^|#\s]+)\s*\|\s*(?<moves>[a-h][1-8][a-h][1-8][nbrq]?(?:\s+[a-h][1-8][a-h][1-8][nbrq]?)*?)\s*$')
        if (-not $match.Success) {
            throw "Invalid opening file line. Expected name | UCI move UCI move: $trimmed"
        }

        $moves = @($match.Groups['moves'].Value -split '\s+')
        $replayArguments = @('startpos', 'moves') + $moves
        $replayOutput = @(& $ReplayExecutable @replayArguments 2>&1 |
            ForEach-Object { $_.ToString() })
        $replayExitCode = $LASTEXITCODE
        if ($replayExitCode -ne 0) {
            throw "Replay failed for opening '$($match.Groups['name'].Value)' with exit code ${replayExitCode}: $($replayOutput -join ' | ')"
        }
        if (-not (@($replayOutput | Where-Object { $_ -ceq 'legal 1' }).Count -eq 1)) {
            throw "Opening '$($match.Groups['name'].Value)' contains an illegal move."
        }
        $fenLine = @($replayOutput | Where-Object { $_ -like 'fen *' } | Select-Object -First 1)
        if ($fenLine.Count -ne 1) {
            throw "Replay did not return a FEN for opening '$($match.Groups['name'].Value)'."
        }
        $records.Add((Convert-FenToEpdRecord $match.Groups['name'].Value $fenLine.Substring(4)))
    }
    if ($records.Count -eq 0) {
        throw "Opening file contains no positions: $SourcePath"
    }
    Set-Content -LiteralPath $DestinationPath -Value @($records) -Encoding ASCII
}

$pgnPath = Join-Path $OutputDirectory 'games.pgn'
$transcriptPath = Join-Path $OutputDirectory 'cutechess-transcript.log'
$reportPath = Join-Path $OutputDirectory 'stability-report.json'
$manifestPath = Join-Path $OutputDirectory 'manifest.json'
$commandArtifactPath = Join-Path $OutputDirectory 'cutechess-command.json'
$cutechessVersionPath = Join-Path $OutputDirectory 'cutechess-version.txt'
$koiStderrPath = Join-Path $OutputDirectory 'koi-stderr.log'
$opponentStderrPath = Join-Path $OutputDirectory 'opponent-stderr.log'
$koiStdoutPath = Join-Path $OutputDirectory 'koi-stdout.log'
$opponentStdoutPath = Join-Path $OutputDirectory 'opponent-stdout.log'
$opponentDebugPath = Join-Path $OutputDirectory 'opponent-debug.jsonl'
$protocolEventsPath = Join-Path $OutputDirectory 'protocol-events.jsonl'
$perPlyPositionsPath = Join-Path $OutputDirectory 'per-ply-positions.jsonl'
$failurePath = Join-Path $OutputDirectory 'failure.json'
$exitStatusPath = Join-Path $OutputDirectory 'exit-status.json'
$koiDebugPath = Join-Path $OutputDirectory 'koi-debug.jsonl'
$convertedOpeningPath = $null
$openingForMatch = $null
$openingFormat = $null

# Create the complete artifact surface before launching Cutechess so a crash,
# disconnect, timeout, or engine start failure still leaves a diagnosable bundle.
$artifactPaths = @(
    $pgnPath, $transcriptPath, $reportPath, $manifestPath, $commandArtifactPath,
    $cutechessVersionPath, $koiStderrPath, $opponentStderrPath, $koiStdoutPath,
    $opponentStdoutPath, $protocolEventsPath, $perPlyPositionsPath, $failurePath,
    $exitStatusPath, $koiDebugPath, $opponentDebugPath
)
foreach ($artifactPath in $artifactPaths) {
    if (-not (Test-Path -LiteralPath $artifactPath -PathType Leaf)) {
        New-Item -ItemType File -Path $artifactPath -Force | Out-Null
    }
}

if (-not [string]::IsNullOrWhiteSpace($FenFile)) {
    $convertedOpeningPath = Join-Path $OutputDirectory 'fen-openings.epd'
    Convert-FenFileToEpd $fen $convertedOpeningPath
    $openingForMatch = $convertedOpeningPath
    $openingFormat = 'epd'
} elseif (-not [string]::IsNullOrWhiteSpace($OpeningFile)) {
    $extension = [System.IO.Path]::GetExtension($opening).ToLowerInvariant()
    if ($extension -eq '.txt') {
        $convertedOpeningPath = Join-Path $OutputDirectory 'uci-openings.epd'
        Convert-UciOpeningFileToEpd $opening $convertedOpeningPath $replay
        $openingForMatch = $convertedOpeningPath
        $openingFormat = 'epd'
    } else {
        $openingForMatch = $opening
        $openingFormat = if ($extension -eq '.epd') { 'epd' } else { 'pgn' }
    }
}

$koiArguments = @(
    '-engine', "cmd=$koi", 'name=Koi', 'proto=uci',
    "option.Hash=$Hash", "option.Threads=$Threads", "option.Speed=$Speed",
    "option.OwnBook=$($OwnBook.ToString().ToLowerInvariant())",
    "option.BookFile=$bookOption", "option.BookDepth=$BookDepth",
    'option.BookRandom=false', "stderr=$koiStderrPath"
)
if ($EnableDebug) {
    $initString = "setoption name DebugFile value $koiDebugPath\nsetoption name Debug value true"
    $koiArguments += @('debug', "initstr=$initString")
}

$opponentArguments = @(
    '-engine', "cmd=$opponent", 'name=Opponent', 'proto=uci',
    "stderr=$opponentStderrPath"
)
if ($EnableDebug -and $opponentIsKoi) {
    $opponentInitString = "setoption name DebugFile value $opponentDebugPath\nsetoption name Debug value true"
    $opponentArguments += @('debug', "initstr=$opponentInitString")
}
$matchArguments = @(
    '-each', "tc=$TimeControl",
    '-rounds', "$Games",
    '-concurrency', '1',
    '-maxmoves', "$MaxMoves",
    '-variant', 'standard',
    '-pgnout', $pgnPath,
    '-event', 'Koi stability compatibility'
)
if ($null -ne $openingForMatch) {
    $matchArguments += @('-openings', "file=$openingForMatch", "format=$openingFormat", 'order=sequential', 'policy=round')
}

$orderedEngineArguments = if ($KoiColor -eq 'black') {
    @($opponentArguments + $koiArguments)
} else {
    @($koiArguments + $opponentArguments)
}
$arguments = @($orderedEngineArguments + $matchArguments)
$commandLine = ConvertTo-ProcessCommandLine $cutechess $arguments
$stdoutPath = Join-Path $OutputDirectory 'cutechess-stdout.log'
$stderrPath = Join-Path $OutputDirectory 'cutechess-stderr.log'

$system = Get-SystemDescription
$runStartedUtc = (Get-Date).ToUniversalTime().ToString('o')
$executableProvenance = @(
    [ordered]@{ name = 'Koi'; path = $koi; sha256 = $koiHash },
    [ordered]@{ name = 'Opponent'; path = $opponent; sha256 = $opponentHash },
    [ordered]@{ name = 'Cutechess'; path = $cutechess; sha256 = $cutechessHash }
)
$inputProvenance = @()
if (-not [string]::IsNullOrWhiteSpace($OpeningFile)) {
    $inputProvenance += [ordered]@{
        kind = 'opening'
        path = $opening
        sha256 = Get-ExecutableHash $opening
    }
}
if (-not [string]::IsNullOrWhiteSpace($FenFile)) {
    $inputProvenance += [ordered]@{
        kind = 'fen'
        path = $fen
        sha256 = Get-ExecutableHash $fen
    }
}
$commandArtifact = [ordered]@{
    schema = 'koi-cutechess-command-v1'
    executable = $cutechess
    arguments = $arguments
    command_line = $commandLine
    global_debug = $false
    diagnostic_mode = if ($EnableDebug) { 'koi-file-only' } else { 'disabled' }
}
$commandArtifact | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $commandArtifactPath -Encoding UTF8
$initialManifest = [ordered]@{
    schema = 'koi-cutechess-stability-manifest-v1'
    run_started_utc = $runStartedUtc
    system = $system
    executables = $executableProvenance
    inputs = $inputProvenance
    command = $commandArtifact
    configuration = [ordered]@{
        hash_mb = $Hash
        threads = $Threads
        speed = $Speed
        own_book = $OwnBook
        book_file = $bookOption
        book_depth = $BookDepth
        koi_color = $KoiColor
        time_control = $TimeControl
        games = $Games
        max_moves = $MaxMoves
        opening_file = if ($null -eq $opening) { $null } else { $opening }
        fen_file = if ($null -eq $fen) { $null } else { $fen }
    }
    process = [ordered]@{ cutechess_pid = $null; exit_code = $null }
    artifacts = [ordered]@{
        manifest = $manifestPath
        command = $commandArtifactPath
        version = $cutechessVersionPath
        cutechess_stdout = $stdoutPath
        cutechess_stderr = $stderrPath
        koi_stdout = $koiStdoutPath
        koi_stderr = $koiStderrPath
        opponent_stdout = $opponentStdoutPath
        opponent_stderr = $opponentStderrPath
        opponent_debug = $opponentDebugPath
        protocol_events = $protocolEventsPath
        per_ply_positions = $perPlyPositionsPath
        debug = $koiDebugPath
        pgn = $pgnPath
        failure = $failurePath
        exit_status = $exitStatusPath
    }
}
$initialManifest | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $manifestPath -Encoding UTF8

# Cutechess 1.5.1 exposes the manager stream and per-engine stderr, but not a
# separate stdout file for each child unless its global -debug mode is used.
# That switch is intentionally forbidden for this harness. Keep explicit,
# machine-readable placeholders so the limitation is visible in every bundle;
# Koi's own JSONL file remains the authoritative per-ply trace.
$stdoutCaptureNote = [ordered]@{
    event = 'capture_unavailable'
    reason = 'cutechess-child-stdout-is-multiplexed-without-global-debug'
    global_debug_used = $false
}
$stdoutCaptureNote | ConvertTo-Json -Compress | Set-Content -LiteralPath $koiStdoutPath -Encoding UTF8
$stdoutCaptureNote | ConvertTo-Json -Compress | Set-Content -LiteralPath $opponentStdoutPath -Encoding UTF8

$run = Invoke-Cutechess $cutechess $arguments $transcriptPath $stdoutPath $stderrPath `
    $protocolEventsPath $TimeoutMilliseconds
$lines = @($run.lines)
$exitCode = $run.exit_code

$failurePatterns = @(
    '(?i)illegal move',
    '(?i)invalid move',
    '(?i)time forfeit',
    '(?i)loses on time',
    '(?i)timed out',
    '(?i)timeout',
    '(?i)crash',
    '(?i)disconnect',
    '(?i)no bestmove',
    '(?i)protocol error',
    '(?i)engine error',
    '(?i)duplicate bestmove',
    '(?i)unexpected output',
    '(?i)could not start'
)
$failureLines = @($lines | Where-Object {
    $line = $_
    @($failurePatterns | Where-Object { $line -match $_ }).Count -gt 0
})
$startedLines = @($lines | Where-Object { $_ -match '^Started game ' })
$finishedLines = @($lines | Where-Object { $_ -match '^Finished game ' })
$koiWhite = @($startedLines | Where-Object { $_ -match '\(Koi vs ' }).Count
$koiBlack = @($startedLines | Where-Object { $_ -match '\(.* vs Koi\)' }).Count
$colorBalanceDelta = [math]::Abs($koiWhite - $koiBlack)

$terminationClassification = if ($run.timed_out) {
    'timeout'
} elseif ($run.start_error) {
    'crash'
} elseif (@($lines | Where-Object { $_ -match '(?i)disconnect' }).Count -gt 0) {
    'disconnect'
} elseif (@($lines | Where-Object { $_ -match '(?i)crash|could not start|process exit' }).Count -gt 0) {
    'crash'
} elseif ($null -ne $exitCode -and $exitCode -ne 0) {
    'crash'
} elseif ($finishedLines.Count -eq 0) {
    'no-result'
} elseif ($finishedLines.Count -ne $Games -or $failureLines.Count -ne 0) {
    'incomplete'
} else {
    'completed'
}

$cutechessVersion = ''
if (-not $run.timed_out) {
    try {
        $cutechessVersion = @(& $cutechess -version 2>&1 | ForEach-Object { $_.ToString() }) -join ' '
    } catch {
        $cutechessVersion = ''
    }
}
$cutechessVersion | Set-Content -LiteralPath $cutechessVersionPath -Encoding UTF8
$koiDebugArtifact = if ($EnableDebug -and (Test-Path -LiteralPath $koiDebugPath -PathType Leaf)) {
    $koiDebugPath
} else {
    $null
}

# Preserve the controller's accepted-root and completion records as a second,
# stable per-ply artifact. Parsing is best effort so a malformed final line
# never destroys the partial incident bundle.
if (Test-Path -LiteralPath $koiDebugPath -PathType Leaf) {
    foreach ($debugLine in Get-Content -LiteralPath $koiDebugPath) {
        try {
            $debugRecord = $debugLine | ConvertFrom-Json
            if ($debugRecord.event -in @('position', 'go', 'completion_validation', 'move')) {
                Add-Content -LiteralPath $perPlyPositionsPath `
                    -Value ($debugRecord | ConvertTo-Json -Compress) -Encoding UTF8
            }
        } catch {
            # Keep the raw Koi debug file for forensic inspection.
        }
    }
}
$system = Get-SystemDescription
$executableProvenance = @(
    [ordered]@{ name = 'Koi'; path = $koi; sha256 = $koiHash },
    [ordered]@{ name = 'Opponent'; path = $opponent; sha256 = $opponentHash },
    [ordered]@{ name = 'Cutechess'; path = $cutechess; sha256 = $cutechessHash }
)
$inputProvenance = @()
if (-not [string]::IsNullOrWhiteSpace($OpeningFile)) {
    $inputProvenance += [ordered]@{
        kind = 'opening'
        path = $opening
        sha256 = Get-ExecutableHash $opening
    }
}
if (-not [string]::IsNullOrWhiteSpace($FenFile)) {
    $inputProvenance += [ordered]@{
        kind = 'fen'
        path = $fen
        sha256 = Get-ExecutableHash $fen
    }
}
$runErrorLines = if ($run.start_error) { @($run.start_error) } else { @() }
$report = [ordered]@{
    schema = 'koi-cutechess-stability-v1'
    system = $system
    run_started_utc = $run.started_utc
    configuration = [ordered]@{
        koi_path = $koi
        opponent_path = $opponent
        cutechess_path = $cutechess
        cutechess_version = $cutechessVersion
        games = $Games
        max_moves = $MaxMoves
        time_control = $TimeControl
        hash_mb = $Hash
        threads = $Threads
        speed = $Speed
        own_book = $OwnBook
        book_file = $bookOption
        book_depth = $BookDepth
        koi_color = $KoiColor
        book_resolved_path = $bookResolved
        book_sha256 = if (Test-Path -LiteralPath $bookResolved -PathType Leaf) {
            Get-ExecutableHash $bookResolved
        } else {
            $null
        }
        opening_file = if ([string]::IsNullOrWhiteSpace($OpeningFile)) { $null } else { $opening }
        fen_file = if ([string]::IsNullOrWhiteSpace($FenFile)) { $null } else { $fen }
        replay_path = $replay
        converted_opening_file = $convertedOpeningPath
        timeout_milliseconds = $TimeoutMilliseconds
        arguments = $arguments
        options = [ordered]@{
            games = $Games
            max_moves = $MaxMoves
            time_control = $TimeControl
            hash_mb = $Hash
            threads = $Threads
            speed = $Speed
            own_book = $OwnBook
            book_depth = $BookDepth
        }
        command = $commandLine
    }
    provenance = [ordered]@{
        executables = $executableProvenance
        inputs = $inputProvenance
    }
    engines = @($executableProvenance | Where-Object { $_.name -ne 'Cutechess' })
    results = [ordered]@{
        exit_code = $exitCode
        termination_classification = $terminationClassification
        started_games = $startedLines.Count
        finished_games = $finishedLines.Count
        failures = $failureLines.Count
        koi_white = $koiWhite
        koi_black = $koiBlack
        color_balance_delta = $colorBalanceDelta
        color_balanced = $colorBalanceDelta -le 1
        failure_lines = @($failureLines + $runErrorLines)
    }
    artifacts = [ordered]@{
        pgn = $pgnPath
        transcript = $transcriptPath
        stdout = $stdoutPath
        stderr = $stderrPath
        manifest = $manifestPath
        cutechess_command = $commandArtifactPath
        cutechess_version = $cutechessVersionPath
        koi_stdout = $koiStdoutPath
        koi_stderr = $koiStderrPath
        opponent_stdout = $opponentStdoutPath
        opponent_stderr = $opponentStderrPath
        opponent_debug = $opponentDebugPath
        protocol_events = $protocolEventsPath
        per_ply_positions = $perPlyPositionsPath
        koi_debug = $koiDebugPath
        failure = $failurePath
        exit_status = $exitStatusPath
        converted_opening = $convertedOpeningPath
    }
    termination_classification = $terminationClassification
    generated_utc = (Get-Date).ToUniversalTime().ToString('o')
}

$debugRecords = @()
if (Test-Path -LiteralPath $koiDebugPath -PathType Leaf) {
    foreach ($debugLine in Get-Content -LiteralPath $koiDebugPath) {
        try { $debugRecords += @($debugLine | ConvertFrom-Json) } catch { }
    }
}
$incidentCompletions = @($debugRecords | Where-Object {
    $_.event -eq 'completion_validation' -and $_.candidate -eq 'd4c3'
})
$invalidIncidentCompletions = @($incidentCompletions | Where-Object {
    $_.native_legal -ne $true -or $_.shadow_legal -ne $true -or
    $_.root_key_match -ne $true -or $_.identity_match -ne $true -or
    $_.disposition -ne 'emit'
})
$incidentRootRecords = [System.Collections.Generic.List[object]]::new()
foreach ($incident in $incidentCompletions) {
    $matchingRoots = @($debugRecords | Where-Object {
        $_.event -eq 'position' -and
        ($_.position_key -eq $incident.root_key -or $_.root_fen -eq $incident.root_fen)
    })
    if ($matchingRoots.Count -gt 0) {
        $incidentRootRecords.Add($matchingRoots[$matchingRoots.Count - 1])
    }
}
$lastPositionRecord = @($debugRecords | Where-Object { $_.event -eq 'position' } | Select-Object -Last 1)
$failure = [ordered]@{
    schema = 'koi-cutechess-stability-failure-v1'
    classification = $terminationClassification
    failure_lines = @($failureLines + $runErrorLines)
    d4c3_completion_records = $incidentCompletions
    d4c3_invalid_completion_records = $invalidIncidentCompletions
    d4c3_matching_position_records = @($incidentRootRecords)
    d4c3_last_command = if ($incidentCompletions.Count -eq 0) { $null } else { $incidentCompletions[$incidentCompletions.Count - 1].last_command }
    last_position_record = if ($lastPositionRecord.Count -eq 0) { $null } else { $lastPositionRecord[0] }
    root_reconstruction = [ordered]@{
        source = 'koi-debug.jsonl'
        cutechess_root_available = $false
        note = 'Cutechess 1.5.1 manager output does not expose a per-ply FEN; compare protocol-events.jsonl with Koi position records.'
    }
    artifacts = [ordered]@{
        protocol_events = $protocolEventsPath
        per_ply_positions = $perPlyPositionsPath
        koi_debug = $koiDebugPath
        opponent_debug = $opponentDebugPath
        transcript = $transcriptPath
    }
    generated_utc = (Get-Date).ToUniversalTime().ToString('o')
}
$failure | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath $failurePath -Encoding UTF8

$exitStatus = [ordered]@{
    schema = 'koi-cutechess-stability-exit-v1'
    process_id = $run.process_id
    exit_code = $exitCode
    timed_out = $run.timed_out
    start_error = $run.start_error
    termination_classification = $terminationClassification
    generated_utc = (Get-Date).ToUniversalTime().ToString('o')
}
$exitStatus | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $exitStatusPath -Encoding UTF8

$initialManifest.process.cutechess_pid = $run.process_id
$initialManifest.process.exit_code = $exitCode
$initialManifest.process.timed_out = $run.timed_out
$initialManifest.results = [ordered]@{
    termination_classification = $terminationClassification
    started_games = $startedLines.Count
    finished_games = $finishedLines.Count
    failures = $failureLines.Count
}
$initialManifest.generated_utc = (Get-Date).ToUniversalTime().ToString('o')
$initialManifest | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath $manifestPath -Encoding UTF8
$report | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $reportPath -Encoding UTF8

Write-Output "report $reportPath"
Write-Output "games started=$($startedLines.Count) finished=$($finishedLines.Count) koi_white=$koiWhite koi_black=$koiBlack"
Write-Output "failures=$($failureLines.Count) exit_code=$exitCode"

if ($exitCode -ne 0 -or $startedLines.Count -ne $Games -or $finishedLines.Count -ne $Games -or
    $failureLines.Count -ne 0 -or $colorBalanceDelta -gt 1) {
    exit 1
}
exit 0
