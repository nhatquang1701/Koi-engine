param(
    [Parameter(Mandatory = $true)]
    [string]$EnginePath
)

$ErrorActionPreference = 'Stop'

Import-Module ([System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../support/UciSession.psm1'))) -Force

function Get-HardSafetyMilliseconds {
    param([int]$RemainingMs)

    $overhead = 30
    $doubled = (2 * $overhead) + 15
    $fraction = [Math]::Min(50, [int]($RemainingMs / 20))
    return [Math]::Max($doubled, $fraction)
}

$session = $null
try {
    $session = Start-UciSession -Executable $EnginePath

    Send-UciCommand -Session $session -Command 'uci'
    $null = Read-UciUntil -Session $session -Predicate { param($line) $line -eq 'uciok' } `
        -Description 'the engine to answer uci with uciok'

    Send-UciCommand -Session $session -Command 'isready'
    $null = Read-UciUntil -Session $session -Predicate { param($line) $line -eq 'readyok' } `
        -Description 'the engine to answer isready with readyok'

    # A single timed move: the engine must answer with bestmove and must leave the
    # safety margin on the clock, otherwise it would flag in a real game.
    function Invoke-TimedMove {
        param($Session, [int]$RemainingMs, [int]$IncrementMs, [string]$Label)

        Send-UciCommand -Session $Session -Command 'position startpos'
        $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
        Send-UciCommand -Session $Session -Command (
            "go wtime $RemainingMs btime $RemainingMs winc $IncrementMs binc $IncrementMs")
        $line = Read-UciUntil -Session $Session -Predicate { param($value) $value.StartsWith('bestmove') } `
            -Description "the engine to answer $Label with a bestmove"
        $stopwatch.Stop()

        return [pscustomobject]@{
            ElapsedMs = $stopwatch.ElapsedMilliseconds
            Line      = $line
        }
    }

    # 6+1 is the control that lost on time before the fix.
    $remaining = 6000
    $increment = 1000
    for ($move = 1; $move -le 8; $move++) {
        $safety = Get-HardSafetyMilliseconds -RemainingMs $remaining
        $result = Invoke-TimedMove -Session $session -RemainingMs $remaining -IncrementMs $increment `
            -Label "6+1 move $move"
        if ($result.ElapsedMs -ge ($remaining - $safety)) {
            throw ("6+1 move $move spent $($result.ElapsedMs) ms with $remaining ms on the clock; " +
                "the safety margin of $safety ms was violated.")
        }
        $remaining = $remaining - $result.ElapsedMs + $increment
        if ($remaining -le 0) {
            throw "the 6+1 clock reached $remaining ms after move $move."
        }
    }

    # 1+0 is the tightest increment-less control.
    $remaining = 1000
    for ($move = 1; $move -le 12; $move++) {
        $safety = Get-HardSafetyMilliseconds -RemainingMs $remaining
        $result = Invoke-TimedMove -Session $session -RemainingMs $remaining -IncrementMs 0 `
            -Label "1+0 move $move"
        if ($result.ElapsedMs -ge ($remaining - $safety)) {
            throw ("1+0 move $move spent $($result.ElapsedMs) ms with $remaining ms on the clock; " +
                "the safety margin of $safety ms was violated.")
        }
        $remaining = $remaining - $result.ElapsedMs
        if ($remaining -le 0) {
            throw "the 1+0 clock reached $remaining ms after move $move."
        }
    }

    # An explicit movetime must be honoured without overrunning.
    Send-UciCommand -Session $session -Command 'position startpos'
    $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
    Send-UciCommand -Session $session -Command 'go movetime 500'
    $null = Read-UciUntil -Session $session -Predicate { param($value) $value.StartsWith('bestmove') } `
        -Description 'a bestmove after go movetime 500'
    $stopwatch.Stop()
    if ($stopwatch.ElapsedMilliseconds -ge 500) {
        throw "a go movetime 500 search took $($stopwatch.ElapsedMilliseconds) ms."
    }

    # Slow Mover tunes clock allocation; it must not extend a movetime request.
    Send-UciCommand -Session $session -Command 'setoption name Slow Mover value 300'
    Send-UciCommand -Session $session -Command 'position startpos'
    $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
    Send-UciCommand -Session $session -Command 'go movetime 300'
    $null = Read-UciUntil -Session $session -Predicate { param($value) $value.StartsWith('bestmove') } `
        -Description 'a bestmove after go movetime 300 with Slow Mover 300'
    $stopwatch.Stop()
    Send-UciCommand -Session $session -Command 'setoption name Slow Mover value 100'
    if ($stopwatch.ElapsedMilliseconds -ge 500) {
        throw "Slow Mover 300 extended a go movetime 300 search to $($stopwatch.ElapsedMilliseconds) ms."
    }

    # Fixed-depth searches are not governed by the clock and must stay fast.
    Send-UciCommand -Session $session -Command 'position startpos'
    $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
    Send-UciCommand -Session $session -Command 'go depth 2'
    $null = Read-UciUntil -Session $session -Predicate { param($value) $value.StartsWith('bestmove') } `
        -Description 'a bestmove after go depth 2'
    $stopwatch.Stop()
    if ($stopwatch.ElapsedMilliseconds -gt 5000) {
        throw "a go depth 2 search took $($stopwatch.ElapsedMilliseconds) ms."
    }

    # Mixed depth/node and clock limits: depth and nodes stay strict upper
    # bounds, but a supplied clock must still bound the search. Before the fix
    # these commands ignored the clock entirely and ran for several seconds.
    foreach ($mixed in @(
            @{ Command = 'go depth 6 wtime 200 btime 200'; Label = 'go depth 6 with a 200 ms clock' },
            @{ Command = 'go nodes 2000000 wtime 200 btime 200'; Label = 'go nodes 2000000 with a 200 ms clock' })) {
        Send-UciCommand -Session $session -Command 'position startpos'
        $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
        Send-UciCommand -Session $session -Command $mixed.Command
        $null = Read-UciUntil -Session $session -Predicate { param($value) $value.StartsWith('bestmove') } `
            -Description "a bestmove after $($mixed.Label)"
        $stopwatch.Stop()
        if ($stopwatch.ElapsedMilliseconds -gt 1500) {
            throw "$($mixed.Label) took $($stopwatch.ElapsedMilliseconds) ms; the clock guard did not fire."
        }
    }

    # A ponderhit must always be answered with a bestmove. The controller used to
    # stop the ponder search and return silently when its prediction could not be
    # validated, which leaves the GUI waiting and flags the engine.
    Send-UciCommand -Session $session -Command 'setoption name Ponder value true'
    Send-UciCommand -Session $session -Command 'position startpos moves e2e4'
    Send-UciCommand -Session $session -Command 'go ponder wtime 2500 btime 2500 winc 0 binc 0'
    $null = Read-UciUntil -Session $session -Predicate { param($value) $value.StartsWith('info depth ') } `
        -Description 'an info line during the ponder search'
    Send-UciCommand -Session $session -Command 'ponderhit'
    $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
    $null = Read-UciUntil -Session $session -Predicate { param($value) $value.StartsWith('bestmove') } `
        -Description 'a bestmove after ponderhit'
    $stopwatch.Stop()
    if ($stopwatch.ElapsedMilliseconds -gt 5000) {
        throw "a ponderhit took $($stopwatch.ElapsedMilliseconds) ms to answer."
    }

    Write-Output 'Time safety UCI transcript passed.'
}
finally {
    if ($null -ne $session) {
        Stop-UciSession -Session $session
    }
}
