<#
.SYNOPSIS
Node-limited A/B match: a Koi NNUE network against the classical evaluator.

.DESCRIPTION
Runs two node-limited matches through tools/stability/uci_match.ps1: the Koi
engine loaded with `EvalFile` (NNUE side) against the same engine without it
(classical side), splitting the games so NNUE plays white and black.

The aggregated report is written to JSON (schema koi-nnue-studio-ab-match-v1)
so tools/nnue/studio_core.py can turn it into a studio validation record.

.PARAMETER NnueNet
Path to the `.nnue` file used by the NNUE side.

.PARAMETER EnginePath
Koi engine executable (default: build/release/koi-engine.exe).

.PARAMETER Games
Total games; the odd game goes to the NNUE-white half.

.PARAMETER Nodes
Per-move node limit for both engines (default 20000).

.PARAMETER ReportPath
Aggregated JSON path (default: inside OutputDirectory).

.EXAMPLE
pwsh -NoProfile -File tools/nnue/ab_match.ps1 -NnueNet artifacts/training/koi-sf-v1.nnue -Games 20
#>

param(
    [Parameter(Mandatory = $true)]
    [string]$NnueNet,

    [string]$EnginePath,

    [ValidateRange(2, 2000)]
    [int]$Games = 20,

    [uint64]$Nodes = 20000,

    [ValidateRange(1, 64)]
    [int]$Threads = 1,

    [ValidateRange(1, 4096)]
    [int]$Hash = 64,

    [ValidateRange(1000, 120000)]
    [int]$TimeoutMilliseconds = 20000,

    [string]$OutputDirectory,

    [string]$ReportPath,

    [string]$UciMatchPath
)

$ErrorActionPreference = 'Stop'
$repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
if ([string]::IsNullOrWhiteSpace($EnginePath)) {
    $EnginePath = Join-Path $repositoryRoot 'build\release\koi-engine.exe'
}
if ([string]::IsNullOrWhiteSpace($UciMatchPath)) {
    $UciMatchPath = Join-Path $repositoryRoot 'tools\stability\uci_match.ps1'
}
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $repositoryRoot ('artifacts\matches\nnue-ab-' +
        (Get-Date).ToUniversalTime().ToString('yyyyMMdd-HHmmss-fff'))
}
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null

$nnuePath = (Resolve-Path -LiteralPath $NnueNet).Path
$engine = (Resolve-Path -LiteralPath $EnginePath).Path
$replay = Join-Path (Split-Path -Parent $engine) 'koi-replay.exe'
if (-not (Test-Path -LiteralPath $replay -PathType Leaf)) {
    throw "koi-replay executable is missing beside the engine: $replay"
}
if (-not (Test-Path -LiteralPath $UciMatchPath -PathType Leaf)) {
    throw "uci_match.ps1 is missing: $UciMatchPath"
}
if ([string]::IsNullOrWhiteSpace($ReportPath)) {
    $ReportPath = Join-Path $OutputDirectory 'ab-match.json'
}

$nnueWhiteGames = [int][math]::Ceiling($Games / 2.0)
$nnueBlackGames = $Games - $nnueWhiteGames

function Invoke-AbLeg {
    param(
        [string]$Tag,
        [string]$KoiColor,
        [int]$LegGames
    )

    $legDirectory = Join-Path $OutputDirectory "match-$Tag"
    New-Item -ItemType Directory -Force -Path $legDirectory | Out-Null

    $output = & $UciMatchPath `
        -KoiPath $engine `
        -OpponentPath $engine `
        -ReplayPath $replay `
        -Nodes $Nodes `
        -Threads $Threads `
        -Hash $Hash `
        -KoiColor $KoiColor `
        -Games $LegGames `
        -KoiOwnBook 'false' `
        -KoiOptions @{ EvalFile = $nnuePath } `
        -TimeoutMilliseconds $TimeoutMilliseconds `
        -OutputDirectory $legDirectory `
        -BatchId "nnue-ab-$Tag" `
        -RunLabel 'measurement'

    $jsonLine = @($output | Where-Object { $_ -match '^json\s+(.+)$' } | Select-Object -Last 1)
    if ($jsonLine.Count -ne 1) {
        throw "uci_match.ps1 did not report a report path for the '$Tag' leg."
    }
    $reportFile = $Matches[1].Trim()
    if (-not (Test-Path -LiteralPath $reportFile -PathType Leaf)) {
        throw "uci_match.ps1 report is missing: $reportFile"
    }
    return [pscustomobject]@{
        Tag = $Tag
        KoiColor = $KoiColor
        Games = $LegGames
        ReportPath = $reportFile
        Report = Get-Content -LiteralPath $reportFile -Raw | ConvertFrom-Json
    }
}

$whiteLeg = Invoke-AbLeg -Tag 'white' -KoiColor 'white' -LegGames $nnueWhiteGames
$blackLeg = Invoke-AbLeg -Tag 'black' -KoiColor 'black' -LegGames $nnueBlackGames

$wins = 0
$draws = 0
$losses = 0
$aborted = 0
$gameRecords = [System.Collections.Generic.List[object]]::new()

function Add-Games {
    param($Leg)

    foreach ($game in @($Leg.Report.games)) {
        $result = [string]$game.result
        $outcome = 'aborted'
        if ($result -ceq '1/2-1/2') {
            $script:draws++
            $outcome = 'draw'
        } elseif ($result -ceq '1-0' -or $result -ceq '0-1') {
            $winner = if ($result -ceq '1-0') { 'white' } else { 'black' }
            $nnueColor = $Leg.KoiColor
            if ($winner -ceq $nnueColor) {
                $script:wins++
                $outcome = 'win'
            } else {
                $script:losses++
                $outcome = 'loss'
            }
        } else {
            $script:aborted++
        }
        $gameRecords.Add([ordered]@{
            leg = $Leg.Tag
            nnue_color = $Leg.KoiColor
            result = $result
            outcome = $outcome
            termination = $game.termination
        })
    }
}

Add-Games -Leg $whiteLeg
Add-Games -Leg $blackLeg

$decided = $wins + $draws + $losses
$score = $wins + 0.5 * $draws
$percent = if ($decided -gt 0) { [math]::Round(100.0 * $score / $decided, 2) } else { 0.0 }
$verdict = if ($aborted -gt 0 -or $decided -lt $Games) {
    'incomplete'
} elseif ($percent -ge 55.0) {
    'nnue-stronger'
} elseif ($percent -le 45.0) {
    'classical-stronger'
} else {
    'inconclusive'
}

$report = [ordered]@{
    schema = 'koi-nnue-studio-ab-match-v1'
    generated_utc = (Get-Date).ToUniversalTime().ToString('o')
    nnue_path = $nnuePath
    engine_path = $engine
    nodes = $Nodes
    threads = $Threads
    hash_mb = $Hash
    games = $Games
    wins = $wins
    draws = $draws
    losses = $losses
    aborted = $aborted
    score = $score
    percent = $percent
    verdict = $verdict
    legs = @(
        [ordered]@{
            tag = $whiteLeg.Tag
            koi_color = $whiteLeg.KoiColor
            games = $whiteLeg.Games
            report = $whiteLeg.ReportPath
        },
        [ordered]@{
            tag = $blackLeg.Tag
            koi_color = $blackLeg.KoiColor
            games = $blackLeg.Games
            report = $blackLeg.ReportPath
        }
    )
    game_results = @($gameRecords)
}

$report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $ReportPath -Encoding UTF8
Write-Output ("nnue ab match {0}: +{1} ={2} -{3} ({4}%) {5}" -f $Games, $wins, $draws, $losses, $percent, $verdict)
Write-Output "json $ReportPath"

if ($aborted -gt 0) {
    throw "The A/B match produced $aborted aborted game(s); inspect the leg reports."
}
