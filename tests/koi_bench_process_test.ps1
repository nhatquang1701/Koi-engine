param(
    [Parameter(Mandatory = $true)]
    [string]$BenchPath
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $BenchPath -PathType Leaf)) {
    throw "koi-bench executable is missing: $BenchPath"
}

function Invoke-Benchmark([string]$Executable, [string]$Arguments = '') {
    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $Executable
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.Arguments = $Arguments
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true

    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    if (-not $process.Start()) {
        throw 'Unable to start koi-bench.'
    }

    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit(10000)) {
        $process.Kill()
        $process.WaitForExit()
        throw 'koi-bench did not exit within 10000 ms.'
    }

    return [pscustomobject]@{
        ExitCode = $process.ExitCode
        Stdout = $stdoutTask.GetAwaiter().GetResult()
        Stderr = $stderrTask.GetAwaiter().GetResult()
    }
}

function Assert-BenchmarkOutput($Result) {
    if ($Result.ExitCode -ne 0) {
        throw "koi-bench exited with $($Result.ExitCode): $($Result.Stderr)"
    }
    if ($Result.Stderr.Length -ne 0) {
        throw "koi-bench wrote stderr: $($Result.Stderr)"
    }

    $lines = @($Result.Stdout -split "`r?`n" | Where-Object { $_.Length -ne 0 })
    if ($lines.Count -lt 2 -or $lines[0] -cne 'Koi benchmark') {
        throw "koi-bench must begin with its benchmark header: $($Result.Stdout)"
    }

    foreach ($line in $lines) {
        if ($line -match '^(id |option |uciok$|readyok$|info |bestmove )') {
            throw "koi-bench must not emit UCI protocol output: $line"
        }
    }
    foreach ($line in $lines[1..($lines.Count - 1)]) {
        if ($line -notmatch '^position [a-z0-9_-]+ depth [1-9][0-9]* nodes [0-9]+ qnodes [0-9]+ tt_hits [0-9]+ score -?[0-9]+ expected ([a-h][1-8][a-h][1-8][nbrq]?) move ([a-h][1-8][a-h][1-8][nbrq]?|0000) match [01]$') {
            throw "koi-bench emitted an invalid benchmark result: $line"
        }
    }
}

$first = Invoke-Benchmark $BenchPath
$second = Invoke-Benchmark $BenchPath
Assert-BenchmarkOutput $first
Assert-BenchmarkOutput $second
if ($first.Stdout -cne $second.Stdout) {
    throw "koi-bench output must be byte-identical across runs: first=$($first.Stdout) second=$($second.Stdout)"
}

$maximumThreads = [Math]::Max(1, [Math]::Min(64, [Environment]::ProcessorCount))
$benchmarkThreads = [Math]::Max(1, [Math]::Min(2, $maximumThreads))
$timed = Invoke-Benchmark $BenchPath "--threads $benchmarkThreads --speed 50 --timed"
if ($timed.ExitCode -ne 0) {
    throw "configured koi-bench exited with $($timed.ExitCode): $($timed.Stderr)"
}
if ($timed.Stderr.Length -ne 0) {
    throw "configured koi-bench wrote stderr: $($timed.Stderr)"
}
$timedLines = @($timed.Stdout -split "`r?`n" | Where-Object { $_.Length -ne 0 })
if ($timedLines.Count -lt 3 -or $timedLines[0] -cne 'Koi benchmark' -or
    $timedLines[1] -cne "config threads $benchmarkThreads speed 50 timed 1") {
    throw "configured koi-bench must report its thread, speed, and timed settings: $($timed.Stdout)"
}
foreach ($line in $timedLines[2..($timedLines.Count - 1)]) {
    if ($line -notmatch '^position [a-z0-9_-]+ depth [1-9][0-9]* nodes [0-9]+ qnodes [0-9]+ tt_hits [0-9]+ score -?[0-9]+ expected ([a-h][1-8][a-h][1-8][nbrq]?) move ([a-h][1-8][a-h][1-8][nbrq]?|0000) match [01] elapsed_ms [0-9]+ nps [0-9]+$') {
        throw "configured koi-bench emitted an invalid timed result: $line"
    }
}
