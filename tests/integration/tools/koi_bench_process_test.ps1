param(
    [Parameter(Mandatory = $true)]
    [string]$BenchPath
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $BenchPath -PathType Leaf)) {
    throw "koi-bench executable is missing: $BenchPath"
}

# Profile JSON scratch files must be unique per run: this test may execute in
# parallel with another test invocation (or another build tree), so fixed names
# in the shared temp directory would let one run read another run's file.
$scratchDirectory = Join-Path ([System.IO.Path]::GetTempPath()) ("koi-bench-" + [System.Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $scratchDirectory -Force | Out-Null

function Invoke-Benchmark([string]$Executable, [string]$Arguments = '', [string]$Label = 'benchmark') {
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
    if (-not $process.WaitForExit(120000)) {
        $process.Kill()
        $process.WaitForExit()
        throw "koi-bench ($Label) did not exit within 120000 ms."
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
    if ($lines.Count -lt 3 -or $lines[0] -cne 'Koi benchmark' -or
        $lines[1] -notmatch '^config threads [1-9][0-9]* speed (?:[1-9]|[1-9][0-9]|100) timed [01] hash (?:cold|warm)(?: suite optional_strength)?$') {
        throw "koi-bench must begin with its benchmark header: $($Result.Stdout)"
    }

    foreach ($line in $lines) {
        if ($line -match '^(id |option |uciok$|readyok$|info |bestmove )') {
            throw "koi-bench must not emit UCI protocol output: $line"
        }
    }
    $positionLines = @($lines | Where-Object { $_ -like 'position *' })
    if ($positionLines.Count -eq 0) {
        throw "koi-bench did not emit any position rows: $($Result.Stdout)"
    }
    foreach ($line in $positionLines) {
        if ($line -notmatch '^position [a-z0-9_-]+ depth [1-9][0-9]* nodes [0-9]+ qnodes [0-9]+ tt_hits [0-9]+ score -?[0-9]+ expected ([a-h][1-8][a-h][1-8][nbrq]?) move ([a-h][1-8][a-h][1-8][nbrq]?|0000) match [01]$') {
            throw "koi-bench emitted an invalid benchmark result: $line"
        }
    }
}

function Get-NormalizedBenchmarkRows($Result) {
    return @($Result.Stdout -split "`r?`n" |
        Where-Object { $_ -like 'position *' } |
        ForEach-Object { $_ -replace 'nodes [0-9]+ qnodes [0-9]+ tt_hits [0-9]+', 'nodes N qnodes Q tt_hits H' }) -join "`n"
}

$first = Invoke-Benchmark $BenchPath '--threads 1 --speed 100' 'reference-1'
$second = Invoke-Benchmark $BenchPath '--threads 1 --speed 100' 'reference-2'
Assert-BenchmarkOutput $first
Assert-BenchmarkOutput $second
if ($first.Stdout -notmatch '(?m)^config threads 1 speed 100 timed 0 hash cold\r?$') {
    throw "the default reference benchmark must identify its cold hash state: $($first.Stdout)"
}
if ($first.Stdout -cne $second.Stdout) {
    throw "koi-bench output must be byte-identical across runs: first=$($first.Stdout) second=$($second.Stdout)"
}

$maximumThreads = [Math]::Max(1, [Math]::Min(64, [Environment]::ProcessorCount))
$benchmarkThreads = [Math]::Max(1, [Math]::Min(2, $maximumThreads))
$threadedVerificationThreads = if ($maximumThreads -ge 4) { 4 } else { $maximumThreads }
$threadedVerificationProfile = Join-Path $scratchDirectory 'threads4-profile.json'
$threadedVerification = Invoke-Benchmark $BenchPath "--threads $threadedVerificationThreads --speed 100 --profile-json `"$threadedVerificationProfile`"" 'threaded-profile'
$threadedVerificationRepeat = Invoke-Benchmark $BenchPath "--threads $threadedVerificationThreads --speed 100" 'threaded-repeat'
Assert-BenchmarkOutput $threadedVerification
Assert-BenchmarkOutput $threadedVerificationRepeat
if ($threadedVerification.Stderr.Length -ne 0 -or $threadedVerificationRepeat.Stderr.Length -ne 0) {
    throw "threaded benchmark verification wrote stderr: $($threadedVerification.Stderr) $($threadedVerificationRepeat.Stderr)"
}
if ($threadedVerification.Stdout -notmatch "(?m)^config threads $threadedVerificationThreads speed 100 timed 0 hash cold\r?$") {
    throw "threaded benchmark must report the required Threads 4 case or safe maximum-thread fallback ($threadedVerificationThreads): $($threadedVerification.Stdout)"
}
if ((Get-NormalizedBenchmarkRows $threadedVerification) -cne (Get-NormalizedBenchmarkRows $threadedVerificationRepeat)) {
    throw "threaded benchmark move/score rows must be deterministic at Threads $threadedVerificationThreads"
}
if (-not (Test-Path -LiteralPath $threadedVerificationProfile -PathType Leaf)) {
    throw "threaded benchmark did not write its verification profile: $threadedVerificationProfile"
}
$threadedVerificationJson = Get-Content -LiteralPath $threadedVerificationProfile -Raw | ConvertFrom-Json
if ($threadedVerificationJson.threads -ne $threadedVerificationThreads -or
    $threadedVerificationJson.speed -ne 100 -or $threadedVerificationJson.timed -ne $false -or
    $threadedVerificationJson.hash_state -cne 'cold' -or
    $threadedVerificationJson.positions.Count -lt 1) {
    throw 'Threads 4 verification profile must preserve its explicit configuration and cold hash label.'
}
foreach ($position in $threadedVerificationJson.positions) {
    if ($position.hash_state -cne $threadedVerificationJson.hash_state) {
        throw 'Threads 4 verification profile positions must match the top-level hash state.'
    }
}
$timed = Invoke-Benchmark $BenchPath "--threads $benchmarkThreads --speed 50 --timed" 'timed'
if ($timed.ExitCode -ne 0) {
    throw "configured koi-bench exited with $($timed.ExitCode): $($timed.Stderr)"
}
if ($timed.Stderr.Length -ne 0) {
    throw "configured koi-bench wrote stderr: $($timed.Stderr)"
}
$timedLines = @($timed.Stdout -split "`r?`n" | Where-Object { $_.Length -ne 0 })
if ($timedLines.Count -lt 3 -or $timedLines[0] -cne 'Koi benchmark' -or
    $timedLines[1] -cne "config threads $benchmarkThreads speed 50 timed 1 hash cold") {
    throw "configured koi-bench must report its thread, speed, and timed settings: $($timed.Stdout)"
}
foreach ($line in $timedLines[2..($timedLines.Count - 1)]) {
    if ($line -notmatch '^position [a-z0-9_-]+ depth [1-9][0-9]* nodes [0-9]+ qnodes [0-9]+ tt_hits [0-9]+ score -?[0-9]+ expected ([a-h][1-8][a-h][1-8][nbrq]?) move ([a-h][1-8][a-h][1-8][nbrq]?|0000) match [01] elapsed_ms [0-9]+ nps [0-9]+$') {
        throw "configured koi-bench emitted an invalid timed result: $line"
    }
}

$coldProfile = Join-Path $scratchDirectory 'cold-profile.json'
$coldReplayProfile = Join-Path $scratchDirectory 'cold-replay-profile.json'
$warmProfile = Join-Path $scratchDirectory 'warm-profile.json'
$timedProfile = Join-Path $scratchDirectory 'timed-profile.json'
$optionalProfile = Join-Path $scratchDirectory 'optional-profile.json'
$cold = Invoke-Benchmark $BenchPath "--profile-json `"$coldProfile`"" 'cold-profile'
$coldReplay = Invoke-Benchmark $BenchPath "--profile-json `"$coldReplayProfile`"" 'cold-replay-profile'
$warm = Invoke-Benchmark $BenchPath "--warm-hash --profile-json `"$warmProfile`"" 'warm-profile'
$timedProfileRun = Invoke-Benchmark $BenchPath "--timed --profile-json `"$timedProfile`"" 'timed-profile'
foreach ($profileRun in @($cold, $coldReplay, $warm, $timedProfileRun)) {
    if ($profileRun.ExitCode -ne 0 -or $profileRun.Stderr.Length -ne 0) {
        throw "profiled koi-bench run failed: $($profileRun.Stderr)"
    }
}
foreach ($profilePath in @($coldProfile, $coldReplayProfile, $warmProfile, $timedProfile)) {
    if (-not (Test-Path -LiteralPath $profilePath -PathType Leaf)) {
        throw "koi-bench did not write profile JSON: $profilePath"
    }
}
$coldJson = Get-Content -LiteralPath $coldProfile -Raw | ConvertFrom-Json
$coldReplayText = Get-Content -LiteralPath $coldReplayProfile -Raw
$warmJson = Get-Content -LiteralPath $warmProfile -Raw | ConvertFrom-Json
$timedJson = Get-Content -LiteralPath $timedProfile -Raw | ConvertFrom-Json
if ((Get-Content -LiteralPath $coldProfile -Raw) -cne $coldReplayText) {
    throw 'untimed profile JSON must be byte-identical across repeated runs.'
}
if ($coldJson.build -cne 'Koi Engine 1.1.0') {
    throw "untimed profile JSON must expose a stable build identity, got: $($coldJson.build)"
}
if ($warm.Stdout -notmatch '(?m)^config threads 1 speed 100 timed 0 hash warm\r?$') {
    throw "--warm-hash must mark the normal deterministic text report: $($warm.Stdout)"
}
if ($coldJson.schema -ne 'koi-bench-profile-v1' -or $coldJson.warm_hash -ne $false -or
    $coldJson.hash_state -cne 'cold' -or $coldJson.timed -ne $false -or
    $coldJson.positions.Count -lt 1) {
    throw 'cold profile JSON must identify its schema, cold table state, and positions.'
}
if ($coldJson.evaluator -cne 'classical' -or $null -ne $coldJson.nnue) {
    throw 'the default profile must identify the classical evaluator and carry no NNUE identity.'
}
if ($warmJson.schema -ne 'koi-bench-profile-v1' -or $warmJson.warm_hash -ne $true -or
    $warmJson.hash_state -cne 'warm' -or $warmJson.timed -ne $false -or
    $warmJson.positions.Count -ne $coldJson.positions.Count) {
    throw 'warm profile JSON must identify shared table state and the same suite.'
}
foreach ($position in $coldJson.positions) {
    foreach ($field in @('id', 'fen', 'limits', 'hash_mb', 'hash_state', 'threads', 'speed', 'score_cp', 'pv',
                          'nodes', 'qnodes', 'tt_hits', 'pruning', 'nps')) {
        if ($null -eq $position.$field) {
            throw "profile JSON position is missing $field"
        }
    }
    if ($null -ne $position.elapsed_ms) {
        throw 'untimed profile JSON must not include elapsed wall-clock data.'
    }
    if ($position.hash_state -cne $coldJson.hash_state) {
        throw 'profile JSON position hash state must match its top-level hash state.'
    }
    if ([uint64]$position.nps -ne 0) {
        throw 'untimed profile JSON must mark NPS as unmeasured.'
    }
}
if (-not @($coldJson.positions | Where-Object { $_.pv.Count -gt 1 })) {
    throw 'profile JSON must retain at least one completed multi-move principal variation.'
}
foreach ($position in $timedJson.positions) {
    if ($timedJson.timed -ne $true -or $timedJson.hash_state -cne 'cold' -or
        $position.hash_state -cne $timedJson.hash_state) {
        throw 'timed profile must identify timed=true and keep cold hash state consistent at every position.'
    }
    if ($null -eq $position.elapsed_ms) {
        throw 'timed profile JSON must include elapsed_ms.'
    }
    $visited = [uint64]$position.nodes + [uint64]$position.qnodes
    $expectedNps = if ($position.elapsed_ms -gt 0) {
        [uint64][Math]::Floor(([double]$visited * 1000) / $position.elapsed_ms)
    } else {
        $visited
    }
    if ([uint64]$position.nps -ne $expectedNps) {
        throw "timed profile NPS must use elapsed_ms: expected $expectedNps, got $($position.nps)"
    }
}

$optional = Invoke-Benchmark $BenchPath "--optional --profile-json `"$optionalProfile`"" 'optional-profile'
Assert-BenchmarkOutput $optional
if (-not (Test-Path -LiteralPath $optionalProfile -PathType Leaf)) {
    throw "koi-bench did not write the optional profile JSON: $optionalProfile"
}
$optionalJson = Get-Content -LiteralPath $optionalProfile -Raw | ConvertFrom-Json
if ($optionalJson.suite -cne 'optional_strength' -or $optionalJson.positions.Count -ne 128) {
    throw 'the optional benchmark profile must identify and contain all 128 optional strength positions.'
}
$optionalRows = @($optional.Stdout -split "`r?`n" | Where-Object { $_ -like 'position *' })
if ($optional.Stdout -notmatch '(?m)^config threads 1 speed 100 timed 0 hash cold suite optional_strength\r?$' -or
    $optionalRows.Count -ne 128) {
    throw "the optional benchmark text report must identify its suite and report 128 positions: $($optional.Stdout)"
}
