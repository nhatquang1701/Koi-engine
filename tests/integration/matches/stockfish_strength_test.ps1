param(
    [Parameter(Mandatory = $true)]
    [string]$EnginePath
)

$ErrorActionPreference = 'Stop'

# Invoke the match harness with the current PowerShell host instead of a
# hardcoded powershell.exe so the test works on hosts that only ship pwsh.
$powerShellExecutable = (Get-Process -Id $PID).Path

$repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..'))
$matchScript = Join-Path $repositoryRoot 'tools\stability\uci_match.ps1'
$enginePath = (Resolve-Path -LiteralPath $EnginePath).Path
$fixturePath = Join-Path (Split-Path -Parent $enginePath) 'uci_match_fixture.exe'
$replayPath = Join-Path (Split-Path -Parent $enginePath) 'koi-replay.exe'
$defaultOutputDirectory = Join-Path ([System.IO.Path]::GetTempPath()) ('koi-task2-default-strength-' + [guid]::NewGuid().ToString('N'))
$outputDirectory = Join-Path ([System.IO.Path]::GetTempPath()) ('koi-task2-strength-' + [guid]::NewGuid().ToString('N'))

try {
    if (-not (Test-Path -LiteralPath $fixturePath -PathType Leaf)) {
        throw "UCI fixture executable is missing: $fixturePath"
    }
    if (-not (Test-Path -LiteralPath $replayPath -PathType Leaf)) {
        throw "replay executable is missing: $replayPath"
    }

    $defaultOutput = & $powerShellExecutable -NoProfile -ExecutionPolicy Bypass -File $matchScript `
        -KoiPath $enginePath -OpponentPath $fixturePath -ReplayPath $replayPath `
        -Depth 1 -Games 1 -MaxPlies 2 -KoiOwnBook false `
        -OutputDirectory $defaultOutputDirectory
    if ($LASTEXITCODE -ne 0) {
        throw "default match exited with ${LASTEXITCODE}: $($defaultOutput -join ' | ')"
    }
    $defaultJson = @(Get-ChildItem -LiteralPath $defaultOutputDirectory -Filter '*.json' -File)
    $defaultReport = Get-Content -LiteralPath $defaultJson[0].FullName -Raw | ConvertFrom-Json
    $defaultOpponent = @($defaultReport.engines | Where-Object { $_.label -eq 'Opponent' })[0]
    if (@($defaultOpponent.options | Where-Object { $_ -match '^setoption name UCI_(LimitStrength|Elo) value ' }).Count -ne 0) {
        throw 'omitted opponent strength must not send Stockfish strength options.'
    }

    $output = & $powerShellExecutable -NoProfile -ExecutionPolicy Bypass -File $matchScript `
        -KoiPath $enginePath -OpponentPath $fixturePath -ReplayPath $replayPath `
        -Depth 1 -Games 1 -MaxPlies 2 -KoiOwnBook false -OpponentElo 0 `
        -OutputDirectory $outputDirectory
    if ($LASTEXITCODE -ne 0) {
        throw "strength match exited with ${LASTEXITCODE}: $($output -join ' | ')"
    }

    $jsonFiles = @(Get-ChildItem -LiteralPath $outputDirectory -Filter '*.json' -File)
    if ($jsonFiles.Count -ne 1) {
        throw 'strength match must create one JSON report.'
    }
    $report = Get-Content -LiteralPath $jsonFiles[0].FullName -Raw | ConvertFrom-Json
    $opponent = @($report.engines | Where-Object { $_.label -eq 'Opponent' })[0]
    $koi = @($report.engines | Where-Object { $_.label -eq 'Koi' })[0]
    if ($opponent.options -notcontains 'setoption name UCI_LimitStrength value true' -or
        $opponent.options -notcontains 'setoption name UCI_Elo value 1320') {
        throw 'explicit zero opponent strength must enable UCI_LimitStrength and clamp UCI_Elo to the lower bound.'
    }
    if (@($koi.options | Where-Object { $_ -match '^setoption name UCI_(LimitStrength|Elo) value ' }).Count -ne 0) {
        throw 'opponent strength options must not be sent to Koi.'
    }

    Write-Output 'PASS stockfish strength option integration'
}
finally {
    if (Test-Path -LiteralPath $defaultOutputDirectory) {
        Remove-Item -LiteralPath $defaultOutputDirectory -Recurse -Force
    }
    if (Test-Path -LiteralPath $outputDirectory) {
        Remove-Item -LiteralPath $outputDirectory -Recurse -Force
    }
}
