param(
    [Parameter(Mandatory = $true)]
    [string]$EnginePath
)

$ErrorActionPreference = 'Stop'
Import-Module ([System.IO.Path]::GetFullPath(
    (Join-Path $PSScriptRoot '../../support/UciSession.psm1'))) -Force

trap {
    Stop-AllUciSessions
    break
}

$session = Start-UciSession -Executable $EnginePath
try {
    Send-UciCommand $session 'uci'
    while ($true) {
        if ((Read-UciLine $session 'UCI handshake before perft') -ceq 'uciok') {
            break
        }
    }
    Send-UciCommand $session 'isready'
    if ((Read-UciLine $session 'initial readyok before perft') -cne 'readyok') {
        throw 'The engine did not become ready before the perft checks.'
    }

    Send-UciCommand $session 'position startpos'
    Send-UciCommand $session 'go perft 1'
    $rootMoveCount = 0
    $totalNodes = $null
    while ($null -eq $totalNodes) {
        $line = Read-UciLine $session 'completed depth-one perft output'
        if ($line -match '^info string [a-h][1-8][a-h][1-8][nbrq]?: [0-9]+$') {
            ++$rootMoveCount
        } elseif ($line -match '^info string Nodes searched: ([0-9]+)$') {
            $totalNodes = [uint64]$Matches[1]
        } elseif ($line -like 'bestmove *') {
            throw 'go perft must not emit bestmove.'
        } else {
            throw "Unexpected depth-one perft output: $line"
        }
    }
    if ($rootMoveCount -ne 20 -or $totalNodes -ne 20) {
        throw "Start-position perft depth 1 reported $rootMoveCount root moves and $totalNodes nodes."
    }
    Send-UciCommand $session 'isready'
    if ((Read-UciLine $session 'readyok after completed perft') -cne 'readyok') {
        throw 'A completed perft did not leave the controller ready.'
    }

    Send-UciCommand $session 'position startpos'
    Send-UciCommand $session 'go perft 10'
    Send-UciCommand $session 'isready'

    # Depth 10 keeps the perft worker occupied long enough to prove that the
    # protocol loop can still answer readiness. The old inline implementation
    # blocks here; stop the child through the trap if it does.
    $readyTask = $session.Process.StandardOutput.ReadLineAsync()
    if (-not $readyTask.Wait(1500)) {
        throw 'isready was blocked by go perft instead of returning promptly.'
    }
    if ($readyTask.GetAwaiter().GetResult() -cne 'readyok') {
        throw 'Expected readyok while the perft worker was active.'
    }

    Send-UciCommand $session 'stop'
    Send-UciCommand $session 'isready'
    if ((Read-UciLine $session 'readyok after stopping perft') -cne 'readyok') {
        throw 'The controller did not join the stopped perft worker.'
    }

    Send-UciCommand $session 'quit'
    $lines = @(Complete-UciSession $session $false)
    if (@($lines | Where-Object { $_ -like 'bestmove *' }).Count -ne 0) {
        throw 'go perft must not emit bestmove.'
    }
    if (@($lines | Where-Object { $_ -like 'info string Nodes searched:*' }).Count -ne 1) {
        throw 'Only the completed perft may publish a node total.'
    }
    $reportedRootMoves = @($lines | Where-Object {
        $_ -match '^info string [a-h][1-8][a-h][1-8][nbrq]?: [0-9]+$'
    })
    if ($reportedRootMoves.Count -ne 20) {
        throw "A cancelled perft published root-move output; expected only 20 completed depth-one rows, got $($reportedRootMoves.Count)."
    }
    Write-Output 'go perft remained responsive to isready and stop.'
} finally {
    Stop-UciSession $session
}
