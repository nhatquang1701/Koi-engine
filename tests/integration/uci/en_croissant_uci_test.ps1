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

function Start-Engine {
    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $EnginePath
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardInput = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true

    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    if (-not $process.Start()) {
        throw "Unable to start Koi Engine at '$EnginePath'."
    }

    return [pscustomobject]@{
        Process = $process
        StderrTask = $process.StandardError.ReadToEndAsync()
        Lines = [System.Collections.Generic.List[string]]::new()
    }
}

function Send-Command($Session, [string]$Command) {
    $Session.Process.StandardInput.WriteLine($Command)
    $Session.Process.StandardInput.Flush()
}

function Read-Line($Session, [string]$Description) {
    $task = $Session.Process.StandardOutput.ReadLineAsync()
    if (-not $task.Wait($TimeoutMilliseconds)) {
        if (-not $Session.Process.HasExited) {
            $Session.Process.Kill()
            $Session.Process.WaitForExit()
        }
        try { $null = $task.GetAwaiter().GetResult() } catch { }
        throw "Timed out waiting for $Description."
    }
    $line = $task.GetAwaiter().GetResult()
    if ($null -eq $line) {
        throw "Koi Engine closed stdout while waiting for $Description."
    }
    $Session.Lines.Add($line)
    return $line
}

function Read-Until($Session, [string]$Description, [scriptblock]$Predicate) {
    while ($true) {
        $line = Read-Line $Session $Description
        if (& $Predicate $line) {
            return $line
        }
        if ($line -notmatch '^(id |option |info depth [1-9][0-9]* seldepth [0-9]+ multipv [1-9][0-9]* score (cp|mate) -?[0-9]+ nodes [0-9]+ nps [0-9]+ time [0-9]+ pv( [a-h][1-8][a-h][1-8][nbrq]?)*( tbhits [0-9]+)?$)') {
            throw "Invalid UCI output while waiting for $Description`: $line"
        }
    }
}

function Finish-Engine($Session) {
    if (-not $Session.Process.HasExited) {
        Send-Command $Session 'quit'
        $Session.Process.StandardInput.Close()
        if (-not $Session.Process.WaitForExit($TimeoutMilliseconds)) {
            $Session.Process.Kill()
            $Session.Process.WaitForExit()
            throw 'Koi Engine did not exit after quit.'
        }
    }
    $stderr = $Session.StderrTask.GetAwaiter().GetResult()
    if ($Session.Process.ExitCode -ne 0) {
        throw "Koi Engine exited with $($Session.Process.ExitCode): $stderr"
    }
    if ($stderr.Length -ne 0) {
        throw "Koi Engine wrote diagnostics for a valid transcript: $stderr"
    }
}

$session = Start-Engine
try {
    Send-Command $session 'uci'
    if ((Read-Until $session 'uciok' { param($line) $line -ceq 'uciok' }) -cne 'uciok') {
        throw 'UCI handshake did not complete.'
    }

    Send-Command $session 'isready'
    if ((Read-Until $session 'initial readyok' { param($line) $line -ceq 'readyok' }) -cne 'readyok') {
        throw 'Initial readiness check did not complete.'
    }

    Send-Command $session 'setoption name Hash value 64'
    Send-Command $session 'setoption name Threads value 1'
    Send-Command $session 'setoption name uci_analysemode value true'
    Send-Command $session 'setoption name multipv value 3'
    Send-Command $session 'position startpos moves e2e4 e7e5 g1f3'
    Send-Command $session 'go depth 2'
    Send-Command $session 'stop'
    $firstBestmove = Read-Until $session 'bestmove after stopped analysis' { param($line) $line -match '^bestmove [a-h][1-8][a-h][1-8][nbrq]?$' }
    if ($firstBestmove -notmatch '^bestmove [a-h][1-8][a-h][1-8][nbrq]?$') {
        throw "Stopped analysis emitted an invalid bestmove: $firstBestmove"
    }

    Send-Command $session 'isready'
    if ((Read-Until $session 'readyok after stopped analysis' { param($line) $line -ceq 'readyok' }) -cne 'readyok') {
        throw 'Stopped analysis did not fence at readyok.'
    }

    Send-Command $session 'position startpos moves e2e4 e7e5 g1f3'
    Send-Command $session 'go depth 2'
    $secondBestmoves = [System.Collections.Generic.List[string]]::new()
    $multiPvRanks = [System.Collections.Generic.HashSet[int]]::new()
    while ($secondBestmoves.Count -eq 0) {
        $line = Read-Line $session 'MultiPV bestmove'
        if ($line -match '^bestmove [a-h][1-8][a-h][1-8][nbrq]?$') {
            $secondBestmoves.Add($line)
        } elseif ($line -match ' multipv ([1-9][0-9]*) score ') {
            $null = $multiPvRanks.Add([int]$Matches[1])
        } elseif ($line -notmatch '^info depth [1-9][0-9]* seldepth [0-9]+ multipv [1-9][0-9]* score (cp|mate) -?[0-9]+ nodes [0-9]+ nps [0-9]+ time [0-9]+ pv( [a-h][1-8][a-h][1-8][nbrq]?)*( tbhits [0-9]+)?$') {
            throw "Invalid MultiPV output: $line"
        }
    }
    if ($secondBestmoves.Count -ne 1 -or -not ($multiPvRanks.Contains(1) -and $multiPvRanks.Contains(2) -and $multiPvRanks.Contains(3))) {
        throw "MultiPV search did not emit ranks 1-3 and exactly one bestmove."
    }

    Send-Command $session 'position startpos moves e2e4 e7e5'
    Send-Command $session 'go infinite'
    Send-Command $session 'isready'
    if ((Read-Until $session 'readyok during infinite analysis' { param($line) $line -ceq 'readyok' }) -cne 'readyok') {
        throw 'Infinite analysis did not remain ready.'
    }
    Send-Command $session 'stop'
    $thirdBestmove = Read-Until $session 'bestmove after infinite analysis' { param($line) $line -match '^bestmove [a-h][1-8][a-h][1-8][nbrq]?$' }
    if ($thirdBestmove -notmatch '^bestmove [a-h][1-8][a-h][1-8][nbrq]?$') {
        throw "Infinite analysis emitted an invalid bestmove: $thirdBestmove"
    }

    Send-Command $session 'register later'
    Send-Command $session 'debug on'
    Finish-Engine $session

    $bestmoves = @($session.Lines | Where-Object { $_ -like 'bestmove *' })
    if ($bestmoves.Count -ne 3) {
        throw "En Croissant transcript emitted $($bestmoves.Count) bestmoves; expected exactly three."
    }
    foreach ($line in $session.Lines) {
        if ($line -notmatch '^(id |option |uciok$|readyok$|info )' -and $line -notmatch '^bestmove [a-h][1-8][a-h][1-8][nbrq]?$') {
            throw "Transcript contains non-UCI output: $line"
        }
    }
    Write-Output 'En Croissant UCI transcript passed.'
} finally {
    if (-not $session.Process.HasExited) {
        $session.Process.Kill()
        $session.Process.WaitForExit()
    }
    try { $null = $session.StderrTask.GetAwaiter().GetResult() } catch { }
    $session.Process.Dispose()
}
