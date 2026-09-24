param(
    [Parameter(Mandatory = $true)]
    [string]$EnginePath
)

$ErrorActionPreference = 'Stop'
$TimeoutMilliseconds = if ($env:KOI_UCI_TIMEOUT_MS) {
    [int]$env:KOI_UCI_TIMEOUT_MS
} else {
    15000
}
$MaximumThreads = [Math]::Max(1, [Math]::Min(64, [Environment]::ProcessorCount))

Import-Module ([System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../support/UciSession.psm1'))) -Force

# Every engine process the shared session module starts is tracked there so a
# failed assertion can never orphan a running koi-engine.exe. The trap runs on
# any terminating error, kills the survivors, and then lets the error propagate.
trap {
    Stop-AllUciSessions
    break
}

function Invoke-UciTranscript([string]$Transcript) {
    $session = Start-UciSession -Executable $EnginePath
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

function Write-StartPositionBook([string]$Path) {
    [byte[]]$bytes = @(
        0x46, 0x3b, 0x96, 0x18, 0x16, 0x91, 0xfc, 0x9c,
        0x03, 0x1c, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00
    )
    [System.IO.File]::WriteAllBytes($Path, $bytes)
}

$session = Start-UciSession -Executable $EnginePath
Send-UciCommand $session 'uci'
# The handshake contract is shared with the C++ unit test through
# tests/data/uci/handshake.txt so both layers assert the same bytes.
$handshakeFixture = [System.IO.Path]::GetFullPath(
    (Join-Path $PSScriptRoot '../../data/uci/handshake.txt'))
if (-not (Test-Path -LiteralPath $handshakeFixture)) {
    throw "Missing handshake fixture: $handshakeFixture"
}
$expectedHandshake = @(
    Get-Content -LiteralPath $handshakeFixture | ForEach-Object {
        $_.Replace('{max_threads}', [string]$MaximumThreads).Replace('{empty}', '')
    }
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
$engineFileName = if ($env:OS -eq 'Windows_NT') { 'koi-engine.exe' } else { 'koi-engine' }
$bookPath = Join-Path (Split-Path -Parent $EnginePath) $bookFileName
$bookLaunchDirectory = Join-Path ([System.IO.Path]::GetTempPath()) "koi-uci-process-$([guid]::NewGuid().ToString('N'))"
$originalPath = $env:PATH
$bookSession = $null
try {
    Write-StartPositionBook $bookPath
    New-Item -ItemType Directory -Path $bookLaunchDirectory | Out-Null
    $env:PATH = "$(Split-Path -Parent $EnginePath)$([System.IO.Path]::PathSeparator)$originalPath"
    $bookSession = Start-UciSession -Executable $engineFileName -WorkingDirectory $bookLaunchDirectory
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
        throw 'A book beside the engine executable must emit its standard marker before bestmove.'
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
            if ($line -notmatch '^bestmove [a-h][1-8][a-h][1-8][nbrq]?( ponder [a-h][1-8][a-h][1-8][nbrq]?)?$') {
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

$immediatePonder = Start-UciSession -Executable $EnginePath
Send-UciCommand $immediatePonder 'position startpos'
$immediatePonder.Process.StandardInput.Write("go ponder depth 2`nponderhit`n")
$immediatePonder.Process.StandardInput.Flush()
$immediatePonderLines = [System.Collections.Generic.List[string]]::new()
Send-UciCommand $immediatePonder 'stop'
Send-UciCommand $immediatePonder 'isready'
while ($true) {
    $line = Read-UciLine $immediatePonder 'readyok after immediate ponderhit'
    if ($line -ceq 'readyok') {
        break
    }
    if ($line -notlike 'bestmove *' -and -not (Test-SearchInfo $line)) {
        throw "Invalid output after immediate ponderhit: $line"
    }
    $immediatePonderLines.Add($line)
}
$null = Complete-UciSession $immediatePonder $true
foreach ($line in @($immediatePonderLines)) {
    if ($line -cne 'readyok' -and $line -notlike 'bestmove *' -and -not (Test-SearchInfo $line)) {
        throw "Invalid shutdown output after immediate ponderhit: $line"
    }
}
$immediatePonderBestmoves = @($immediatePonderLines | Where-Object { $_ -like 'bestmove *' })
if ($immediatePonderBestmoves.Count -ne 1) {
    throw ("a ponderhit always obliges the engine to answer, but the immediate ponderhit emitted " +
        "$($immediatePonderBestmoves.Count) bestmoves")
}

# A depth-limited ponder must keep deepening until stop/ponderhit instead of
# re-searching the same final depth forever, and it must not emit a bestmove
# while it is still pondering.
$deepeningPonder = Start-UciSession -Executable $EnginePath
Send-UciCommand $deepeningPonder 'position startpos'
Send-UciCommand $deepeningPonder 'go ponder depth 2'
$maxPonderDepth = 0
$ponderDeadline = [DateTime]::UtcNow.AddSeconds(10)
while ([DateTime]::UtcNow -lt $ponderDeadline -and $maxPonderDepth -lt 3) {
    $line = Read-UciLine $deepeningPonder 'ponder deepening'
    if ($line -like 'bestmove *') {
        throw "a pondering depth-limited search emitted a bestmove before stop or ponderhit: $line"
    } elseif ($line -match '^info depth ([0-9]+)') {
        $maxPonderDepth = [Math]::Max($maxPonderDepth, [int]$Matches[1])
    } elseif (-not (Test-SearchInfo $line)) {
        throw "Invalid output while pondering: $line"
    }
}
if ($maxPonderDepth -lt 3) {
    throw ("a ponder depth-2 search must keep deepening past depth 2 instead of repeating it; " +
        "the highest depth observed was $maxPonderDepth")
}
Send-UciCommand $deepeningPonder 'ponderhit'
$convertedPonderBestmoves = [System.Collections.Generic.List[string]]::new()
$ponderDeadline = [DateTime]::UtcNow.AddSeconds(10)
while ($convertedPonderBestmoves.Count -eq 0 -and [DateTime]::UtcNow -lt $ponderDeadline) {
    $line = Read-UciLine $deepeningPonder 'bestmove after the converted ponder'
    if ($line -like 'bestmove *') {
        $convertedPonderBestmoves.Add($line)
    } elseif (-not (Test-SearchInfo $line)) {
        throw "Invalid output after the ponderhit: $line"
    }
}
if ($convertedPonderBestmoves.Count -ne 1) {
    throw "a converted ponder search must emit exactly one bestmove"
}
Send-UciCommand $deepeningPonder 'stop'
Send-UciCommand $deepeningPonder 'isready'
while ($true) {
    $line = Read-UciLine $deepeningPonder 'readyok after the converted ponder'
    if ($line -ceq 'readyok') {
        break
    }
    if ($line -like 'bestmove *') {
        $convertedPonderBestmoves.Add($line)
    } elseif (-not (Test-SearchInfo $line)) {
        throw "Invalid output after the converted ponder search: $line"
    }
}
$null = Complete-UciSession $deepeningPonder $true
if ($convertedPonderBestmoves.Count -ne 1) {
    throw "the converted ponder search emitted $($convertedPonderBestmoves.Count) bestmoves"
}

$replacement = Start-UciSession -Executable $EnginePath
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
setoption name OwnBook value false
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
        if ($line -notmatch '^bestmove [a-h][1-8][a-h][1-8][nbrq]?( ponder [a-h][1-8][a-h][1-8][nbrq]?)?$') {
            throw "Invalid coordinate bestmove: $line"
        }
    } elseif (-not (Test-SearchInfo $line)) {
        throw "Invalid limit transcript output: $line"
    }
}

$asymmetricClock = Start-UciSession -Executable $EnginePath
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
if ($asymmetricBestmove -notmatch '^bestmove [a-h][1-8][a-h][1-8][nbrq]?( ponder [a-h][1-8][a-h][1-8][nbrq]?)?$') {
    throw "Asymmetric clock command did not return a coordinate bestmove: $asymmetricBestmove"
}
Send-UciCommand $asymmetricClock 'isready'
if ((Read-UciLine $asymmetricClock 'readyok after asymmetric clock command') -cne 'readyok') {
    throw 'Asymmetric clock command did not complete before readyok.'
}
$null = Complete-UciSession $asymmetricClock $true

$infiniteNodes = Start-UciSession -Executable $EnginePath
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
if ($infiniteNodesBestmove -notmatch '^bestmove [a-h][1-8][a-h][1-8][nbrq]?( ponder [a-h][1-8][a-h][1-8][nbrq]?)?$') {
    throw "Infinite node-limit search emitted an invalid bestmove: $infiniteNodesBestmove"
}
Send-UciCommand $infiniteNodes 'isready'
if ((Read-UciLine $infiniteNodes 'readyok after infinite node limit') -cne 'readyok') {
    throw 'Infinite node-limit search did not complete before readyok.'
}
$null = Complete-UciSession $infiniteNodes $true

$ponderNodes = Start-UciSession -Executable $EnginePath
Send-UciCommand $ponderNodes 'position startpos'
Send-UciCommand $ponderNodes 'go ponder nodes 1'
# UCI forbids a bestmove while pondering. The node budget is already spent, so
# the worker must park and answer isready normally without publishing.
Send-UciCommand $ponderNodes 'isready'
$ponderNodesBestmove = $null
while ($null -eq $ponderNodesBestmove) {
    $line = Read-UciLine $ponderNodes 'readiness while pondering at the node limit'
    if ($line -like 'bestmove *') {
        $ponderNodesBestmove = $line
        break
    }
    if ($line -ceq 'readyok') {
        break
    }
    if (-not (Test-SearchInfo $line)) {
        throw "Invalid output during ponder node-limit search: $line"
    }
}
if ($null -ne $ponderNodesBestmove) {
    throw "Ponder node-limit search emitted a bestmove before stop: $ponderNodesBestmove"
}
Send-UciCommand $ponderNodes 'stop'
while ($null -eq $ponderNodesBestmove) {
    $line = Read-UciLine $ponderNodes 'bestmove after stopping a ponder node-limit search'
    if ($line -like 'bestmove *') {
        $ponderNodesBestmove = $line
        break
    }
    if (-not (Test-SearchInfo $line)) {
        throw "Invalid output while stopping a ponder node-limit search: $line"
    }
}
if ($ponderNodesBestmove -notmatch '^bestmove [a-h][1-8][a-h][1-8][nbrq]?( ponder [a-h][1-8][a-h][1-8][nbrq]?)?$') {
    throw "Ponder node-limit search emitted an invalid bestmove: $ponderNodesBestmove"
}
$null = Complete-UciSession $ponderNodes $true

$quitSession = Start-UciSession -Executable $EnginePath
Send-UciCommand $quitSession 'position startpos'
Send-UciCommand $quitSession 'go infinite'
$quitLines = @(Complete-UciSession $quitSession $true)
if (@($quitLines | Where-Object { $_ -like 'bestmove *' }).Count -ne 0) {
    throw "quit emitted a late bestmove: $($quitLines -join ' | ')"
}

$eofSession = Start-UciSession -Executable $EnginePath
Send-UciCommand $eofSession 'position startpos'
Send-UciCommand $eofSession 'go infinite'
$eofLines = @(Complete-UciSession $eofSession $false)
if (@($eofLines | Where-Object { $_ -like 'bestmove *' }).Count -ne 0) {
    throw "EOF emitted a late bestmove: $($eofLines -join ' | ')"
}

$gameSession = Start-UciSession -Executable $EnginePath
Send-UciCommand $gameSession 'uci'
foreach ($expected in $expectedHandshake) {
    if ((Read-UciLine $gameSession $expected) -cne $expected) {
        throw "Unexpected multi-ply handshake line. Expected '$expected'."
    }
}
Send-UciCommand $gameSession "setoption name Threads value $MaximumThreads"
Send-UciCommand $gameSession 'setoption name Speed value 50'
Send-UciCommand $gameSession 'setoption name OwnBook value false'
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
            if ($line -notmatch '^bestmove ([a-h][1-8][a-h][1-8][nbrq]?)( ponder [a-h][1-8][a-h][1-8][nbrq]?)?$') {
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
