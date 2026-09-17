# Requires the repository's MSVC x64 environment. Either run this script from a
# Visual Studio developer shell, or pass -EnvironmentScript pointing at a .cmd
# wrapper that calls vcvars64.bat and forwards its arguments (see the developer
# notes in tools/README.md). The wrapper avoids re-quoting cl/cmake invocations.
#
# Examples:
#   .\tools\test\run_tests.ps1                                  # full Release suite, parallel
#   .\tools\test\run_tests.ps1 -Label unit                      # fast unit subset
#   .\tools\test\run_tests.ps1 -Filter koi_search_tests -NoBuild
#   .\tools\test\run_tests.ps1 -RepeatUntilPass 1               # disable transient retries
[CmdletBinding()]
param(
    [string]$BuildDirectory = "build/release",
    [string]$Configuration = "Release",
    [string[]]$Label = @(),
    [string[]]$ExcludeLabel = @(),
    [string]$Filter = "",
    [int]$Parallel = 0,
    # CTest retries a failed test until it passes this many attempts.  One
    # retry guards against transient OS/runtime crashes in the process tests
    # (for example the observed pwsh 7 TaskbarJumpList internal CLR error)
    # while a genuine assertion failure still fails every attempt.  Set 1 to
    # disable retries.
    [int]$RepeatUntilPass = 2,
    [string]$OutputDirectory = "",
    [switch]$NoBuild,
    [switch]$NoJUnit,
    [string]$CMakePath = "cmake",
    [string]$CTestPath = "ctest",
    [string]$EnvironmentScript = $env:KOI_MSVC_ENV
)

$ErrorActionPreference = "Stop"

function Resolve-CommandPath {
    param([string]$Name)
    $command = Get-Command $Name -ErrorAction SilentlyContinue
    if (-not $command) {
        throw "Required command '$Name' was not found on PATH."
    }
    return $command.Source
}

$cmake = Resolve-CommandPath $CMakePath
$ctest = Resolve-CommandPath $CTestPath

if ($Parallel -le 0) {
    $Parallel = [Math]::Max(1, [Environment]::ProcessorCount)
}

function Invoke-WithEnvironment {
    param([scriptblock]$Action)
    $needsWrapper = -not (Get-Command cl.exe -ErrorAction SilentlyContinue)
    if (-not $needsWrapper) {
        & $Action
        return
    }
    if (-not $EnvironmentScript) {
        throw "cl.exe is not on PATH and no -EnvironmentScript was provided. Run from a Visual Studio developer shell or pass a wrapper .cmd that calls vcvars64.bat."
    }
    if (-not (Test-Path -LiteralPath $EnvironmentScript)) {
        throw "Environment wrapper '$EnvironmentScript' does not exist."
    }
    $env:KOI_TEST_WRAPPER = $EnvironmentScript
    try {
        & $Action
    } finally {
        Remove-Item Env:\KOI_TEST_WRAPPER -ErrorAction SilentlyContinue
    }
}

if (-not (Test-Path -LiteralPath $BuildDirectory)) {
    throw "Build directory '$BuildDirectory' does not exist. Configure the tree first (see tests/README.md)."
}

if (-not $NoBuild) {
    Write-Host "build: $BuildDirectory ($Configuration)"
    if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
        & $cmake --build $BuildDirectory --config $Configuration
    } else {
        if (-not $EnvironmentScript) {
            throw "cl.exe is not on PATH and no -EnvironmentScript was provided."
        }
        & $EnvironmentScript $cmake --build $BuildDirectory --config $Configuration
    }
    if ($LASTEXITCODE -ne 0) {
        throw "Build failed with exit code $LASTEXITCODE."
    }
}

if (-not $OutputDirectory) {
    $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
    $OutputDirectory = Join-Path "artifacts/verification/test-suite-hardening" "run-$stamp"
}
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null

$arguments = @("--test-dir", $BuildDirectory, "-C", $Configuration, "--output-on-failure", "-j", "$Parallel")
if ($RepeatUntilPass -gt 1) {
    $arguments += @("--repeat", "until-pass:$RepeatUntilPass")
}
if ($Filter) {
    $arguments += @("-R", $Filter)
}
foreach ($value in $Label) {
    $arguments += @("-L", $value)
}
foreach ($value in $ExcludeLabel) {
    $arguments += @("-LE", $value)
}
$junitPath = Join-Path $OutputDirectory "ctest-junit.xml"
if (-not $NoJUnit) {
    $arguments += @("--output-junit", $junitPath)
}

Write-Host "ctest: $ctest $($arguments -join ' ')"
& $ctest @arguments
$exitCode = $LASTEXITCODE

$lastTestLog = Join-Path $BuildDirectory "Testing/Temporary/LastTest.log"
if (Test-Path -LiteralPath $lastTestLog) {
    Copy-Item -LiteralPath $lastTestLog -Destination (Join-Path $OutputDirectory "LastTest.log") -Force
}

Write-Host "results: $OutputDirectory"
if ($exitCode -ne 0) {
    throw "CTest failed with exit code $exitCode."
}
