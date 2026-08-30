param(
    [Parameter(Mandatory = $true)]
    [string]$EnginePath
)

$ErrorActionPreference = 'Stop'

function Invoke-UciTranscript([string]$Transcript) {
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

    $process.StandardInput.Write($Transcript)
    $process.StandardInput.Close()
    $output = $process.StandardOutput.ReadToEnd()
    $diagnostics = $process.StandardError.ReadToEnd()
    $process.WaitForExit()

    if ($process.ExitCode -ne 0) {
        throw "koi-engine exited with $($process.ExitCode): $diagnostics"
    }
    if ($diagnostics.Length -ne 0) {
        throw "koi-engine wrote diagnostics for a valid transcript: $diagnostics"
    }

    return @($output -split "`r?`n" | Where-Object { $_.Length -ne 0 })
}

$handshake = @(Invoke-UciTranscript "uci`nisready`nposition startpos`ngo`nquit`n")
$expectedPrefix = @(
    'id name Koi Engine',
    'id author Koi Engine contributors',
    'option name RandomSeed type spin default 0 min 0 max 2147483647',
    'uciok',
    'readyok'
)

if ($handshake.Count -ne 6) {
    throw "Unexpected handshake output: $($handshake -join ' | ')"
}
for ($index = 0; $index -lt $expectedPrefix.Count; ++$index) {
    if ($handshake[$index] -cne $expectedPrefix[$index]) {
        throw "Unexpected handshake output: $($handshake -join ' | ')"
    }
}

$initialMoves = @(
    'a2a3', 'a2a4', 'b2b3', 'b2b4', 'c2c3', 'c2c4', 'd2d3', 'd2d4',
    'e2e3', 'e2e4', 'f2f3', 'f2f4', 'g2g3', 'g2g4', 'h2h3', 'h2h4',
    'b1a3', 'b1c3', 'g1f3', 'g1h3'
)

if ($handshake[5] -notmatch '^bestmove ([a-h][1-8][a-h][1-8][nbrq]?)$' -or
    $initialMoves -notcontains $Matches[1]) {
    throw "Expected a legal initial bestmove, received: $($handshake[5])"
}

$checkmate = @(Invoke-UciTranscript "position fen 7k/6Q1/5K2/8/8/8/8/8 b - - 0 1`ngo`nquit`n")
if ($checkmate.Count -ne 1 -or $checkmate[0] -cne 'bestmove 0000') {
    throw "Expected checkmate to return bestmove 0000, received: $($checkmate -join ' | ')"
}
