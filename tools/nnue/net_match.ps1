<#
.SYNOPSIS
Node-limited A/B match: a Koi NNUE network against another Koi NNUE network.

.DESCRIPTION
Runs two node-limited matches through tools/stability/uci_match.ps1.  Both
sides are the same Koi engine executable; the candidate network is loaded on
the Koi side through `EvalFile` and the reference network on the opponent side
through the opponent option set, then the games are split so the candidate
plays white and black.

The aggregated report is written to JSON (schema koi-nnue-net-match-v1).  It is
a local observation for the training campaign, not an Elo claim and not a CI
threshold.  The promotion verdict is an SPRT (H0 = 0 Elo, H1 = +10 Elo,
alpha = beta = 0.05) over the combined color legs; two games that share a move
sequence mark the report invalid, and an SPRT accept below the 200-effective-game
target reports 'accepted-below-target' instead of 'candidate-stronger'.

.PARAMETER NnueNet
Path to the `.nnue` file used by the candidate side. The container must be a
Koi v4 or v5 NNUE file; it is attested through uci_match.ps1's
-KoiEvalFile/-KoiEvaluatorMode path and its SHA-256 is recorded.

.PARAMETER OpponentNet
Path to the `.nnue` file used by the reference side. The container must be a
Koi v4 or v5 NNUE file; its SHA-256 is recorded (the reference side has no
harness attestation path).

.PARAMETER EnginePath
Koi engine executable (default: build/release/koi-engine.exe).

.PARAMETER Games
Total games; the odd game goes to the candidate-white half. Every game must
play a distinct opening, so the request is refused unless Games is at most
twice the opening count.

.PARAMETER Nodes
Per-move node limit for both engines (default 20000).

.PARAMETER OpeningFile
Opening file with `name | uci move uci move ...` lines. When omitted, the
160-opening release-strength corpus
(tests/data/openings/openings-release-strength-160.txt) is preferred; the
32-opening curated book is the fallback. Each game plays exactly one opening
with one color and one derived seed, so the match is refused instead of
silently repeating a game.

.EXAMPLE
pwsh -NoProfile -File tools/nnue/net_match.ps1 -NnueNet artifacts/training/koi-v4-1024.nnue -OpponentNet artifacts/training/koi-sf-v1.nnue -Games 20
#>

param(
    [Parameter(Mandatory = $true)]
    [string]$NnueNet,

    [Parameter(Mandatory = $true)]
    [string]$OpponentNet,

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

    [string]$OpeningFile,

    [string]$OutputDirectory,

    [string]$ReportPath,

    [string]$UciMatchPath
)

$ErrorActionPreference = 'Stop'
$repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$binarySuffix = if ($env:OS -eq 'Windows_NT') { '.exe' } else { '' }
if ([string]::IsNullOrWhiteSpace($EnginePath)) {
    $EnginePath = Join-Path (Join-Path $repositoryRoot 'build/release') "koi-engine$binarySuffix"
}
if ([string]::IsNullOrWhiteSpace($UciMatchPath)) {
    $UciMatchPath = Join-Path (Join-Path $repositoryRoot 'tools/stability') 'uci_match.ps1'
}
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path (Join-Path $repositoryRoot 'artifacts/matches') ('nnue-net-' +
        (Get-Date).ToUniversalTime().ToString('yyyyMMdd-HHmmss-fff'))
}
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null

$nnuePath = (Resolve-Path -LiteralPath $NnueNet).Path
$opponentPath = (Resolve-Path -LiteralPath $OpponentNet).Path
$engine = (Resolve-Path -LiteralPath $EnginePath).Path
$replay = Join-Path (Split-Path -Parent $engine) "koi-replay$binarySuffix"
if (-not (Test-Path -LiteralPath $replay -PathType Leaf)) {
    throw "koi-replay executable is missing beside the engine: $replay"
}
if (-not (Test-Path -LiteralPath $UciMatchPath -PathType Leaf)) {
    throw "uci_match.ps1 is missing: $UciMatchPath"
}
function Read-NnueContainerInfo {
    param(
        [string]$Path,
        [string]$Label
    )

    $header = New-Object byte[] 12
    $stream = [System.IO.File]::OpenRead($Path)
    try {
        $headerBytesRead = $stream.Read($header, 0, $header.Length)
    } finally {
        $stream.Dispose()
    }
    if ($headerBytesRead -lt 12 -or
        [System.Text.Encoding]::ASCII.GetString($header, 0, 8) -cne 'KOI-NNUE') {
        throw "$Label is not a Koi NNUE container: $Path"
    }
    $containerVersion = [System.BitConverter]::ToUInt32($header, 8)
    $evaluatorMode = switch ($containerVersion) {
        4 { 'nnue-v4' }
        5 { 'nnue-v5' }
        default { $null }
    }
    if ($null -eq $evaluatorMode) {
        throw ("$Label uses NNUE container version $containerVersion, but net_match.ps1 only attests " +
            'v4 and v5 containers through uci_match.ps1 -KoiEvaluatorMode.')
    }
    return [pscustomobject]@{
        ContainerVersion = $containerVersion
        EvaluatorMode = $evaluatorMode
        Sha256 = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}

# Attest both containers before any game starts. The candidate net goes
# through uci_match.ps1's -KoiEvalFile/-KoiEvaluatorMode path, which proves the
# engine enabled that exact file; the reference net is header-validated here
# and both SHA-256 hashes are recorded in the aggregated report.
$nnueInfo = Read-NnueContainerInfo -Path $nnuePath -Label 'NnueNet'
$opponentInfo = Read-NnueContainerInfo -Path $opponentPath -Label 'OpponentNet'
$candidateEvaluatorMode = $nnueInfo.EvaluatorMode
$nnueSha256 = $nnueInfo.Sha256
$opponentSha256 = $opponentInfo.Sha256
function Resolve-OpeningFile {
    param(
        [string]$RequestedPath,
        [string]$Root
    )

    if (-not [string]::IsNullOrWhiteSpace($RequestedPath)) {
        return (Resolve-Path -LiteralPath $RequestedPath).Path
    }
    $openingDirectory = Join-Path $Root 'tests/data/openings'
    $releaseOpeningFile = Join-Path $openingDirectory 'openings-release-strength-160.txt'
    $curatedOpeningFile = Join-Path $openingDirectory 'openings-curated-32.txt'
    if (Test-Path -LiteralPath $releaseOpeningFile -PathType Leaf) {
        return $releaseOpeningFile
    }
    if (Test-Path -LiteralPath $curatedOpeningFile -PathType Leaf) {
        Write-Warning "The 160-opening release-strength corpus is absent; falling back to $curatedOpeningFile."
        return $curatedOpeningFile
    }
    throw "No opening file is available: expected $releaseOpeningFile or $curatedOpeningFile."
}

$OpeningFile = Resolve-OpeningFile -RequestedPath $OpeningFile -Root $repositoryRoot
$openingLines = @(Get-Content -LiteralPath $OpeningFile |
    Where-Object { -not [string]::IsNullOrWhiteSpace($_) -and -not $_.TrimStart().StartsWith('#') })
$openingCount = $openingLines.Count
if ($openingCount -lt 1) {
    throw "Opening file contains no openings: $OpeningFile"
}
Write-Output "openings $openingCount $OpeningFile"
if ([string]::IsNullOrWhiteSpace($ReportPath)) {
    $ReportPath = Join-Path $OutputDirectory 'net-match.json'
}

$candidateWhiteGames = [int][math]::Ceiling($Games / 2.0)
$candidateBlackGames = $Games - $candidateWhiteGames
$maximumDistinctGames = $openingCount * 2
if ($Games -gt $maximumDistinctGames) {
    throw ("Requested $Games games but $OpeningFile contains only $openingCount distinct openings; " +
        "with both colors per opening at most $maximumDistinctGames games can be distinct. " +
        "Reduce -Games or provide a larger opening file.")
}

function Get-MoveSequenceHash {
    param(
        [string]$KoiColor,
        [string[]]$Moves
    )

    if ($Moves.Count -eq 0) {
        return $null
    }
    $payload = "$KoiColor|" + ($Moves -join ' ')
    $sha256 = [System.Security.Cryptography.SHA256]::Create()
    try {
        $digest = $sha256.ComputeHash([System.Text.Encoding]::UTF8.GetBytes($payload))
        return (($digest | ForEach-Object { $_.ToString('x2') }) -join '')
    } finally {
        $sha256.Dispose()
    }
}

function Get-DuplicateGameAnalysis {
    param([object[]]$Games)

    $moveHashes = [System.Collections.Generic.List[string]]::new()
    $emptyMoveGames = 0
    foreach ($game in $Games) {
        if ($null -eq $game.move_hash) {
            ++$emptyMoveGames
        } else {
            $moveHashes.Add([string]$game.move_hash)
        }
    }
    $duplicateGroups = @($moveHashes | Group-Object | Where-Object { $_.Count -gt 1 })
    $distinctGames = @($moveHashes | Sort-Object -Unique).Count + $emptyMoveGames
    return [pscustomobject]@{
        effective_games = $distinctGames
        duplicate_games_detected = ($duplicateGroups.Count -gt 0)
        duplicate_games = ($Games.Count - $distinctGames)
        duplicate_groups = @($duplicateGroups | ForEach-Object {
            [ordered]@{ move_hash = $_.Name; games = $_.Count }
        })
    }
}

# Same logistic SPRT model as uci_match.ps1 (see Get-SprtState there).
function Get-SprtState {
    param(
        [int]$Wins,
        [int]$Draws,
        [int]$Losses,
        [double]$Elo0,
        [double]$Elo1,
        [double]$Alpha,
        [double]$Beta
    )

    $games = $Wins + $Draws + $Losses
    $score = [double]$Wins + 0.5 * [double]$Draws
    $mu0 = 1.0 / (1.0 + [Math]::Pow(10.0, -$Elo0 / 400.0))
    $mu1 = 1.0 / (1.0 + [Math]::Pow(10.0, -$Elo1 / 400.0))
    $lower = [Math]::Log($Beta / (1.0 - $Alpha))
    $upper = [Math]::Log((1.0 - $Beta) / $Alpha)

    $llr = 0.0
    if ($games -ge 2) {
        $mean = $score / $games
        $variance = (
            $Wins * (1.0 - $mean) * (1.0 - $mean) +
            $Losses * $mean * $mean +
            $Draws * (0.5 - $mean) * (0.5 - $mean)) / ($games - 1)
        if ($variance -lt 1e-9) {
            $variance = $mu0 * (1.0 - $mu0)
        }
        $llr = (($mu1 - $mu0) * (2.0 * $score - $games * ($mu0 + $mu1))) / (2.0 * $variance)
    }

    $elo = $null
    if ($games -gt 0) {
        $rate = $score / $games
        if ($rate -le 0.0) {
            $elo = -3000.0
        } elseif ($rate -ge 1.0) {
            $elo = 3000.0
        } else {
            $elo = 400.0 / [Math]::Log(10.0) * [Math]::Log($rate / (1.0 - $rate))
        }
    }

    $decision = 'running'
    if ($llr -ge $upper) {
        $decision = 'accept'
    } elseif ($llr -le $lower) {
        $decision = 'reject'
    }

    return [pscustomobject]@{
        llr = [Math]::Round($llr, 6)
        lower = [Math]::Round($lower, 6)
        upper = [Math]::Round($upper, 6)
        decision = $decision
        games = $games
        wins = $Wins
        draws = $Draws
        losses = $Losses
        score = $score
        elo = $elo
    }
}

function Get-NetMatchVerdict {
    param(
        [int]$Aborted,
        [int]$Decided,
        [int]$RequestedGames,
        [bool]$DuplicateGamesDetected,
        [string]$SprtDecision,
        [bool]$TargetMet
    )

    if ($Aborted -gt 0 -or $Decided -lt $RequestedGames) {
        return 'incomplete'
    }
    if ($DuplicateGamesDetected) {
        return 'invalid'
    }
    if ($SprtDecision -eq 'accept') {
        if ($TargetMet) {
            return 'candidate-stronger'
        }
        # An SPRT accept below the effective-games target is evidence, not a
        # promotion: keep it distinct from candidate-stronger.
        return 'accepted-below-target'
    }
    if ($SprtDecision -eq 'reject') {
        return 'candidate-weaker'
    }
    return 'inconclusive'
}

function Invoke-NetLeg {
    param(
        [string]$Tag,
        [string]$KoiColor,
        [int]$LegGames,
        [uint64]$BaseSeed
    )

    if ($LegGames -gt $script:openingCount) {
        throw ("The '$Tag' leg requests $LegGames games but $($script:OpeningFile) contains only " +
            "$($script:openingCount) distinct openings; repeated games are refused.")
    }

    $legDirectory = Join-Path $OutputDirectory "match-$Tag"
    New-Item -ItemType Directory -Force -Path $legDirectory | Out-Null

    # One game per opening: uci_match.ps1 plays -Games games per opening, so a
    # subset of exactly $LegGames openings with -Games 1 gives every game its
    # own opening, color, and RandomSeed.
    $legOpeningFile = Join-Path $legDirectory 'openings.txt'
    Set-Content -LiteralPath $legOpeningFile -Encoding UTF8 -Value $script:openingLines[0..($LegGames - 1)]

    $output = & $UciMatchPath `
        -KoiPath $engine `
        -OpponentPath $engine `
        -ReplayPath $replay `
        -Nodes $Nodes `
        -Threads $Threads `
        -Hash $Hash `
        -KoiColor $KoiColor `
        -KoiRandomSeed $BaseSeed `
        -Games 1 `
        -OpeningFile $legOpeningFile `
        -KoiOwnBook 'false' `
        -KoiEvalFile $nnuePath `
        -KoiEvaluatorMode $script:candidateEvaluatorMode `
        -OpponentOptions @{ EvalFile = $opponentPath } `
        -TimeoutMilliseconds $TimeoutMilliseconds `
        -OutputDirectory $legDirectory `
        -BatchId "nnue-net-$Tag" `
        -RunLabel 'measurement'

    $jsonLine = @($output | Where-Object { $_ -match '^json\s+(.+)$' } | Select-Object -Last 1)
    if ($jsonLine.Count -ne 1) {
        throw "uci_match.ps1 did not report a report path for the '$Tag' leg."
    }
    $reportFile = $Matches[1].Trim()
    if (-not (Test-Path -LiteralPath $reportFile -PathType Leaf)) {
        throw "uci_match.ps1 report is missing: $reportFile"
    }
    $legReport = Get-Content -LiteralPath $reportFile -Raw | ConvertFrom-Json
    $legConfiguration = $legReport.configuration
    $legAttestation = $legConfiguration.koi_evaluator_attestation
    if ($legConfiguration.koi_evaluator_mode -cne $script:candidateEvaluatorMode -or
        $legConfiguration.koi_evalfile_sha256 -cne $script:nnueSha256 -or
        $null -eq $legAttestation -or
        @($legAttestation.evalfile_rejections).Count -ne 0) {
        throw ("The '$Tag' leg report does not attest the candidate network " +
            "($($script:candidateEvaluatorMode), $($script:nnueSha256)).")
    }
    return [pscustomobject]@{
        Tag = $Tag
        KoiColor = $KoiColor
        Games = $LegGames
        ReportPath = $reportFile
        Report = $legReport
    }
}

$whiteLeg = Invoke-NetLeg -Tag 'white' -KoiColor 'white' -LegGames $candidateWhiteGames -BaseSeed 1
$blackLeg = Invoke-NetLeg -Tag 'black' -KoiColor 'black' -LegGames $candidateBlackGames `
    -BaseSeed ([uint64](1 + $candidateWhiteGames))

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
            if ($winner -ceq $Leg.KoiColor) {
                $script:wins++
                $outcome = 'win'
            } else {
                $script:losses++
                $outcome = 'loss'
            }
        } else {
            $script:aborted++
        }
        $moves = @($game.moves | ForEach-Object { [string]$_.move })
        $gameRecords.Add([ordered]@{
            leg = $Leg.Tag
            candidate_color = $Leg.KoiColor
            position = $game.position
            koi_random_seed = $game.koi_random_seed
            result = $result
            outcome = $outcome
            termination = $game.termination
            moves = $moves.Count
            move_hash = Get-MoveSequenceHash -KoiColor $Leg.KoiColor -Moves $moves
        })
    }
}

Add-Games -Leg $whiteLeg
Add-Games -Leg $blackLeg

$decided = $wins + $draws + $losses
$score = $wins + 0.5 * $draws
$percent = if ($decided -gt 0) { [math]::Round(100.0 * $score / $decided, 2) } else { 0.0 }

# Promotion verdict: SPRT (H0 = 0 Elo, H1 = +10 Elo, alpha = beta = 0.05) over
# the combined color legs. The raw score stays for reporting; it no longer
# decides the verdict.
$sprtElo0 = 0.0
$sprtElo1 = 10.0
$sprtAlpha = 0.05
$sprtBeta = 0.05
$sprtTargetEffectiveGames = 200
$sprtState = Get-SprtState -Wins $wins -Draws $draws -Losses $losses `
    -Elo0 $sprtElo0 -Elo1 $sprtElo1 -Alpha $sprtAlpha -Beta $sprtBeta
$sprtDecision = if ($sprtState.decision -eq 'running') { 'inconclusive' } else { $sprtState.decision }

$duplicateAnalysis = Get-DuplicateGameAnalysis -Games @($gameRecords)
$effectiveGames = $duplicateAnalysis.effective_games
$duplicateGamesDetected = $duplicateAnalysis.duplicate_games_detected
$sprtTargetMet = ($effectiveGames -ge $sprtTargetEffectiveGames)

$verdict = Get-NetMatchVerdict -Aborted $aborted -Decided $decided -RequestedGames $Games `
    -DuplicateGamesDetected $duplicateGamesDetected -SprtDecision $sprtDecision -TargetMet $sprtTargetMet

$report = [ordered]@{
    schema = 'koi-nnue-net-match-v1'
    generated_utc = (Get-Date).ToUniversalTime().ToString('o')
    nnue_path = $nnuePath
    nnue_sha256 = $nnueSha256
    nnue_container_version = $nnueInfo.ContainerVersion
    koi_evaluator_mode = $candidateEvaluatorMode
    opponent_path = $opponentPath
    opponent_sha256 = $opponentSha256
    opponent_container_version = $opponentInfo.ContainerVersion
    engine_path = $engine
    nodes = $Nodes
    threads = $Threads
    hash_mb = $Hash
    games_requested = $Games
    requested_games = $Games
    games = $decided + $aborted
    effective_games = $effectiveGames
    opening_file = $OpeningFile
    wins = $wins
    draws = $draws
    losses = $losses
    aborted = $aborted
    score = $score
    percent = $percent
    verdict = $verdict
    report_valid = -not $duplicateGamesDetected
    duplicate_games_detected = $duplicateGamesDetected
    duplicate_game_groups = @($duplicateAnalysis.duplicate_groups)
    sprt = [ordered]@{
        enabled = $true
        elo0 = $sprtElo0
        elo1 = $sprtElo1
        alpha = $sprtAlpha
        beta = $sprtBeta
        target_effective_games = $sprtTargetEffectiveGames
        target_met = $sprtTargetMet
        decision = $sprtDecision
        llr = $sprtState.llr
        lower = $sprtState.lower
        upper = $sprtState.upper
        games = $sprtState.games
    }
    legs = @(
        [ordered]@{
            tag = $whiteLeg.Tag
            candidate_color = $whiteLeg.KoiColor
            games = @($whiteLeg.Report.games).Count
            report = $whiteLeg.ReportPath
        },
        [ordered]@{
            tag = $blackLeg.Tag
            candidate_color = $blackLeg.KoiColor
            games = @($blackLeg.Report.games).Count
            report = $blackLeg.ReportPath
        }
    )
    game_results = @($gameRecords)
}

$report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $ReportPath -Encoding UTF8
Write-Output ("nnue net match {0}: +{1} ={2} -{3} ({4}%) {5} effective_games {6}/{7}" -f `
    ($decided + $aborted), $wins, $draws, $losses, $percent, $verdict, $effectiveGames, $Games)
Write-Output ("nnue net match sprt: elo0 {0} elo1 {1} llr {2} decision {3} target {4} met {5}" -f `
    $sprtElo0, $sprtElo1, $sprtState.llr, $sprtDecision, $sprtTargetEffectiveGames, $report.sprt.target_met)
if ($duplicateGamesDetected) {
    Write-Output (("WARNING: duplicate games detected: {0} of {1} games repeat an earlier color+move sequence; " +
        "effective_games {2}; the report is marked invalid.") -f `
        $duplicateAnalysis.duplicate_games, ($decided + $aborted), $effectiveGames)
    Write-Warning (("duplicate games detected: {0} game(s) repeat an earlier color+move sequence; " +
        "effective_games {1} of {2}; report marked invalid.") -f `
        $duplicateAnalysis.duplicate_games, $effectiveGames, ($decided + $aborted))
}
Write-Output "json $ReportPath"

if ($aborted -gt 0) {
    throw "The net-vs-net match produced $aborted aborted game(s); inspect the leg reports."
}
