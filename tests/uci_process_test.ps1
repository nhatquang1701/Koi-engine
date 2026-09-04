param(
    [Parameter(Mandatory = $true)]
    [string]$EnginePath
)

$ErrorActionPreference = 'Stop'
$TimeoutMilliseconds = 5000
$MaximumThreads = [Math]::Max(1, [Math]::Min(64, [Environment]::ProcessorCount))

function Start-UciSession([string]$Executable = $EnginePath, [string]$WorkingDirectory = '') {
    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $Executable
    if (-not [string]::IsNullOrWhiteSpace($WorkingDirectory)) {
        $startInfo.WorkingDirectory = $WorkingDirectory
    }
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardInput = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true

    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    if (-not $process.Start()) {
        throw 'Unable to start koi-engine.'
    }

    return [pscustomobject]@{
        Process = $process
        StderrTask = $process.StandardError.ReadToEndAsync()
        Lines = [System.Collections.Generic.List[string]]::new()
    }
}

function Stop-TimedOutSession($Session, [string]$Message) {
    if (-not $Session.Process.HasExited) {
        $Session.Process.Kill()
        $Session.Process.WaitForExit()
    }
    throw $Message
}

function Send-UciCommand($Session, [string]$Command) {
    $Session.Process.StandardInput.WriteLine($Command)
    $Session.Process.StandardInput.Flush()
}

function Read-UciLine($Session, [string]$Description) {
    $readTask = $Session.Process.StandardOutput.ReadLineAsync()
    if (-not $readTask.Wait($TimeoutMilliseconds)) {
        Stop-TimedOutSession $Session "Timed out waiting for $Description."
    }

    $line = $readTask.GetAwaiter().GetResult()
    if ($null -eq $line) {
        throw "koi-engine closed stdout while waiting for $Description."
    }
    $Session.Lines.Add($line)
    return $line
}

function Complete-UciSession($Session, [bool]$SendQuit) {
    $stdoutTailTask = $Session.Process.StandardOutput.ReadToEndAsync()
    if ($SendQuit -and -not $Session.Process.HasExited) {
        Send-UciCommand $Session 'quit'
    }
    $Session.Process.StandardInput.Close()

    if (-not $Session.Process.WaitForExit($TimeoutMilliseconds)) {
        Stop-TimedOutSession $Session 'koi-engine did not exit within 5000 ms.'
    }

    $tail = $stdoutTailTask.GetAwaiter().GetResult()
    foreach ($line in @($tail -split "`r?`n" | Where-Object { $_.Length -ne 0 })) {
        $Session.Lines.Add($line)
    }
    $diagnostics = $Session.StderrTask.GetAwaiter().GetResult()

    if ($Session.Process.ExitCode -ne 0) {
        throw "koi-engine exited with $($Session.Process.ExitCode): $diagnostics"
    }
    if ($diagnostics.Length -ne 0) {
        throw "koi-engine wrote diagnostics for a valid transcript: $diagnostics"
    }

    return @($Session.Lines)
}

function Invoke-UciTranscript([string]$Transcript) {
    $session = Start-UciSession
    $stdoutTask = $session.Process.StandardOutput.ReadToEndAsync()
    $session.Process.StandardInput.Write($Transcript)
    $session.Process.StandardInput.Close()

    if (-not $session.Process.WaitForExit($TimeoutMilliseconds)) {
        Stop-TimedOutSession $session 'koi-engine transcript did not exit within 5000 ms.'
    }

    $output = $stdoutTask.GetAwaiter().GetResult()
    $diagnostics = $session.StderrTask.GetAwaiter().GetResult()
    if ($session.Process.ExitCode -ne 0) {
        throw "koi-engine exited with $($session.Process.ExitCode): $diagnostics"
    }
    if ($diagnostics.Length -ne 0) {
        throw "koi-engine wrote diagnostics for a valid transcript: $diagnostics"
    }

    return @($output -split "`r?`n" | Where-Object { $_.Length -ne 0 })
}

function Test-SearchInfo([string]$Line) {
    return $Line -match '^info depth [1-9][0-9]* seldepth [0-9]+ multipv [1-9][0-6]? score (cp|mate) -?[0-9]+ nodes [0-9]+ nps [0-9]+ time [0-9]+ pv( [a-h][1-8][a-h][1-8][nbrq]?)*$'
}

function Write-StartPositionBook([string]$Path) {
    [byte[]]$bytes = @(
        0x46, 0x3b, 0x96, 0x18, 0x16, 0x91, 0xfc, 0x9c,
        0x07, 0x0c, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00
    )
    [System.IO.File]::WriteAllBytes($Path, $bytes)
}

$session = Start-UciSession
Send-UciCommand $session 'uci'
$expectedHandshake = @(
    'id name Koi Engine',
    'id author Koi Engine contributors',
    'option name RandomSeed type spin default 0 min 0 max 2147483647',
    'option name Hash type spin default 16 min 1 max 4096',
    "option name Threads type spin default 1 min 1 max $MaximumThreads",
    'option name Speed type spin default 100 min 1 max 100',
    'option name UCI_AnalyseMode type check default false',
    'option name MultiPV type spin default 1 min 1 max 16',
    'option name Ponder type check default false',
    'option name OwnBook type check default true',
    'option name BookFile type string default book.bin',
    'option name BookDepth type spin default 16 min 0 max 40',
    'option name Clear Hash type button',
    'uciok'
)
foreach ($expected in $expectedHandshake) {
    $actual = Read-UciLine $session $expected
    if ($actual -cne $expected) {
        throw "Unexpected handshake line. Expected '$expected', received '$actual'."
    }
}

Send-UciCommand $session 'isready'
if ((Read-UciLine $session 'initial readyok') -cne 'readyok') {
    throw 'Expected readyok after the UCI handshake.'
}

$bookFileName = "uci-process-book-$([guid]::NewGuid().ToString('N')).bin"
$bookPath = Join-Path (Split-Path -Parent $EnginePath) $bookFileName
$bookLaunchDirectory = Join-Path ([System.IO.Path]::GetTempPath()) "koi-uci-process-$([guid]::NewGuid().ToString('N'))"
$originalPath = $env:PATH
$bookSession = $null
try {
    Write-StartPositionBook $bookPath
    New-Item -ItemType Directory -Path $bookLaunchDirectory | Out-Null
    $env:PATH = "$(Split-Path -Parent $EnginePath);$originalPath"
    $bookSession = Start-UciSession 'koi-engine.exe' $bookLaunchDirectory
    Send-UciCommand $bookSession 'uci'
    foreach ($expected in $expectedHandshake) {
        if ((Read-UciLine $bookSession $expected) -cne $expected) {
            throw "Unexpected book-session handshake line. Expected '$expected'."
        }
    }
    Send-UciCommand $bookSession 'setoption name RandomSeed value 29'
    Send-UciCommand $bookSession "setoption name BookFile value $bookFileName"
    Send-UciCommand $bookSession 'setoption name BookDepth value 16'
    Send-UciCommand $bookSession 'position startpos'
    Send-UciCommand $bookSession 'go depth 1'
    if ((Read-UciLine $bookSession 'executable-relative book marker') -cne 'info string book move e2e4 depth 0') {
        throw 'A book beside koi-engine.exe must emit its standard marker before bestmove.'
    }
    if ((Read-UciLine $bookSession 'executable-relative book bestmove') -cne 'bestmove e2e4') {
        throw 'A book hit must emit exactly the selected legal bestmove.'
    }
    Send-UciCommand $bookSession 'isready'
    if ((Read-UciLine $bookSession 'readyok after book bestmove') -cne 'readyok') {
        throw 'A book hit must not emit a duplicate bestmove before readyok.'
    }
    Send-UciCommand $bookSession 'setoption name MultiPV value 3'
    Send-UciCommand $bookSession 'position startpos'
    Send-UciCommand $bookSession 'go depth 1'
    $multiPvRanks = [System.Collections.Generic.HashSet[int]]::new()
    $multiPvBestmove = $null
    while ($null -eq $multiPvBestmove) {
        $line = Read-UciLine $bookSession 'normal MultiPV search with matching book'
        if ($line -like 'info string book move *') {
            throw "MultiPV greater than one must search instead of using the book: $line"
        }
        if ($line -like 'bestmove *') {
            if ($line -notmatch '^bestmove [a-h][1-8][a-h][1-8][nbrq]?$') {
                throw "MultiPV search emitted an invalid bestmove: $line"
            }
            $multiPvBestmove = $line
        } elseif (Test-SearchInfo $line) {
            if ($line -match ' multipv ([1-9][0-9]*) score ') {
                $null = $multiPvRanks.Add([int]$Matches[1])
            }
        } else {
            throw "Invalid normal MultiPV output: $line"
        }
    }
    if (-not ($multiPvRanks.Contains(1) -and $multiPvRanks.Contains(2) -and $multiPvRanks.Contains(3))) {
        throw "Normal MultiPV search did not emit all three ranked variations: $($multiPvRanks -join ', ')"
    }
    Send-UciCommand $bookSession 'isready'
    if ((Read-UciLine $bookSession 'readyok after MultiPV search') -cne 'readyok') {
        throw 'MultiPV search emitted a duplicate bestmove before readyok.'
    }
    $null = Complete-UciSession $bookSession $true
} finally {
    if ($null -ne $bookSession -and -not $bookSession.Process.HasExited) {
        $bookSession.Process.StandardInput.Close()
        if (-not $bookSession.Process.WaitForExit($TimeoutMilliseconds)) {
            $bookSession.Process.Kill()
            $bookSession.Process.WaitForExit()
        }
    }
    $env:PATH = $originalPath
    if (Test-Path -LiteralPath $bookPath) {
        Remove-Item -LiteralPath $bookPath -Force
    }
    if (Test-Path -LiteralPath $bookLaunchDirectory) {
        Remove-Item -LiteralPath $bookLaunchDirectory -Recurse -Force
    }
}

Send-UciCommand $session 'setoption name UCI_AnalyseMode value true'
Send-UciCommand $session 'setoption name MultiPV value 3'
Send-UciCommand $session 'setoption name Ponder value true'
Send-UciCommand $session 'position startpos'
Send-UciCommand $session 'go ponder depth 2'
$ponderInfoLines = [System.Collections.Generic.List[string]]::new()
$ponderDepthTwoSeen = $false
while (-not $ponderDepthTwoSeen) {
    $line = Read-UciLine $session 'depth-two ponder PV before ponderhit'
    if (Test-SearchInfo $line) {
        $ponderInfoLines.Add($line)
        $ponderDepthTwoSeen = $line -match '^info depth 2 .* multipv 1 .* pv [a-h][1-8][a-h][1-8][nbrq]? [a-h][1-8][a-h][1-8][nbrq]?$'
    } else {
        throw "Invalid output before ponderhit: $line"
    }
}
Send-UciCommand $session 'ponderhit'
$ponderBestmove = $null
while ($null -eq $ponderBestmove) {
    $line = Read-UciLine $session 'bestmove after ponderhit'
    if ($line -like 'bestmove *') {
        if ($line -notmatch '^bestmove [a-h][1-8][a-h][1-8][nbrq]?( ponder [a-h][1-8][a-h][1-8][nbrq]?)?$') {
            throw "ponderhit emitted an invalid bestmove: $line"
        }
        $ponderBestmove = $line
    } elseif (Test-SearchInfo $line) {
        $ponderInfoLines.Add($line)
    } else {
        throw "Invalid output after ponderhit: $line"
    }
}
if (-not @($ponderInfoLines | Where-Object { $_ -match ' multipv 2 score ' })) {
    throw "Ponder MultiPV search did not emit rank two: $($ponderInfoLines -join ' | ')"
}
Send-UciCommand $session 'isready'
while ($true) {
    $line = Read-UciLine $session 'readyok after ponderhit'
    if ($line -ceq 'readyok') {
        break
    }
    if ($line -like 'bestmove *') {
        throw "ponderhit emitted a duplicate bestmove: $line"
    }
    if (-not (Test-SearchInfo $line)) {
        throw "Invalid output after ponderhit: $line"
    }
}

Send-UciCommand $session 'position startpos'
Send-UciCommand $session 'go ponder searchmoves e2e4'
Send-UciCommand $session 'stop'
$searchmovesBestmove = $null
while ($null -eq $searchmovesBestmove) {
    $line = Read-UciLine $session 'bestmove after ponder searchmoves stop'
    if ($line -like 'bestmove *') {
        $searchmovesBestmove = $line
    } elseif (-not (Test-SearchInfo $line)) {
        throw "Invalid output while stopping ponder searchmoves: $line"
    }
}
if ($searchmovesBestmove -cne 'bestmove e2e4') {
    throw "Ponder searchmoves did not preserve the root filter: $searchmovesBestmove"
}

Send-UciCommand $session 'position startpos'
Send-UciCommand $session "setoption name Threads value $MaximumThreads"
Send-UciCommand $session 'setoption name Speed value 50'
Send-UciCommand $session 'go infinite'
Send-UciCommand $session 'isready'
while ($true) {
    $line = Read-UciLine $session 'readyok during infinite search'
    if ($line -ceq 'readyok') {
        break
    }
    if ($line -like 'bestmove *') {
        throw "Infinite search completed before isready: $line"
    }
    if (-not (Test-SearchInfo $line)) {
        throw "Invalid output during infinite search: $line"
    }
}

Send-UciCommand $session 'stop'
$bestmove = $null
while ($null -eq $bestmove) {
    $line = Read-UciLine $session 'bestmove after stop'
    if ($line -like 'bestmove *') {
        $bestmove = $line
    } elseif (-not (Test-SearchInfo $line)) {
        throw "Invalid output while stopping search: $line"
    }
}

$initialMoves = @(
    'a2a3', 'a2a4', 'b2b3', 'b2b4', 'c2c3', 'c2c4', 'd2d3', 'd2d4',
    'e2e3', 'e2e4', 'f2f3', 'f2f4', 'g2g3', 'g2g4', 'h2h3', 'h2h4',
    'b1a3', 'b1c3', 'g1f3', 'g1h3'
)
if ($bestmove -notmatch '^bestmove ([a-h][1-8][a-h][1-8][nbrq]?)( ponder [a-h][1-8][a-h][1-8][nbrq]?)?$' -or
    $initialMoves -notcontains $Matches[1]) {
    throw "Expected a legal initial bestmove, received: $bestmove"
}

Send-UciCommand $session 'stop'
Send-UciCommand $session 'isready'
while ($true) {
    $line = Read-UciLine $session 'readyok after duplicate stop'
    if ($line -ceq 'readyok') {
        break
    }
    if ($line -like 'bestmove *') {
        throw "Duplicate stop emitted another bestmove: $line"
    }
    if (-not (Test-SearchInfo $line)) {
        throw "Invalid output after duplicate stop: $line"
    }
}
$null = Complete-UciSession $session $true

$immediatePonder = Start-UciSession
Send-UciCommand $immediatePonder 'position startpos'
$immediatePonder.Process.StandardInput.Write("go ponder depth 2`nponderhit`n")
$immediatePonder.Process.StandardInput.Flush()
$immediatePonderBestmoves = [System.Collections.Generic.List[string]]::new()
while ($immediatePonderBestmoves.Count -eq 0) {
    $line = Read-UciLine $immediatePonder 'bestmove after immediate ponderhit'
    if ($line -like 'bestmove *') {
        if ($line -notmatch '^bestmove ([a-h][1-8][a-h][1-8][nbrq]?)$' -or
            $initialMoves -notcontains $Matches[1]) {
            throw "Immediate ponderhit emitted an invalid bestmove: $line"
        }
        $immediatePonderBestmoves.Add($line)
    } elseif (-not (Test-SearchInfo $line)) {
        throw "Invalid output after immediate ponderhit: $line"
    }
}
Send-UciCommand $immediatePonder 'isready'
while ($true) {
    $line = Read-UciLine $immediatePonder 'readyok after immediate ponderhit'
    if ($line -ceq 'readyok') {
        break
    }
    if ($line -like 'bestmove *') {
        $immediatePonderBestmoves.Add($line)
    } elseif (-not (Test-SearchInfo $line)) {
        throw "Invalid output after immediate ponderhit: $line"
    }
}
$immediatePonderLines = @(Complete-UciSession $immediatePonder $true)
foreach ($line in $immediatePonderLines) {
    if ($line -like 'bestmove *') {
        if ($line -notmatch '^bestmove ([a-h][1-8][a-h][1-8][nbrq]?)$' -or
            $initialMoves -notcontains $Matches[1]) {
            throw "Immediate ponderhit emitted an invalid bestmove: $line"
        }
    } elseif ($line -cne 'readyok' -and -not (Test-SearchInfo $line)) {
        throw "Invalid shutdown output after immediate ponderhit: $line"
    }
}
$allImmediatePonderBestmoves = @($immediatePonderLines | Where-Object { $_ -like 'bestmove *' })
if ($allImmediatePonderBestmoves.Count -ne 1) {
    throw "Immediate ponderhit emitted duplicate or missing bestmoves: $($allImmediatePonderBestmoves -join ' | ')"
}

$replacement = Start-UciSession
Send-UciCommand $replacement 'position startpos'
Send-UciCommand $replacement 'go infinite'
Send-UciCommand $replacement 'position fen 7k/6Q1/5K2/8/8/8/8/8 b - - 0 1'
Send-UciCommand $replacement 'go infinite'
Send-UciCommand $replacement 'stop'
$replacementBestmoves = [System.Collections.Generic.List[string]]::new()
while ($replacementBestmoves.Count -eq 0) {
    $line = Read-UciLine $replacement 'replacement bestmove'
    if ($line -like 'bestmove *') {
        $replacementBestmoves.Add($line)
    } elseif (-not (Test-SearchInfo $line)) {
        throw "Invalid output during position replacement: $line"
    }
}
Send-UciCommand $replacement 'isready'
while ($true) {
    $line = Read-UciLine $replacement 'replacement readyok fence'
    if ($line -ceq 'readyok') {
        break
    }
    if ($line -like 'bestmove *') {
        $replacementBestmoves.Add($line)
    } elseif (-not (Test-SearchInfo $line)) {
        throw "Invalid output after position replacement: $line"
    }
}
$null = Complete-UciSession $replacement $true
if ($replacementBestmoves.Count -ne 1 -or $replacementBestmoves[0] -cne 'bestmove 0000') {
    throw "Position replacement leaked a stale result: $($replacementBestmoves -join ' | ')"
}

$limitLines = @(Invoke-UciTranscript @'
position startpos
go depth 1
stop
go nodes 0
stop
go movetime 0
stop
go wtime 0 btime 0 winc 0 binc 0 movestogo 1
stop
go infinite
stop
  go depth bad nodes -1 movetime -1 wtime bad btime -1 winc nope binc -4 movestogo 0
stop
go
stop
setoption name Threads value 1
setoption name Speed value 100
setoption name Hash value 1
setoption name Clear Hash
quit
'@
)
$limitBestmoves = @($limitLines | Where-Object { $_ -like 'bestmove *' })
if ($limitBestmoves.Count -ne 7) {
    throw "Expected one bestmove for every go limit variant: $($limitLines -join ' | ')"
}
foreach ($line in $limitLines) {
    if ($line -like 'bestmove *') {
        if ($line -notmatch '^bestmove [a-h][1-8][a-h][1-8][nbrq]?$') {
            throw "Invalid coordinate bestmove: $line"
        }
    } elseif (-not (Test-SearchInfo $line)) {
        throw "Invalid limit transcript output: $line"
    }
}

$asymmetricClock = Start-UciSession
Send-UciCommand $asymmetricClock 'position fen 4k3/8/8/8/8/8/4P3/4K3 b - - 0 1'
Send-UciCommand $asymmetricClock 'go wtime 1000'
$asymmetricBestmove = $null
while ($null -eq $asymmetricBestmove) {
    $line = Read-UciLine $asymmetricClock 'bestmove after asymmetric clock command'
    if ($line -like 'bestmove *') {
        $asymmetricBestmove = $line
    } elseif (-not (Test-SearchInfo $line)) {
        throw "Invalid output for asymmetric clock command: $line"
    }
}
if ($asymmetricBestmove -notmatch '^bestmove [a-h][1-8][a-h][1-8][nbrq]?$') {
    throw "Asymmetric clock command did not return a coordinate bestmove: $asymmetricBestmove"
}
Send-UciCommand $asymmetricClock 'isready'
if ((Read-UciLine $asymmetricClock 'readyok after asymmetric clock command') -cne 'readyok') {
    throw 'Asymmetric clock command did not complete before readyok.'
}
$null = Complete-UciSession $asymmetricClock $true

$infiniteNodes = Start-UciSession
Send-UciCommand $infiniteNodes 'position startpos'
Send-UciCommand $infiniteNodes 'go infinite nodes 1'
$infiniteNodesBestmove = $null
while ($null -eq $infiniteNodesBestmove) {
    $line = Read-UciLine $infiniteNodes 'bestmove after infinite node-limit search'
    if ($line -like 'bestmove *') {
        $infiniteNodesBestmove = $line
    } elseif (-not (Test-SearchInfo $line)) {
        throw "Invalid output during infinite node-limit search: $line"
    }
}
if ($infiniteNodesBestmove -notmatch '^bestmove [a-h][1-8][a-h][1-8][nbrq]?$') {
    throw "Infinite node-limit search emitted an invalid bestmove: $infiniteNodesBestmove"
}
Send-UciCommand $infiniteNodes 'isready'
if ((Read-UciLine $infiniteNodes 'readyok after infinite node limit') -cne 'readyok') {
    throw 'Infinite node-limit search did not complete before readyok.'
}
$null = Complete-UciSession $infiniteNodes $true

$ponderNodes = Start-UciSession
Send-UciCommand $ponderNodes 'position startpos'
Send-UciCommand $ponderNodes 'go ponder nodes 1'
$ponderNodesBestmove = $null
while ($null -eq $ponderNodesBestmove) {
    $line = Read-UciLine $ponderNodes 'bestmove after ponder node-limit search'
    if ($line -like 'bestmove *') {
        $ponderNodesBestmove = $line
    } elseif (-not (Test-SearchInfo $line)) {
        throw "Invalid output during ponder node-limit search: $line"
    }
}
if ($ponderNodesBestmove -notmatch '^bestmove [a-h][1-8][a-h][1-8][nbrq]?( ponder [a-h][1-8][a-h][1-8][nbrq]?)?$') {
    throw "Ponder node-limit search emitted an invalid bestmove: $ponderNodesBestmove"
}
$null = Complete-UciSession $ponderNodes $true

$quitSession = Start-UciSession
Send-UciCommand $quitSession 'position startpos'
Send-UciCommand $quitSession 'go infinite'
$quitLines = @(Complete-UciSession $quitSession $true)
if (@($quitLines | Where-Object { $_ -like 'bestmove *' }).Count -ne 0) {
    throw "quit emitted a late bestmove: $($quitLines -join ' | ')"
}

$eofSession = Start-UciSession
Send-UciCommand $eofSession 'position startpos'
Send-UciCommand $eofSession 'go infinite'
$eofLines = @(Complete-UciSession $eofSession $false)
if (@($eofLines | Where-Object { $_ -like 'bestmove *' }).Count -ne 0) {
    throw "EOF emitted a late bestmove: $($eofLines -join ' | ')"
}

$gameSession = Start-UciSession
Send-UciCommand $gameSession 'uci'
foreach ($expected in $expectedHandshake) {
    if ((Read-UciLine $gameSession $expected) -cne $expected) {
        throw "Unexpected multi-ply handshake line. Expected '$expected'."
    }
}
Send-UciCommand $gameSession "setoption name Threads value $MaximumThreads"
Send-UciCommand $gameSession 'setoption name Speed value 50'
$gameMoves = [System.Collections.Generic.List[string]]::new()
for ($ply = 0; $ply -lt 6; $ply++) {
    $positionCommand = 'position startpos'
    if ($gameMoves.Count -gt 0) {
        $positionCommand += ' moves ' + ($gameMoves -join ' ')
    }
    Send-UciCommand $gameSession $positionCommand
    Send-UciCommand $gameSession 'go depth 1'
    $gameBestmove = $null
    while ($null -eq $gameBestmove) {
        $line = Read-UciLine $gameSession "multi-ply bestmove $ply"
        if ($line -like 'bestmove *') {
            if ($line -notmatch '^bestmove ([a-h][1-8][a-h][1-8][nbrq]?)$') {
                throw "Multi-ply game emitted an invalid bestmove: $line"
            }
            $gameBestmove = $Matches[1]
            $gameMoves.Add($gameBestmove)
        } elseif (-not (Test-SearchInfo $line)) {
            throw "Invalid multi-ply game output: $line"
        }
    }
    Send-UciCommand $gameSession 'isready'
    if ((Read-UciLine $gameSession "multi-ply readyok $ply") -cne 'readyok') {
        throw "Multi-ply game did not fence the completed search at ply $ply."
    }
}
$null = Complete-UciSession $gameSession $true
