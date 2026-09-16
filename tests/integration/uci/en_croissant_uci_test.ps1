param(
    [Parameter(Mandatory = $true)]
    [string]$EnginePath
)

$ErrorActionPreference = 'Stop'

Import-Module ([System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\support\UciSession.psm1'))) -Force

$session = Start-UciSession -Executable $EnginePath
try {
    Send-UciCommand $session 'uci'
    if ((Read-UciUntil -Session $session -Description 'uciok' -ValidateUciOutput -Predicate { param($line) $line -ceq 'uciok' }) -cne 'uciok') {
        throw 'UCI handshake did not complete.'
    }

    Send-UciCommand $session 'isready'
    if ((Read-UciUntil -Session $session -Description 'initial readyok' -ValidateUciOutput -Predicate { param($line) $line -ceq 'readyok' }) -cne 'readyok') {
        throw 'Initial readiness check did not complete.'
    }

    Send-UciCommand $session 'setoption name Hash value 64'
    Send-UciCommand $session 'setoption name Threads value 1'
    Send-UciCommand $session 'setoption name uci_analysemode value true'
    Send-UciCommand $session 'setoption name multipv value 3'
    Send-UciCommand $session 'position startpos moves e2e4 e7e5 g1f3'
    Send-UciCommand $session 'go depth 2'
    Send-UciCommand $session 'stop'
    $firstBestmove = Read-UciUntil -Session $session -Description 'bestmove after stopped analysis' -ValidateUciOutput -Predicate { param($line) $line -match '^bestmove [a-h][1-8][a-h][1-8][nbrq]?$' }
    if ($firstBestmove -notmatch '^bestmove [a-h][1-8][a-h][1-8][nbrq]?$') {
        throw "Stopped analysis emitted an invalid bestmove: $firstBestmove"
    }

    Send-UciCommand $session 'isready'
    if ((Read-UciUntil -Session $session -Description 'readyok after stopped analysis' -ValidateUciOutput -Predicate { param($line) $line -ceq 'readyok' }) -cne 'readyok') {
        throw 'Stopped analysis did not fence at readyok.'
    }

    Send-UciCommand $session 'position startpos moves e2e4 e7e5 g1f3'
    Send-UciCommand $session 'go depth 2'
    $secondBestmoves = [System.Collections.Generic.List[string]]::new()
    $multiPvRanks = [System.Collections.Generic.HashSet[int]]::new()
    while ($secondBestmoves.Count -eq 0) {
        $line = Read-UciLine $session 'MultiPV bestmove'
        if ($line -match '^bestmove [a-h][1-8][a-h][1-8][nbrq]?$') {
            $secondBestmoves.Add($line)
        } elseif ($line -match ' multipv ([1-9][0-9]*) score ') {
            $null = $multiPvRanks.Add([int]$Matches[1])
        } elseif ($line -notmatch '^info depth [1-9][0-9]* seldepth [0-9]+ multipv [1-9][0-9]* score (cp|mate) -?[0-9]+ nodes [0-9]+ nps [0-9]+ hashfull [0-9]+ time [0-9]+ pv( [a-h][1-8][a-h][1-8][nbrq]?)*( tbhits [0-9]+)?$') {
            throw "Invalid MultiPV output: $line"
        }
    }
    if ($secondBestmoves.Count -ne 1 -or -not ($multiPvRanks.Contains(1) -and $multiPvRanks.Contains(2) -and $multiPvRanks.Contains(3))) {
        throw "MultiPV search did not emit ranks 1-3 and exactly one bestmove."
    }

    Send-UciCommand $session 'position startpos moves e2e4 e7e5'
    Send-UciCommand $session 'go infinite'
    Send-UciCommand $session 'isready'
    if ((Read-UciUntil -Session $session -Description 'readyok during infinite analysis' -ValidateUciOutput -Predicate { param($line) $line -ceq 'readyok' }) -cne 'readyok') {
        throw 'Infinite analysis did not remain ready.'
    }
    Send-UciCommand $session 'stop'
    $thirdBestmove = Read-UciUntil -Session $session -Description 'bestmove after infinite analysis' -ValidateUciOutput -Predicate { param($line) $line -match '^bestmove [a-h][1-8][a-h][1-8][nbrq]?$' }
    if ($thirdBestmove -notmatch '^bestmove [a-h][1-8][a-h][1-8][nbrq]?$') {
        throw "Infinite analysis emitted an invalid bestmove: $thirdBestmove"
    }

    Send-UciCommand $session 'register later'
    Send-UciCommand $session 'debug on'
    $null = Complete-UciSession $session $true

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
