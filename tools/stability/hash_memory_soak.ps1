[CmdletBinding()]
param(
    [string]$EnginePath = (Join-Path $PSScriptRoot '..\..\build\release\koi-engine.exe'),
    [string]$HashValues = '512,2048,3072,4096',
    [int]$Cycles = 1,
    [string]$OutputDirectory = (Join-Path $PSScriptRoot '..\..\artifacts\stability\hash-memory')
)

$ErrorActionPreference = 'Stop'

$repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$resolvedEnginePath = [System.IO.Path]::GetFullPath($EnginePath)
$resolvedOutputDirectory = [System.IO.Path]::GetFullPath($OutputDirectory)
$parsedHashValues = @($HashValues -split '[,;\s]+' | Where-Object { $_ -ne '' } |
    ForEach-Object { [int]$_ })
$artifactRoot = [System.IO.Path]::GetFullPath((Join-Path $repositoryRoot 'artifacts'))
$artifactPrefix = $artifactRoot.TrimEnd('\') + '\'

if (-not (Test-Path -LiteralPath $resolvedEnginePath -PathType Leaf)) {
    throw "Engine executable was not found: $resolvedEnginePath"
}
if (-not $resolvedOutputDirectory.StartsWith($artifactPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Hash soak output must remain under the repository artifacts directory: $resolvedOutputDirectory"
}

$runDirectory = Join-Path $resolvedOutputDirectory ((Get-Date).ToUniversalTime().ToString('yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $runDirectory -Force | Out-Null

function Invoke-HashProbe {
    param(
        [string]$Path,
        [int]$HashMegabytes,
        [int]$Cycle,
        [string]$Directory
    )

    $stdoutPath = Join-Path $Directory 'stdout.log'
    $stderrPath = Join-Path $Directory 'stderr.log'
    $debugPath = Join-Path $Directory 'koi-debug.jsonl'
    $start = Get-Date
    $processInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $processInfo.FileName = $Path
    $processInfo.UseShellExecute = $false
    $processInfo.CreateNoWindow = $true
    $processInfo.RedirectStandardInput = $true
    $processInfo.RedirectStandardOutput = $true
    $processInfo.RedirectStandardError = $true

    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $processInfo
    if (-not $process.Start()) {
        throw "Unable to start engine for Hash=$HashMegabytes cycle=$Cycle"
    }

    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    $writer = $process.StandardInput
    @(
        'uci'
        "setoption name DebugFile value $debugPath"
        'setoption name Debug value true'
        "setoption name Hash value $HashMegabytes"
        'isready'
        'position startpos'
        'go depth 1'
        'stop'
        'ucinewgame'
        'position startpos'
        'go depth 1'
        'stop'
        'quit'
    ) | ForEach-Object { $writer.WriteLine($_) }
    $writer.Close()

    [Int64]$peakWorkingSet = 0
    [Int64]$peakCommitBytes = 0
    while (-not $process.WaitForExit(100)) {
        $process.Refresh()
        $peakWorkingSet = [Math]::Max($peakWorkingSet, $process.WorkingSet64)
        $peakCommitBytes = [Math]::Max($peakCommitBytes, $process.PrivateMemorySize64)
    }
    $process.Refresh()
    $peakWorkingSet = [Math]::Max($peakWorkingSet, $process.WorkingSet64)
    $peakCommitBytes = [Math]::Max($peakCommitBytes, $process.PrivateMemorySize64)
    $process.WaitForExit()

    $stdout = $stdoutTask.GetAwaiter().GetResult()
    $stderr = $stderrTask.GetAwaiter().GetResult()
    Set-Content -LiteralPath $stdoutPath -Value $stdout -Encoding utf8
    Set-Content -LiteralPath $stderrPath -Value $stderr -Encoding utf8

    $resizeEvent = $null
    if (Test-Path -LiteralPath $debugPath -PathType Leaf) {
        foreach ($line in Get-Content -LiteralPath $debugPath) {
            try {
                $event = $line | ConvertFrom-Json
                if ($event.event -eq 'hash_resize') {
                    $resizeEvent = $event
                }
            } catch {
                # Preserve the raw diagnostic file even if an individual line is incomplete.
            }
        }
    }

    $effectiveHash = if ($null -ne $resizeEvent) {
        [int]$resizeEvent.effective_mb
    } else {
        $HashMegabytes
    }
    $resizeStatus = if ($null -ne $resizeEvent) { [string]$resizeEvent.status } else { 'unobserved' }
    $resizeReason = if ($null -ne $resizeEvent) { [string]$resizeEvent.reason } else { 'unobserved' }
    $bestmoveCount = @($stdout -split "`r?`n" | Where-Object { $_ -like 'bestmove *' }).Count
    [pscustomobject]@{
        hash_requested_mb = $HashMegabytes
        hash_effective_mb = $effectiveHash
        hash_status = $resizeStatus
        hash_reason = $resizeReason
        cycle = $Cycle
        exit_code = $process.ExitCode
        duration_ms = [int]((Get-Date) - $start).TotalMilliseconds
        peak_working_set_bytes = $peakWorkingSet
        peak_commit_bytes = $peakCommitBytes
        bestmove_count = $bestmoveCount
        stdout = $stdoutPath
        stderr = $stderrPath
        diagnostics = $debugPath
    }
}

$records = [System.Collections.Generic.List[object]]::new()
$failures = [System.Collections.Generic.List[string]]::new()
foreach ($hash in $parsedHashValues) {
    if ($hash -lt 1 -or $hash -gt 4096) {
        throw "Hash value is outside the public UCI range: $hash"
    }
    for ($cycle = 1; $cycle -le $Cycles; ++$cycle) {
        $caseDirectory = Join-Path $runDirectory ("hash-{0:D4}-cycle-{1:D2}" -f $hash, $cycle)
        New-Item -ItemType Directory -Path $caseDirectory -Force | Out-Null
        try {
            $record = Invoke-HashProbe -Path $resolvedEnginePath -HashMegabytes $hash -Cycle $cycle -Directory $caseDirectory
            $records.Add($record)
            if ($record.exit_code -ne 0 -or $record.bestmove_count -lt 1) {
                $failures.Add("Hash=$hash cycle=$cycle exit=$($record.exit_code) bestmoves=$($record.bestmove_count)")
            }
        } catch {
            $failures.Add("Hash=$hash cycle=$cycle exception=$($_.Exception.Message)")
        }
    }
}

$manifest = [ordered]@{
    schema = 'koi-hash-memory-soak-v1'
    generated_utc = (Get-Date).ToUniversalTime().ToString('o')
    repository_root = $repositoryRoot
    engine_path = $resolvedEnginePath
    engine_sha256 = (Get-FileHash -LiteralPath $resolvedEnginePath -Algorithm SHA256).Hash
    hash_values_mb = @($parsedHashValues)
    cycles = $Cycles
    records = @($records)
    failures = @($failures)
    status = if ($failures.Count -eq 0) { 'pass' } else { 'fail' }
}
$manifestPath = Join-Path $runDirectory 'manifest.json'
$manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $manifestPath -Encoding utf8
Write-Output ($manifest | ConvertTo-Json -Depth 8)
if ($failures.Count -ne 0) {
    exit 1
}
