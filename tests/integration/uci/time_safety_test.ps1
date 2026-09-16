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
    param([string]$Path)

    $info = [System.Diagnostics.ProcessStartInfo]::new()
    $info.FileName = $Path
    $info.RedirectStandardInput = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true

    $process = [System.Diagnostics.Process]::Start($info)
    return [pscustomobject]@{
        Process   = $process
        StderrTask = $process.StandardError.ReadToEndAsync()
    }
}

function Send-Command {
    param($Session, [string]$Command)

    $Session.Process.StandardInput.WriteLine($Command)
    $Session.Process.StandardInput.Flush()
}

function Read-Line {
    param($Session)

    $task = $Session.Process.StandardOutput.ReadLineAsync()
    if (-not $task.Wait($TimeoutMilliseconds)) {
        throw 'Timed out waiting for engine output.'
    }
    return $task.Result
}

function Read-Until {
    param($Session, [scriptblock]$Predicate, [string]$Failure)

    while ($true) {
        $line = Read-Line -Session $Session
        if ($null -eq $line) {
            throw $Failure
        }
        if (& $Predicate $line) {
            return $line
        }
    }
}

function Finish-Engine {
    param($Session)

    if (-not $Session.Process.HasExited) {
        Send-Command -Session $Session -Command 'quit'
        $Session.Process.StandardInput.Close()
        if (-not $Session.Process.WaitForExit(5000)) {
            $Session.Process.Kill($true)
        }
    }
    $null = $Session.StderrTask.GetAwaiter().GetResult()
    $Session.Process.Dispose()
}

function Get-HardSafetyMilliseconds {
    param([int]$RemainingMs)

    $overhead = 30
    $doubled = (2 * $overhead) + 15
    $fraction = [Math]::Min(50, [int]($RemainingMs / 20))
    return [Math]::Max($doubled, $fraction)
}

$session = $null
try {
    $session = Start-Engine -Path $EnginePath

    Send-Command -Session $session -Command 'uci'
    $null = Read-Until -Session $session -Predicate { param($line) $line -eq 'uciok' } `
        -Failure 'the engine must answer uci with uciok.'

    Send-Command -Session $session -Command 'isready'
    $null = Read-Until -Session $session -Predicate { param($line) $line -eq 'readyok' } `
        -Failure 'the engine must answer isready with readyok.'

    # A single timed move: the engine must answer with bestmove and must leave the
    # safety margin on the clock, otherwise it would flag in a real game.
    function Invoke-TimedMove {
        param($Session, [int]$RemainingMs, [int]$IncrementMs, [string]$Label)

        Send-Command -Session $Session -Command 'position startpos'
        $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
        Send-Command -Session $Session -Command (
            "go wtime $RemainingMs btime $RemainingMs winc $IncrementMs binc $IncrementMs")
        $line = Read-Until -Session $Session -Predicate { param($value) $value.StartsWith('bestmove') } `
            -Failure "the engine must answer $Label with a bestmove."
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
    Send-Command -Session $session -Command 'position startpos'
    $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
    Send-Command -Session $session -Command 'go movetime 500'
    $null = Read-Until -Session $session -Predicate { param($value) $value.StartsWith('bestmove') } `
        -Failure 'a movetime search must answer with a bestmove.'
    $stopwatch.Stop()
    if ($stopwatch.ElapsedMilliseconds -ge 500) {
        throw "a go movetime 500 search took $($stopwatch.ElapsedMilliseconds) ms."
    }

    # Fixed-depth searches are not governed by the clock and must stay fast.
    Send-Command -Session $session -Command 'position startpos'
    $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
    Send-Command -Session $session -Command 'go depth 2'
    $null = Read-Until -Session $session -Predicate { param($value) $value.StartsWith('bestmove') } `
        -Failure 'a fixed-depth search must answer with a bestmove.'
    $stopwatch.Stop()
    if ($stopwatch.ElapsedMilliseconds -gt 5000) {
        throw "a go depth 2 search took $($stopwatch.ElapsedMilliseconds) ms."
    }

    # A ponderhit must always be answered with a bestmove. The controller used to
    # stop the ponder search and return silently when its prediction could not be
    # validated, which leaves the GUI waiting and flags the engine.
    Send-Command -Session $session -Command 'setoption name Ponder value true'
    Send-Command -Session $session -Command 'position startpos moves e2e4'
    Send-Command -Session $session -Command 'go ponder wtime 2500 btime 2500 winc 0 binc 0'
    $null = Read-Until -Session $session -Predicate { param($value) $value.StartsWith('info depth ') } `
        -Failure 'a ponder search must publish an info line.'
    Send-Command -Session $session -Command 'ponderhit'
    $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
    $null = Read-Until -Session $session -Predicate { param($value) $value.StartsWith('bestmove') } `
        -Failure 'a ponderhit must always be answered with a bestmove.'
    $stopwatch.Stop()
    if ($stopwatch.ElapsedMilliseconds -gt 5000) {
        throw "a ponderhit took $($stopwatch.ElapsedMilliseconds) ms to answer."
    }

    Write-Output 'Time safety UCI transcript passed.'
}
finally {
    if ($null -ne $session) {
        Finish-Engine -Session $session
    }
}
