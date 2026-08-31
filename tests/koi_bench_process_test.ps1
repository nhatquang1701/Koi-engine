param(
    [Parameter(Mandatory = $true)]
    [string]$BenchPath
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $BenchPath -PathType Leaf)) {
    throw "koi-bench executable is missing: $BenchPath"
}

$startInfo = [System.Diagnostics.ProcessStartInfo]::new()
$startInfo.FileName = $BenchPath
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

$stdout = $stdoutTask.GetAwaiter().GetResult()
$stderr = $stderrTask.GetAwaiter().GetResult()
if ($process.ExitCode -ne 0) {
    throw "koi-bench exited with $($process.ExitCode): $stderr"
}
if ($stderr.Length -ne 0) {
    throw "koi-bench wrote stderr: $stderr"
}
if ($stdout -notmatch '^Koi benchmark\r?\nposition ') {
    throw "koi-bench must emit only its benchmark header and results: $stdout"
}
if ($stdout -match '^(id |option |uciok|readyok|info |bestmove )') {
    throw "koi-bench must not emit UCI protocol output: $stdout"
}
