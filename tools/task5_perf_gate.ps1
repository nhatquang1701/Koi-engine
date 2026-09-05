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

if ($null -eq ('KoiTask5CanonicalPath' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32.SafeHandles;

public static class KoiTask5CanonicalPath
{
    private const uint ShareRead = 0x00000001;
    private const uint ShareWrite = 0x00000002;
    private const uint ShareDelete = 0x00000004;
    private const uint OpenExisting = 3;
    private const uint FileFlagBackupSemantics = 0x02000000;

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern SafeFileHandle CreateFile(
        string fileName, uint desiredAccess, uint shareMode, IntPtr securityAttributes,
        uint creationDisposition, uint flagsAndAttributes, IntPtr templateFile);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern uint GetFinalPathNameByHandle(
        SafeFileHandle file, StringBuilder path, uint pathLength, uint flags);

    public static string Resolve(string path)
    {
        using (SafeFileHandle handle = CreateFile(path, 0,
            ShareRead | ShareWrite | ShareDelete, IntPtr.Zero, OpenExisting,
            FileFlagBackupSemantics, IntPtr.Zero))
        {
            if (handle.IsInvalid)
                throw new Win32Exception(Marshal.GetLastWin32Error(),
                    "Unable to open path for canonicalization: " + path);

            for (int capacity = 1024; capacity <= 65536; capacity *= 2)
            {
                StringBuilder buffer = new StringBuilder(capacity);
                uint length = GetFinalPathNameByHandle(handle, buffer, (uint)capacity, 0);
                if (length == 0)
                    throw new Win32Exception(Marshal.GetLastWin32Error(),
                        "Unable to resolve path: " + path);
                if (length < capacity)
                    return StripExtendedPrefix(buffer.ToString());
            }
        }
        throw new PathTooLongException("Unable to canonicalize path: " + path);
    }

    private static string StripExtendedPrefix(string path)
    {
        if (path.StartsWith(@"\\?\UNC\", StringComparison.OrdinalIgnoreCase))
            return @"\\" + path.Substring(8);
        if (path.StartsWith(@"\\?\", StringComparison.OrdinalIgnoreCase))
            return path.Substring(4);
        return path;
    }
}
'@
}

function Resolve-CanonicalPath([string]$Path) {
    $fullPath = [System.IO.Path]::GetFullPath($Path)
    $pathRoot = [System.IO.Path]::GetPathRoot($fullPath)
    if ($fullPath -ne $pathRoot) {
        $fullPath = $fullPath.TrimEnd('\', '/')
    }
    $existingPath = $fullPath
    $missingComponents = [System.Collections.Generic.List[string]]::new()
    while (-not (Test-Path -LiteralPath $existingPath)) {
        $parentPath = [System.IO.Path]::GetDirectoryName($existingPath)
        if ([string]::IsNullOrEmpty($parentPath) -or $parentPath -eq $existingPath) {
            throw "Unable to find an existing parent for path: $Path"
        }
        $missingComponents.Insert(0, [System.IO.Path]::GetFileName($existingPath))
        $existingPath = $parentPath
    }

    $canonicalPath = [KoiTask5CanonicalPath]::Resolve($existingPath)
    foreach ($component in $missingComponents) {
        $canonicalPath = Join-Path $canonicalPath $component
    }
    return [System.IO.Path]::GetFullPath($canonicalPath)
}

$repositoryRoot = Resolve-CanonicalPath (Join-Path $PSScriptRoot '..')
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path ([System.IO.Path]::GetTempPath()) (
        'koi-task5-perf-gate-' + [guid]::NewGuid().ToString('N'))
}
$verificationRoot = Resolve-CanonicalPath $OutputDirectory
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

function Get-BenchmarkRows([object[]]$Lines, [string]$Label) {
    $allLines = @($Lines)
    $expectedHeader = "config threads $Threads speed $Speed timed 1 hash cold"
    if ($allLines.Count -ne 66 -or $allLines[0] -cne 'Koi benchmark' -or
        $allLines[1] -cne $expectedHeader) {
        throw "$Label benchmark stdout does not contain the required 64-position timed report"
    }

    $rowPattern = '^position (?<id>[a-z0-9_-]+) depth (?<depth>[1-9][0-9]*) nodes (?<nodes>[0-9]+) ' +
        'qnodes (?<qnodes>[0-9]+) tt_hits (?<tt_hits>[0-9]+) score (?<score>-?[0-9]+) ' +
        'expected (?<expected>[a-h][1-8][a-h][1-8][nbrq]?) move ' +
        '(?<move>[a-h][1-8][a-h][1-8][nbrq]?|0000) match (?<match>[01]) ' +
        'elapsed_ms (?<elapsed>[0-9]+) nps (?<nps>[0-9]+)$'
    $rows = [System.Collections.Generic.List[object]]::new()
    $seenIds = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::Ordinal)
    for ($index = 2; $index -lt $allLines.Count; ++$index) {
        $line = [string]$allLines[$index]
        if ($line -notmatch $rowPattern) {
            throw "$Label benchmark stdout contains an invalid timed row: $line"
        }
        $id = [string]$Matches['id']
        if (-not $seenIds.Add($id)) {
            throw "$Label benchmark stdout contains a duplicate position row: $id"
        }
        $rows.Add([pscustomobject]@{
            Id = $id
            CompletedDepth = [int]$Matches['depth']
            Nodes = [uint64]$Matches['nodes']
            QNodes = [uint64]$Matches['qnodes']
            TtHits = [uint64]$Matches['tt_hits']
            ScoreCp = [int]$Matches['score']
            ExpectedMove = [string]$Matches['expected']
            Move = [string]$Matches['move']
            Accepted = [int]$Matches['match']
            ElapsedMilliseconds = [long]$Matches['elapsed']
            Nps = [uint64]$Matches['nps']
        })
    }
    if ($rows.Count -ne 64) {
        throw "$Label benchmark stdout contains $($rows.Count) positions; expected 64"
    }
    return @($rows)
}

function Get-BenchmarkReport([object[]]$Lines, [string]$ProfilePath, [string]$Label) {
    $rows = @(Get-BenchmarkRows $Lines $Label)
    if (-not (Test-Path -LiteralPath $ProfilePath -PathType Leaf)) {
        throw "$Label benchmark did not produce a profile: $ProfilePath"
    }
    try {
        $profile = Get-Content -LiteralPath $ProfilePath -Raw | ConvertFrom-Json
    } catch {
        throw "$Label benchmark profile is not valid JSON: $ProfilePath"
    }

    $profilePositions = @($profile.positions)
    if ($profile.schema -cne 'koi-bench-profile-v1' -or $profile.suite -cne 'strength' -or
        $profile.timed -ne $true -or $profile.warm_hash -ne $false -or
        $profile.hash_state -cne 'cold' -or $profile.threads -ne $Threads -or
        $profile.speed -ne $Speed -or $profilePositions.Count -ne 64) {
        throw "$Label benchmark profile does not describe the required 64-position cold timed strength run: $ProfilePath"
    }

    [long]$totalMilliseconds = 0
    $positions = [System.Collections.Generic.List[object]]::new()
    for ($index = 0; $index -lt $profilePositions.Count; ++$index) {
        $position = $profilePositions[$index]
        $row = $rows[$index]
        if ($null -eq $position.id -or [string]$position.id -cne $row.Id) {
            throw "$Label benchmark row/profile position order differs at index $($index + 1): $ProfilePath"
        }
        if ($null -eq $position.hash_state -or $position.hash_state -cne 'cold' -or
            $position.threads -ne $Threads -or $position.speed -ne $Speed) {
            throw "$Label benchmark profile position has inconsistent run configuration: $($row.Id)"
        }
        if ($null -eq $position.limits -or $null -eq $position.limits.depth) {
            throw "$Label benchmark profile position has no requested depth: $($row.Id)"
        }
        [int]$requestedDepth = $position.limits.depth
        if ($requestedDepth -lt 1 -or $row.CompletedDepth -ne $requestedDepth) {
            throw "$Label benchmark position $($row.Id) completed depth $($row.CompletedDepth), requested $requestedDepth"
        }
        if ($null -eq $position.score_cp -or $row.ScoreCp -ne [int]$position.score_cp) {
            throw "$Label benchmark position $($row.Id) stdout/profile score differs"
        }

        $profilePv = @($position.pv)
        $profileMove = if ($profilePv.Count -gt 0 -and $null -ne $profilePv[0]) {
            [string]$profilePv[0]
        } else {
            '0000'
        }
        if ($row.Move -cne $profileMove) {
            throw "$Label benchmark position $($row.Id) stdout/profile move differs"
        }
        if ($row.Accepted -ne 1) {
            throw "$Label benchmark position $($row.Id) does not satisfy the accepted-move contract"
        }
        if ($null -eq $position.elapsed_ms) {
            throw "$Label benchmark profile position has no elapsed time: $($row.Id)"
        }
        [long]$elapsedMilliseconds = $position.elapsed_ms
        if ($elapsedMilliseconds -lt 0 -or $row.ElapsedMilliseconds -ne $elapsedMilliseconds) {
            throw "$Label benchmark position $($row.Id) stdout/profile timing differs"
        }
        $totalMilliseconds += $elapsedMilliseconds
        $positions.Add([pscustomobject]@{
            Id = $row.Id
            RequestedDepth = $requestedDepth
            CompletedDepth = $row.CompletedDepth
            ScoreCp = $row.ScoreCp
            Move = $row.Move
            ElapsedMilliseconds = $elapsedMilliseconds
        })
    }
    if ($totalMilliseconds -le 0) {
        throw "$Label benchmark measured no elapsed time: $ProfilePath"
    }
    return [pscustomobject]@{
        Positions = @($positions)
        TotalMilliseconds = $totalMilliseconds
    }
}

function Assert-BenchmarkParity([object]$BaselineReport, [object]$CandidateReport, [int]$Run) {
    if ($BaselineReport.Positions.Count -ne 64 -or $CandidateReport.Positions.Count -ne 64) {
        throw "Run $Run benchmark parity requires 64 validated positions per executable"
    }
    for ($index = 0; $index -lt 64; ++$index) {
        $baseline = $BaselineReport.Positions[$index]
        $candidate = $CandidateReport.Positions[$index]
        if ($candidate.Id -cne $baseline.Id) {
            throw "Run $Run benchmark parity position mismatch at index $($index + 1)"
        }
        if ($candidate.RequestedDepth -ne $baseline.RequestedDepth -or
            $candidate.CompletedDepth -ne $baseline.CompletedDepth) {
            throw "Run $Run benchmark parity depth mismatch for position $($baseline.Id)"
        }
        if ($candidate.ScoreCp -ne $baseline.ScoreCp) {
            throw "Run $Run benchmark parity score mismatch for position $($baseline.Id)"
        }
        if ($candidate.Move -cne $baseline.Move) {
            throw "Run $Run benchmark parity move mismatch for position $($baseline.Id)"
        }
    }
}

function Invoke-ValidatedBenchmark([string]$Label, [string]$Executable, [string]$ProfilePath,
                                    [string]$StdoutPath, [string]$StderrPath) {
    $lines = @(Invoke-CapturedProcess $Executable ($benchmarkArguments + @('--profile-json', $ProfilePath)) `
        $StdoutPath $StderrPath)
    return Get-BenchmarkReport $lines $ProfilePath $Label
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
    $candidateProfile = Join-Path $candidateDirectory ("run-{0:D2}.json" -f $run)
    $candidateStdout = Join-Path $candidateDirectory ("run-{0:D2}.stdout.txt" -f $run)
    $candidateStderr = Join-Path $candidateDirectory ("run-{0:D2}.stderr.txt" -f $run)

    if (($run % 2) -eq 1) {
        $baselineReport = Invoke-ValidatedBenchmark "Baseline run $run" $baselinePath $baselineProfile `
            $baselineStdout $baselineStderr
        $candidateReport = Invoke-ValidatedBenchmark "Candidate run $run" $candidatePath $candidateProfile `
            $candidateStdout $candidateStderr
        $firstLabel = 'baseline'
        $secondLabel = 'candidate'
    } else {
        $candidateReport = Invoke-ValidatedBenchmark "Candidate run $run" $candidatePath $candidateProfile `
            $candidateStdout $candidateStderr
        $baselineReport = Invoke-ValidatedBenchmark "Baseline run $run" $baselinePath $baselineProfile `
            $baselineStdout $baselineStderr
        $firstLabel = 'candidate'
        $secondLabel = 'baseline'
    }

    Assert-BenchmarkParity $baselineReport $candidateReport $run
    [void]$baselineTimes.Add([long]$baselineReport.TotalMilliseconds)
    [void]$candidateTimes.Add([long]$candidateReport.TotalMilliseconds)

    Write-Output "run=$run baseline_total_ms=$($baselineReport.TotalMilliseconds) candidate_total_ms=$($candidateReport.TotalMilliseconds) order=$firstLabel,$secondLabel"
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
