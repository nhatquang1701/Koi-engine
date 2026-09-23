param(
    [Parameter(Mandatory = $true)]
    [string]$EnginePath
)

$ErrorActionPreference = 'Stop'

$repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
$matchScript = Join-Path $repositoryRoot 'tools/stability/uci_match.ps1'
$binarySuffix = if ($env:OS -eq 'Windows_NT') { '.exe' } else { '' }
$enginePath = (Resolve-Path -LiteralPath $EnginePath).Path
$fixturePath = Join-Path (Split-Path -Parent $enginePath) "uci_match_fixture$binarySuffix"
$replayPath = Join-Path (Split-Path -Parent $enginePath) "koi-replay$binarySuffix"
$outputDirectory = Join-Path ([System.IO.Path]::GetTempPath()) ('koi-uci-match-clock-' + [guid]::NewGuid().ToString('N'))

Import-Module ([System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../support/MatchSupport.psm1'))) -Force

$PowerShellExecutable = Get-PowerShellExecutable

function Invoke-ClockMatch([string]$KoiPath, [string]$OpponentPath, [string]$Directory, [int]$Games) {
    $output = & $PowerShellExecutable -NoProfile -ExecutionPolicy Bypass -File $matchScript `
        -KoiPath $KoiPath -OpponentPath $OpponentPath -ReplayPath $replayPath `
        -KoiColor white -TimeControl 1+0 -Games $Games -MaxPlies 1 `
        -KoiOwnBook false -TimeoutMilliseconds 1000 -OutputDirectory $Directory
    if ($LASTEXITCODE -ne 0) {
        throw "clock match exited with ${LASTEXITCODE}: $($output -join ' | ')"
    }
    $jsonFiles = @(Get-ChildItem -LiteralPath $Directory -Filter '*.json' -File)
    if ($jsonFiles.Count -ne 1) {
        throw 'clock match must create one JSON artifact.'
    }
    return Get-Content -LiteralPath $jsonFiles[0].FullName -Raw | ConvertFrom-Json
}

try {
    if (-not (Test-Path -LiteralPath $fixturePath -PathType Leaf)) {
        throw "UCI fixture executable is missing: $fixturePath"
    }
    if (-not (Test-Path -LiteralPath $replayPath -PathType Leaf)) {
        throw "replay executable is missing: $replayPath"
    }

    New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
    $beforeKoi = New-ScriptedUciEngine -FixturePath $fixturePath -Directory $outputDirectory 'clock-before-deadline-koi'
    $beforeOpponent = New-ScriptedUciEngine -FixturePath $fixturePath -Directory $outputDirectory 'clock-before-deadline-opponent'
    $beforeReport = Invoke-ClockMatch $beforeKoi.path $beforeOpponent.path $outputDirectory 1
    $beforeGame = $beforeReport.games[0]
    if ($beforeReport.games.Count -ne 1 -or $beforeGame.termination -ne 'max plies' -or
        $beforeGame.process_status.koi -ne 'clean shutdown' -or
        $beforeGame.moves.Count -ne 1 -or $beforeGame.moves[0].elapsed_ms -lt 5000 -or
        $beforeGame.moves[0].elapsed_ms -ge 60000) {
        throw 'A 6-second bestmove must be accepted under the 1+0 chess-clock deadline despite a 1-second protocol timeout.'
    }

    $afterDirectory = Join-Path $outputDirectory 'after-deadline'
    New-Item -ItemType Directory -Path $afterDirectory -Force | Out-Null
    $afterKoi = New-ScriptedUciEngine -FixturePath $fixturePath -Directory $afterDirectory 'clock-after-deadline-koi'
    $afterOpponent = New-ScriptedUciEngine -FixturePath $fixturePath -Directory $afterDirectory 'clock-after-deadline-opponent'
    $afterReport = Invoke-ClockMatch $afterKoi.path $afterOpponent.path $afterDirectory 2
    $afterGame = $afterReport.games[0]
    if ($afterReport.games.Count -ne 1 -or $afterGame.result -ne '*' -or
        $afterGame.termination -ne 'timeout' -or $afterGame.process_status.koi -ne 'timeout' -or
        $afterGame.moves.Count -ne 0) {
        throw 'A bestmove after the 1+0 chess-clock deadline must be adjudicated as timeout and not accepted as a legal game move.'
    }
    $newGameCommands = @(Get-Content -LiteralPath $afterKoi.log | Where-Object { $_ -ceq 'ucinewgame' })
    if ($newGameCommands.Count -ne 1) {
        throw 'A chess-clock timeout must safely retire the engine before a later game can start.'
    }

    Write-Output 'PASS UCI match clock deadline integration'
}
finally {
    if (Test-Path -LiteralPath $outputDirectory) {
        Remove-Item -LiteralPath $outputDirectory -Recurse -Force
    }
}
