# UciSession.psm1
#
# Shared UCI process-session helpers for the Koi integration tests.
#
# The UCI integration scripts used to each carry their own copy of the engine
# launch / read / shutdown plumbing.  Import this module instead:
#
#   Import-Module ([System.IO.Path]::GetFullPath(
#       (Join-Path $PSScriptRoot '../../support/UciSession.psm1'))) -Force
#
# Session objects returned by Start-UciSession expose:
#   Process    : the System.Diagnostics.Process running the engine
#   StderrTask : a pending ReadToEndAsync() task for the engine's stderr
#   Lines      : the transcript accumulated by Read-UciLine/Complete-UciSession
#
# The read timeout honours $env:KOI_UCI_TIMEOUT_MS (milliseconds, default
# 15000).  Every process started here is tracked in the module so a caller's
# trap handler can call Stop-AllUciSessions and never orphan an engine.

$script:UciTimeoutMilliseconds = if ($env:KOI_UCI_TIMEOUT_MS) {
    [int]$env:KOI_UCI_TIMEOUT_MS
} else {
    15000
}

# A hard upper bound for graceful "quit" handling during session teardown.
$script:UciShutdownMilliseconds = 5000

$script:KoiSessions = [System.Collections.Generic.List[System.Diagnostics.Process]]::new()

$script:UciTranscriptLinePattern =
    '^(id |option |info depth [1-9][0-9]* seldepth [0-9]+ multipv [1-9][0-9]*( score (cp|mate) -?[0-9]+( (lowerbound|upperbound))?( wdl [0-9]+ [0-9]+ [0-9]+)?)? nodes [0-9]+ nps [0-9]+ hashfull [0-9]+ time [0-9]+ pv( [a-h][1-8][a-h][1-8][nbrq]?)*( tbhits [0-9]+)?$)'

function Start-UciSession {
    <#
    .SYNOPSIS
        Start an engine process and return a UCI session object.
    .DESCRIPTION
        Launches $Executable with redirected stdin/stdout/stderr, registers the
        process for trap-based cleanup, and returns the session object consumed
        by the other helpers in this module.
    #>
    param(
        [Parameter(Mandatory = $true)]
        [string]$Executable,
        [string]$WorkingDirectory = ''
    )

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
        throw "Unable to start engine at '$Executable'."
    }
    $script:KoiSessions.Add($process)

    return [pscustomobject]@{
        Process = $process
        StderrTask = $process.StandardError.ReadToEndAsync()
        Lines = [System.Collections.Generic.List[string]]::new()
    }
}

function Stop-AllUciSessions {
    <#
    .SYNOPSIS
        Kill every engine process started through Start-UciSession.
    .DESCRIPTION
        Intended for a script-level trap so a failed assertion can never leave a
        running koi-engine.exe behind.  Already exited processes are ignored.
    #>
    foreach ($tracked in $script:KoiSessions) {
        if ($null -ne $tracked -and -not $tracked.HasExited) {
            try { $tracked.Kill() } catch { }
        }
    }
}

function Send-UciCommand {
    <#
    .SYNOPSIS
        Write one UCI command line to the engine's stdin and flush it.
    #>
    param(
        [Parameter(Mandatory = $true)]
        $Session,
        [Parameter(Mandatory = $true)]
        [string]$Command
    )

    $Session.Process.StandardInput.WriteLine($Command)
    $Session.Process.StandardInput.Flush()
}

function Stop-TimedOutSession {
    <#
    .SYNOPSIS
        Kill a session that exceeded its timeout and rethrow the failure.
    #>
    param(
        [Parameter(Mandatory = $true)]
        $Session,
        [Parameter(Mandatory = $true)]
        [string]$Message
    )

    if (-not $Session.Process.HasExited) {
        $Session.Process.Kill()
        $Session.Process.WaitForExit()
    }
    throw $Message
}

function Read-UciLine {
    <#
    .SYNOPSIS
        Read one stdout line, appending it to the session transcript.
    .DESCRIPTION
        Waits up to $env:KOI_UCI_TIMEOUT_MS (default 15000) for a line.  A
        timeout kills the engine through Stop-TimedOutSession; a closed stdout
        raises a terminating error.  The returned line is also appended to
        $Session.Lines so Complete-UciSession can return the whole transcript
        without double-counting it.
    #>
    param(
        [Parameter(Mandatory = $true)]
        $Session,
        [string]$Description = 'engine output'
    )

    $readTask = $Session.Process.StandardOutput.ReadLineAsync()
    if (-not $readTask.Wait($script:UciTimeoutMilliseconds)) {
        Stop-TimedOutSession $Session "Timed out waiting for $Description."
    }

    $line = $readTask.GetAwaiter().GetResult()
    if ($null -eq $line) {
        throw "koi-engine closed stdout while waiting for $Description."
    }
    $Session.Lines.Add($line)
    return $line
}

function Read-UciUntil {
    <#
    .SYNOPSIS
        Read lines until $Predicate accepts one, then return that line.
    .DESCRIPTION
        With -ValidateUciOutput, any skipped line that is not a recognised UCI
        transcript line (id/option/info) fails the caller.  Without it, skipped
        lines are ignored exactly as the older permissive waiter did.
    #>
    param(
        [Parameter(Mandatory = $true)]
        $Session,
        [Parameter(Mandatory = $true)]
        [scriptblock]$Predicate,
        [string]$Description = 'engine output',
        [switch]$ValidateUciOutput
    )

    while ($true) {
        $line = Read-UciLine $Session $Description
        if (& $Predicate $line) {
            return $line
        }
        if ($ValidateUciOutput -and $line -notmatch $script:UciTranscriptLinePattern) {
            throw "Invalid UCI output while waiting for $Description`: $line"
        }
    }
}

function Complete-UciSession {
    <#
    .SYNOPSIS
        Drain the session, shut the engine down and return the transcript.
    .DESCRIPTION
        Captures any remaining stdout, optionally sends "quit", and waits for
        the process to exit.  A non-zero exit code or any stderr output fails
        the caller.  Returns the entire accumulated transcript ($Session.Lines),
        which is why read helpers must not count those lines twice.
    #>
    param(
        [Parameter(Mandatory = $true)]
        $Session,
        [bool]$SendQuit
    )

    $stdoutTailTask = $Session.Process.StandardOutput.ReadToEndAsync()
    if ($SendQuit -and -not $Session.Process.HasExited) {
        Send-UciCommand $Session 'quit'
    }
    $Session.Process.StandardInput.Close()

    if (-not $Session.Process.WaitForExit($script:UciTimeoutMilliseconds)) {
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

function Stop-UciSession {
    <#
    .SYNOPSIS
        Best-effort cleanup of a session without asserting on its exit state.
    .DESCRIPTION
        Sends "quit" when the engine is still running, escalates to a kill if it
        does not exit promptly, then drains stderr and disposes the process.
        Never throws, so it is safe inside a finally block.
    #>
    param(
        [Parameter(Mandatory = $true)]
        $Session
    )

    if ($null -eq $Session) {
        return
    }
    if (-not $Session.Process.HasExited) {
        try { Send-UciCommand $Session 'quit' } catch { }
        try { $Session.Process.StandardInput.Close() } catch { }
        if (-not $Session.Process.WaitForExit($script:UciShutdownMilliseconds)) {
            try { $Session.Process.Kill($true) } catch {
                try { $Session.Process.Kill() } catch { }
            }
        }
    }
    try { $null = $Session.StderrTask.GetAwaiter().GetResult() } catch { }
    try { $Session.Process.Dispose() } catch { }
}

function Test-SearchInfo {
    <#
    .SYNOPSIS
        Return $true when $Line is a well-formed search info line.
    #>
    param(
        [Parameter(Mandatory = $true)]
        [string]$Line
    )

    # UCI permits an info line without score. Koi uses that form for a
    # selective estimate with no proven bound direction while retaining PV.
    return $Line -match '^info depth [1-9][0-9]* seldepth [0-9]+ multipv ([1-9]|1[0-6])( score (cp|mate) -?[0-9]+( (lowerbound|upperbound))?( wdl [0-9]+ [0-9]+ [0-9]+)?)? nodes [0-9]+ nps [0-9]+ hashfull [0-9]+ time [0-9]+ pv( [a-h][1-8][a-h][1-8][nbrq]?)*( tbhits [0-9]+)?$'
}

Export-ModuleMember -Function Start-UciSession, Stop-AllUciSessions, Send-UciCommand,
    Stop-TimedOutSession, Read-UciLine, Read-UciUntil, Complete-UciSession,
    Stop-UciSession, Test-SearchInfo
