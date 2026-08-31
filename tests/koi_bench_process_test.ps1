param(
    [Parameter(Mandatory = $true)]
    [string]$BenchPath
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $BenchPath -PathType Leaf)) {
    throw "koi-bench executable is missing: $BenchPath"
}

function Invoke-Benchmark([string]$Executable) {
    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $Executable
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
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
        if ($line -notmatch '^position [a-z0-9-]+ depth [1-9][0-9]* nodes [0-9]+ qnodes [0-9]+ tt_hits [0-9]+ score -?[0-9]+ move ([a-h][1-8][a-h][1-8][nbrq]?|0000)$') {
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
