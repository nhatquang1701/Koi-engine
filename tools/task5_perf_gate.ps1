[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$BaselineExecutable,
    [Parameter(Mandatory = $true)]
    [string]$CandidateExecutable,
    [string]$OutputDirectory = '',
    [ValidateRange(2, 50)]
    [int]$Runs = 5,
    [ValidateRange(1, 64)]
    [int]$Threads = 2,
    [ValidateRange(1, 100)]
    [int]$Speed = 100
)

$ErrorActionPreference = 'Stop'

$repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path ([System.IO.Path]::GetTempPath()) (
        'koi-task5-perf-gate-' + [guid]::NewGuid().ToString('N'))
}
$verificationRoot = [System.IO.Path]::GetFullPath($OutputDirectory)
$repositoryPrefix = $repositoryRoot.TrimEnd('\') + '\'
if ($verificationRoot -ieq $repositoryRoot -or
    $verificationRoot.StartsWith($repositoryPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Performance-gate output must be outside the repository: $verificationRoot"
}

function Resolve-Executable([string]$Path, [string]$Label) {
    $resolved = [System.IO.Path]::GetFullPath($Path)
    if (-not (Test-Path -LiteralPath $resolved -PathType Leaf)) {
        throw "$Label benchmark executable does not exist: $resolved"
    }
    return $resolved
}

function Write-Log([string]$Path, [object[]]$Lines) {
    if ($Lines.Count -eq 0) {
        Set-Content -LiteralPath $Path -Value '' -Encoding UTF8
    } else {
        Set-Content -LiteralPath $Path -Value ($Lines | ForEach-Object { $_.ToString() }) -Encoding UTF8
    }
}

function Invoke-CapturedProcess([string]$FilePath, [string[]]$Arguments,
                                [string]$StdoutPath, [string]$StderrPath) {
    $stdout = @(& $FilePath @Arguments 2> $StderrPath | ForEach-Object { $_.ToString() })
    $exitCode = $LASTEXITCODE
    Write-Log $StdoutPath $stdout
    if ($exitCode -ne 0) {
        throw "Benchmark process failed with exit code $exitCode. See $StdoutPath and $StderrPath"
    }
    return $stdout
}

function Get-ProfileElapsedMilliseconds([string]$Path, [string]$Label) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Label benchmark did not produce a profile: $Path"
    }
    $profile = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
    if ($profile.schema -cne 'koi-bench-profile-v1' -or
        $profile.timed -ne $true -or $profile.warm_hash -ne $false -or
        $profile.hash_state -cne 'cold' -or $profile.threads -ne $Threads -or
        $profile.speed -ne $Speed -or $profile.positions.Count -ne 64) {
        throw "$Label benchmark profile does not describe the required 64-position cold fixed-depth timed run: $Path"
    }

    [long]$totalMilliseconds = 0
    foreach ($position in $profile.positions) {
        if ($null -eq $position.limits -or $null -eq $position.limits.depth -or
            [int]$position.limits.depth -lt 1 -or $null -eq $position.elapsed_ms) {
            throw "$Label benchmark profile contains a non-fixed-depth or untimed position: $Path"
        }
        [long]$elapsedMilliseconds = $position.elapsed_ms
        if ($elapsedMilliseconds -lt 0) {
            throw "$Label benchmark profile contains a negative elapsed time: $Path"
        }
        $totalMilliseconds += $elapsedMilliseconds
    }
    if ($totalMilliseconds -le 0) {
        throw "$Label benchmark profile measured no elapsed time: $Path"
    }
    return $totalMilliseconds
}

function Get-Median([long[]]$Values) {
    if ($Values.Count -eq 0) {
        throw 'Cannot compute a median without benchmark runs'
    }
    $ordered = @($Values | Sort-Object)
    $middle = [int]($ordered.Count / 2)
    if (($ordered.Count % 2) -eq 1) {
        return [double]$ordered[$middle]
    }
    return ([double]$ordered[$middle - 1] + [double]$ordered[$middle]) / 2.0
}

$baselinePath = Resolve-Executable $BaselineExecutable 'Baseline'
$candidatePath = Resolve-Executable $CandidateExecutable 'Candidate'
New-Item -ItemType Directory -Path $verificationRoot -Force | Out-Null
$baselineDirectory = Join-Path $verificationRoot 'baseline'
$candidateDirectory = Join-Path $verificationRoot 'candidate'
New-Item -ItemType Directory -Path $baselineDirectory -Force | Out-Null
New-Item -ItemType Directory -Path $candidateDirectory -Force | Out-Null

$baselineTimes = [System.Collections.Generic.List[long]]::new()
$candidateTimes = [System.Collections.Generic.List[long]]::new()
$benchmarkArguments = @('--threads', "$Threads", '--speed', "$Speed", '--timed')

Write-Output "output_directory=$verificationRoot"
Write-Output "baseline_executable=$baselinePath"
Write-Output "candidate_executable=$candidatePath"
Write-Output "runs=$Runs threads=$Threads speed=$Speed suite=strength limits=fixed-depth hash=cold"

for ($run = 1; $run -le $Runs; ++$run) {
    $baselineProfile = Join-Path $baselineDirectory ("run-{0:D2}.json" -f $run)
    $baselineStdout = Join-Path $baselineDirectory ("run-{0:D2}.stdout.txt" -f $run)
    $baselineStderr = Join-Path $baselineDirectory ("run-{0:D2}.stderr.txt" -f $run)
    $baselineLines = Invoke-CapturedProcess $baselinePath ($benchmarkArguments + @('--profile-json', $baselineProfile)) `
        $baselineStdout $baselineStderr
    $baselineMilliseconds = Get-ProfileElapsedMilliseconds $baselineProfile "Baseline run $run"
    [void]$baselineTimes.Add($baselineMilliseconds)

    $candidateProfile = Join-Path $candidateDirectory ("run-{0:D2}.json" -f $run)
    $candidateStdout = Join-Path $candidateDirectory ("run-{0:D2}.stdout.txt" -f $run)
    $candidateStderr = Join-Path $candidateDirectory ("run-{0:D2}.stderr.txt" -f $run)
    $candidateLines = Invoke-CapturedProcess $candidatePath ($benchmarkArguments + @('--profile-json', $candidateProfile)) `
        $candidateStdout $candidateStderr
    $candidateMilliseconds = Get-ProfileElapsedMilliseconds $candidateProfile "Candidate run $run"
    [void]$candidateTimes.Add($candidateMilliseconds)

    Write-Output "run=$run baseline_total_ms=$baselineMilliseconds candidate_total_ms=$candidateMilliseconds"
}

$baselineMedian = Get-Median $baselineTimes.ToArray()
$candidateMedian = Get-Median $candidateTimes.ToArray()
$ratio = $candidateMedian / $baselineMedian
$threshold = $baselineMedian * 1.05

Write-Output ("baseline_median_ms={0:N1}" -f $baselineMedian)
Write-Output ("candidate_median_ms={0:N1}" -f $candidateMedian)
Write-Output ("candidate_vs_baseline={0:P2} regression_limit=5.00%" -f ($ratio - 1.0))

if ($candidateMedian -gt $threshold) {
    throw ("Performance gate failed: candidate median {0:N1} ms is more than 5% slower than " +
        "baseline median {1:N1} ms") -f $candidateMedian, $baselineMedian
}

Write-Output 'result=PASS'
