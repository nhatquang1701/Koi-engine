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
    [int]$Threads = 1,
    [ValidateRange(1, 100)]
    [int]$Speed = 100,
    [string]$FenFile = '',
    [ValidateRange(0, 255)]
    [int]$Depth = 0,
    [string]$DepthSweep = '',
    [ValidateRange(0.0, 50.0)]
    [double]$MaxNpsRegressionPercent = 2.0,
    [ValidateRange(0.0, 50.0)]
    [double]$MaxRowNpsRegressionPercent = 2.0,
    # Rows whose baseline median elapsed is below this are timer-quantized: a
    # 3-4 ms row moves in ~25% steps, and the Phase 0 noise floor recorded up
    # to 99.9% swings on 0-1 ms rows.  The 20 ms default keeps T8 rows (where
    # one 1 ms tick is still >5%) out of the per-row verdict too.  Such rows
    # are reported (with noise=true) but excluded from the row regression test.
    [ValidateRange(0.0, 1000.0)]
    [double]$MinRowElapsedMs = 20.0
)

$ErrorActionPreference = 'Stop'

# The speed gate consumes the koi-bench-speed-v1 report written by `koi-bench
# --report`. Both executables must therefore come from a build that includes the
# Phase 0 measurement upgrade. It is intentionally a standalone measurement
# command, never a CTest threshold.
$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$verificationRoot = Join-Path $repoRoot 'artifacts\verification'

function Resolve-RepoPath([string]$Path, [string]$Label) {
    $resolved = if ([System.IO.Path]::IsPathRooted($Path)) {
        [System.IO.Path]::GetFullPath($Path)
    } else {
        [System.IO.Path]::GetFullPath((Join-Path $repoRoot $Path))
    }
    if (-not (Test-Path -LiteralPath $resolved)) {
        throw "$Label does not exist: $resolved"
    }
    return $resolved
}

function Get-Median([double[]]$Values) {
    if ($Values.Count -eq 0) {
        throw 'Cannot compute a median without samples'
    }
    $ordered = @($Values | Sort-Object)
    # Casting a half-integer to [int] rounds (banker's), it does not truncate;
    # floor explicitly so odd sample counts pick the true middle index.
    $middle = [int][Math]::Floor($ordered.Count / 2.0)
    if (($ordered.Count % 2) -eq 1) {
        return [double]$ordered[$middle]
    }
    return ([double]$ordered[$middle - 1] + [double]$ordered[$middle]) / 2.0
}

function Get-VisitedNps($Totals) {
    $visited = [double]([uint64]$Totals.nodes + [uint64]$Totals.qnodes)
    $elapsed = [uint64]$Totals.elapsed_ms
    if ($elapsed -eq 0) {
        return $visited
    }
    return [Math]::Floor($visited * 1000.0 / $elapsed)
}

function Invoke-BenchRun([string]$Label, [string]$Executable, [string]$ReportPath,
                         [string]$StdoutPath, [string]$StderrPath) {
    $arguments = @('--threads', "$Threads", '--speed', "$Speed", '--timed',
        '--report', $ReportPath)
    if ($FenFile -ne '') {
        $arguments += @('--fen-file', $resolvedFenFile)
    }
    if ($DepthSweep -ne '') {
        $arguments += @('--depth-sweep', $DepthSweep)
    } elseif ($Depth -gt 0) {
        $arguments += @('--depth', "$Depth")
    }

    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $Executable
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.Arguments = ($arguments | ForEach-Object {
        if ($_ -match '[\s"]') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ }
    }) -join ' '
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true

    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    if (-not $process.Start()) {
        throw "Unable to start $Label"
    }
    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit(1800000)) {
        $process.Kill()
        $process.WaitForExit()
        throw "$Label did not exit within 1800000 ms"
    }
    $stdout = $stdoutTask.GetAwaiter().GetResult()
    $stderr = $stderrTask.GetAwaiter().GetResult()
    [System.IO.File]::WriteAllText($StdoutPath, $stdout)
    [System.IO.File]::WriteAllText($StderrPath, $stderr)
    if ($process.ExitCode -ne 0) {
        throw "$Label exited with $($process.ExitCode): $stderr"
    }
    if (-not (Test-Path -LiteralPath $ReportPath -PathType Leaf)) {
        throw "$Label did not write its speed report: $ReportPath"
    }
    return Get-Content -LiteralPath $ReportPath -Raw | ConvertFrom-Json
}

function Assert-ReportParity($BaselineReport, $CandidateReport, [int]$Run) {
    foreach ($field in @('schema', 'suite', 'source', 'evaluator', 'threads', 'speed',
                          'hash_state', 'timed', 'warmup', 'repeat', 'node_limit')) {
        if ("$($BaselineReport.$field)" -cne "$($CandidateReport.$field)") {
            throw "speed gate run $Run mismatched ${field}: baseline=$($BaselineReport.$field) candidate=$($CandidateReport.$field)"
        }
    }
    if ("$($BaselineReport.depth)" -cne "$($CandidateReport.depth)" -or
        "$($BaselineReport.depth_sweep)" -cne "$($CandidateReport.depth_sweep)") {
        throw "speed gate run $Run mismatched depth configuration"
    }
    $baselineKeys = @($BaselineReport.positions | ForEach-Object { "$($_.id)#$($_.depth)#$($_.fen)" })
    $candidateKeys = @($CandidateReport.positions | ForEach-Object { "$($_.id)#$($_.depth)#$($_.fen)" })
    if (($baselineKeys -join '|') -cne ($candidateKeys -join '|')) {
        throw "speed gate run $Run covered different rows: baseline=$($baselineKeys.Count) candidate=$($candidateKeys.Count)"
    }
}

if ($Depth -gt 0 -and $DepthSweep -ne '') {
    throw 'Specify either -Depth or -DepthSweep, not both.'
}
if ($DepthSweep -ne '' -and $DepthSweep -notmatch '^[0-9]{1,3}\.\.[0-9]{1,3}$') {
    throw "DepthSweep must look like '2..7', got: $DepthSweep"
}
if ($FenFile -ne '') {
    $resolvedFenFile = Resolve-RepoPath $FenFile 'FEN corpus'
}

$baselinePath = Resolve-RepoPath $BaselineExecutable 'Baseline executable'
$candidatePath = Resolve-RepoPath $CandidateExecutable 'Candidate executable'

if ($null -eq $DepthSweep -or $DepthSweep -eq '') {
    $depthDescription = if ($Depth -gt 0) { "depth=$Depth" } else { 'depth=koi-bench-default' }
} else {
    $depthDescription = "depth-sweep=$DepthSweep"
}
if ($FenFile -ne '') {
    $corpusDescription = "fen-file=$resolvedFenFile"
} else {
    $corpusDescription = 'suite=strength'
}

if ($OutputDirectory -eq '') {
    $OutputDirectory = Join-Path $verificationRoot ("speed-program\speed-gate\" + (Get-Date -Format 'yyyyMMdd-HHmmss'))
} elseif (-not [System.IO.Path]::IsPathRooted($OutputDirectory)) {
    $OutputDirectory = Join-Path $repoRoot $OutputDirectory
}
$verificationRootFull = [System.IO.Path]::GetFullPath($verificationRoot).TrimEnd('\', '/')
$outputFull = [System.IO.Path]::GetFullPath($OutputDirectory).TrimEnd('\', '/')
if (-not $outputFull.StartsWith($verificationRootFull + '\', [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "OutputDirectory must live under $verificationRootFull, got: $outputFull"
}
$baselineDirectory = Join-Path $outputFull 'baseline'
$candidateDirectory = Join-Path $outputFull 'candidate'
New-Item -ItemType Directory -Path $baselineDirectory -Force | Out-Null
New-Item -ItemType Directory -Path $candidateDirectory -Force | Out-Null

Write-Output "output_directory=$outputFull"
Write-Output "baseline_executable=$baselinePath"
Write-Output "candidate_executable=$candidatePath"
Write-Output "runs=$Runs threads=$Threads speed=$Speed timed=1 $corpusDescription $depthDescription"
Write-Output ("nps_regression_limit={0:N2}% row_nps_regression_limit={1:N2}% min_row_elapsed_ms={2:N1}" -f $MaxNpsRegressionPercent, $MaxRowNpsRegressionPercent, $MinRowElapsedMs)

# Alternating order spreads thermal and background-load drift across both
# executables; per-row and total medians absorb the remaining noise.
$baselineReports = [System.Collections.Generic.List[object]]::new()
$candidateReports = [System.Collections.Generic.List[object]]::new()
for ($run = 1; $run -le $Runs; ++$run) {
    $baselineReportPath = Join-Path $baselineDirectory ("run-{0:D2}.json" -f $run)
    $candidateReportPath = Join-Path $candidateDirectory ("run-{0:D2}.json" -f $run)
    $baselineStdout = Join-Path $baselineDirectory ("run-{0:D2}.stdout.txt" -f $run)
    $baselineStderr = Join-Path $baselineDirectory ("run-{0:D2}.stderr.txt" -f $run)
    $candidateStdout = Join-Path $candidateDirectory ("run-{0:D2}.stdout.txt" -f $run)
    $candidateStderr = Join-Path $candidateDirectory ("run-{0:D2}.stderr.txt" -f $run)

    if (($run % 2) -eq 1) {
        $baselineReport = Invoke-BenchRun "Baseline run $run" $baselinePath $baselineReportPath $baselineStdout $baselineStderr
        $candidateReport = Invoke-BenchRun "Candidate run $run" $candidatePath $candidateReportPath $candidateStdout $candidateStderr
        $order = 'baseline,candidate'
    } else {
        $candidateReport = Invoke-BenchRun "Candidate run $run" $candidatePath $candidateReportPath $candidateStdout $candidateStderr
        $baselineReport = Invoke-BenchRun "Baseline run $run" $baselinePath $baselineReportPath $baselineStdout $baselineStderr
        $order = 'candidate,baseline'
    }

    Assert-ReportParity $baselineReport $candidateReport $run
    [void]$baselineReports.Add($baselineReport)
    [void]$candidateReports.Add($candidateReport)
    Write-Output ("run={0} baseline_nps={1:N0} candidate_nps={2:N0} order={3}" -f `
        $run, (Get-VisitedNps $baselineReport.totals), (Get-VisitedNps $candidateReport.totals), $order)
}

# Row identity comes from the first validated baseline run; parity guarantees the
# candidate covers exactly the same (position, depth, FEN) set.
$rowKeys = @($baselineReports[0].positions | ForEach-Object { "$($_.id)#$($_.depth)" })
$baselineRows = @{}
$candidateRows = @{}
foreach ($key in $rowKeys) {
    $baselineRows[$key] = [System.Collections.Generic.List[double]]::new()
    $candidateRows[$key] = [System.Collections.Generic.List[double]]::new()
}
$baselineElapsed = @{}
$candidateElapsed = @{}
foreach ($key in $rowKeys) {
    $baselineElapsed[$key] = [System.Collections.Generic.List[double]]::new()
    $candidateElapsed[$key] = [System.Collections.Generic.List[double]]::new()
}

foreach ($report in $baselineReports) {
    foreach ($position in $report.positions) {
        $key = "$($position.id)#$($position.depth)"
        [void]$baselineRows[$key].Add([double]$position.nps)
        [void]$baselineElapsed[$key].Add([double]$position.elapsed_ms)
    }
}
foreach ($report in $candidateReports) {
    foreach ($position in $report.positions) {
        $key = "$($position.id)#$($position.depth)"
        [void]$candidateRows[$key].Add([double]$position.nps)
        [void]$candidateElapsed[$key].Add([double]$position.elapsed_ms)
    }
}

$deltas = [System.Collections.Generic.List[double]]::new()
$excludedNoiseRows = 0
$rows = [System.Collections.Generic.List[object]]::new()
foreach ($key in $rowKeys) {
    $baselineNps = Get-Median -Values $baselineRows[$key].ToArray()
    $candidateNps = Get-Median -Values $candidateRows[$key].ToArray()
    $baselineMs = Get-Median -Values $baselineElapsed[$key].ToArray()
    $candidateMs = Get-Median -Values $candidateElapsed[$key].ToArray()
    $deltaFraction = if ($baselineNps -gt 0.0) { ($candidateNps - $baselineNps) / $baselineNps } else { 0.0 }
    $isNoise = $baselineMs -lt $MinRowElapsedMs
    if ($isNoise) {
        $excludedNoiseRows++
    } else {
        [void]$deltas.Add($deltaFraction * 100.0)
    }
    [void]$rows.Add([pscustomobject]@{
        key = $key
        baseline_nps = [uint64]$baselineNps
        candidate_nps = [uint64]$candidateNps
        nps_delta_percent = [Math]::Round($deltaFraction * 100.0, 4)
        baseline_elapsed_ms = [uint64]$baselineMs
        candidate_elapsed_ms = [uint64]$candidateMs
        noise = $isNoise
    })
}

$baselineTotalNps = Get-Median -Values @($baselineReports | ForEach-Object { Get-VisitedNps $_.totals })
$candidateTotalNps = Get-Median -Values @($candidateReports | ForEach-Object { Get-VisitedNps $_.totals })
$totalDelta = if ($baselineTotalNps -gt 0.0) { ($candidateTotalNps - $baselineTotalNps) / $baselineTotalNps } else { 0.0 }
$medianRowDelta = if ($deltas.Count -eq 0) { 0.0 } else { Get-Median -Values $deltas.ToArray() }

Write-Output ("baseline_total_nps={0:N0} candidate_total_nps={1:N0} delta={2:P2}" -f `
    $baselineTotalNps, $candidateTotalNps, $totalDelta)
Write-Output ("median_row_nps_delta={0:P2} excluded_noise_rows={1}" -f ($medianRowDelta / 100.0), $excludedNoiseRows)
foreach ($row in $rows) {
    Write-Output ("row={0} baseline_nps={1} candidate_nps={2} nps_delta={3:N2}% baseline_ms={4} candidate_ms={5}" -f `
        $row.key, $row.baseline_nps, $row.candidate_nps, $row.nps_delta_percent,
        $row.baseline_elapsed_ms, $row.candidate_elapsed_ms)
}

$summary = [pscustomobject]@{
    schema = 'koi-speed-gate-v1'
    timestamp = (Get-Date).ToString('o')
    baseline_executable = $baselinePath
    candidate_executable = $candidatePath
    runs = $Runs
    threads = $Threads
    speed = $Speed
    fen_file = if ($FenFile -ne '') { $resolvedFenFile } else { $null }
    depth = if ($Depth -gt 0) { $Depth } else { $null }
    depth_sweep = if ($DepthSweep -ne '') { $DepthSweep } else { $null }
    nps_regression_limit_percent = $MaxNpsRegressionPercent
    row_nps_regression_limit_percent = $MaxRowNpsRegressionPercent
    min_row_elapsed_ms = $MinRowElapsedMs
    excluded_noise_rows = $excludedNoiseRows
    baseline_total_nps = [uint64]$baselineTotalNps
    candidate_total_nps = [uint64]$candidateTotalNps
    total_nps_delta_percent = [Math]::Round($totalDelta * 100.0, 4)
    median_row_nps_delta_percent = [Math]::Round($medianRowDelta, 4)
    rows = $rows
}
$summaryPath = Join-Path $outputFull 'speed-gate.json'
$summary | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $summaryPath

$failures = [System.Collections.Generic.List[string]]::new()
if ($totalDelta * 100.0 -lt (-1.0 * $MaxNpsRegressionPercent)) {
    [void]$failures.Add(("total NPS regressed {0:P2} (limit -{1:N2}%)" -f $totalDelta, $MaxNpsRegressionPercent))
}
if ($medianRowDelta -lt (-1.0 * $MaxRowNpsRegressionPercent)) {
    [void]$failures.Add(("median row NPS regressed {0:P2} (limit -{1:N2}%)" -f ($medianRowDelta / 100.0), $MaxRowNpsRegressionPercent))
}
if ($failures.Count -ne 0) {
    throw ("Speed gate failed: " + ($failures.ToArray() -join '; ') + ". Summary: $summaryPath")
}

Write-Output "summary=$summaryPath"
Write-Output 'result=PASS'
