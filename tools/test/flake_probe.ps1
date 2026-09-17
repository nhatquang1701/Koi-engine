# Repeats a labelled or name-filtered CTest subset to surface intermittent
# failures. CTest runs the subset once per repeat; the script reports every run
# and exits non-zero when any run fails.
#
# Examples:
#   .\tools\test\flake_probe.ps1 -TestRegex koi_search_tests -CaseFilter "short oracle b2b4 rook lift"
#   .\tools\test\flake_probe.ps1 -Label timing -Repeats 5
#   .\tools\test\flake_probe.ps1 -TestRegex "koi_engine_time_safety_process" -Repeats 3
[CmdletBinding()]
param(
    [string]$BuildDirectory = "build/release",
    [string]$Configuration = "Release",
    [string]$TestRegex = "",
    [string[]]$Label = @(),
    [string]$CaseFilter = "",
    [int]$Repeats = 5,
    [string]$OutputDirectory = "",
    [string]$CTestPath = "ctest",
    [string]$Parallel = "1"
)

$ErrorActionPreference = "Stop"

$ctest = (Get-Command $CTestPath -ErrorAction SilentlyContinue).Source
if (-not $ctest) {
    throw "Required command '$CTestPath' was not found on PATH."
}
if (-not $TestRegex -and $Label.Count -eq 0) {
    throw "Provide -TestRegex and/or -Label to select the subset."
}
if (-not (Test-Path -LiteralPath $BuildDirectory)) {
    throw "Build directory '$BuildDirectory' does not exist."
}

if (-not $OutputDirectory) {
    $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
    $OutputDirectory = Join-Path "artifacts/verification/test-suite-hardening" "flake-probe-$stamp"
}
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null

$previousFilter = $env:KOI_TEST_FILTER
if ($CaseFilter) {
    $env:KOI_TEST_FILTER = $CaseFilter
}

$failures = 0
$results = @()
try {
    for ($run = 1; $run -le $Repeats; ++$run) {
        $arguments = @("--test-dir", $BuildDirectory, "-C", $Configuration, "--output-on-failure", "-j", "$Parallel")
        if ($TestRegex) {
            $arguments += @("-R", $TestRegex)
        }
        foreach ($value in $Label) {
            $arguments += @("-L", $value)
        }
        $logPath = Join-Path $OutputDirectory ("run-{0:D2}.log" -f $run)
        Write-Host ("probe {0}/{1}: {2}" -f $run, $Repeats, ($arguments -join " "))
        & $ctest @arguments 2>&1 | Tee-Object -FilePath $logPath | Out-Host
        $code = $LASTEXITCODE
        $status = if ($code -eq 0) { "pass" } else { "fail" }
        if ($code -ne 0) {
            ++$failures
        }
        $results += [pscustomobject]@{ Run = $run; Status = $status; ExitCode = $code }
    }
} finally {
    if ($CaseFilter) {
        $env:KOI_TEST_FILTER = $previousFilter
    } else {
        Remove-Item Env:\KOI_TEST_FILTER -ErrorAction SilentlyContinue
    }
}

Write-Host ""
Write-Host "flake probe summary ($OutputDirectory):"
$results | Format-Table -AutoSize | Out-String | Write-Host
if ($failures -gt 0) {
    throw "flake probe failed $failures of $Repeats runs."
}
Write-Host "flake probe: $Repeats/$Repeats runs passed."
