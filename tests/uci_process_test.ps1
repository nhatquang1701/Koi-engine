param(
    [Parameter(Mandatory = $true)]
    [string]$EnginePath
)

$ErrorActionPreference = 'Stop'
$TimeoutMilliseconds = 5000

function Start-UciSession {
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
    return $Line -match '^info depth [1-9][0-9]* score (cp|mate) -?[0-9]+ nodes [0-9]+ nps [0-9]+ time [0-9]+ pv( [a-h][1-8][a-h][1-8][nbrq]?)*$'
}

$session = Start-UciSession
Send-UciCommand $session 'uci'
$expectedHandshake = @(
    'id name Koi Engine',
    'id author Koi Engine contributors',
    'option name RandomSeed type spin default 0 min 0 max 2147483647',
    'option name Hash type spin default 16 min 1 max 4096',
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

Send-UciCommand $session 'position startpos'
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
if ($bestmove -notmatch '^bestmove ([a-h][1-8][a-h][1-8][nbrq]?)$' -or
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
