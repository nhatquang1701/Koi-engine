param(
    [Parameter(Mandatory = $true)]
    [string]$CandidatePath,
    [Parameter(Mandatory = $true)]
    [string]$BaselinePath,
    [string]$ReplayPath = '',
    [string]$OpeningFile = (Join-Path $PSScriptRoot '../../tests/data/openings/openings-curated-32.txt'),
    [int]$Nodes = 20000,
    [string]$TimeControl = '',
    [int]$Games = 1,
    [int]$MinGames = 40,
    [int]$MaxGames = 64,
    [double]$Elo0 = 0,
    [double]$Elo1 = 5,
    [int]$Threads = 1,
    [int]$Hash = 64,
    [string]$OutputDirectory = 'artifacts/matches/sprt-compare',
    [string]$Label = 'compare',
    [string]$CandidateOptionsJsonPath = '',
    [string]$BaselineOptionsJsonPath = '',
    [ValidateRange(1, 512)]
    [int]$MaxPlies = 512
)

$binarySuffix = if ($env:OS -eq 'Windows_NT') { '.exe' } else { '' }
if ([string]::IsNullOrWhiteSpace($ReplayPath)) {
    $ReplayPath = Join-Path (Join-Path $PSScriptRoot '../../build/release') "koi-replay$binarySuffix"
}

$ErrorActionPreference = 'Stop'

# Combined-bounds LLR thresholds for alpha = beta = 0.05 (the same bounds the
# per-color SPRT runs use), applied to the sum of the two independent colors.
$acceptThreshold = [Math]::Log(19.0)   # ln((1 - beta) / alpha)
$rejectThreshold = -$acceptThreshold

function Get-CombinedSprtResult([double]$WhiteLlr, [double]$BlackLlr,
                                [double]$AcceptThreshold, [double]$RejectThreshold) {
    $llr = $WhiteLlr + $BlackLlr
    $decision = if ($llr -ge $AcceptThreshold) {
        'accept'
    } elseif ($llr -le $RejectThreshold) {
        'reject'
    } else {
        'inconclusive'
    }
    return [pscustomobject]@{ llr = $llr; decision = $decision }
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

function Assert-FileExists([string]$Path, [string]$Description) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Description is missing: $Path"
    }
}

Assert-FileExists $CandidatePath 'candidate engine'
Assert-FileExists $BaselinePath 'baseline engine'
Assert-FileExists $ReplayPath 'replay tool'
Assert-FileExists $OpeningFile 'opening file'
if (-not [string]::IsNullOrWhiteSpace($CandidateOptionsJsonPath)) {
    Assert-FileExists $CandidateOptionsJsonPath 'candidate UCI options JSON'
}
if (-not [string]::IsNullOrWhiteSpace($BaselineOptionsJsonPath)) {
    Assert-FileExists $BaselineOptionsJsonPath 'baseline UCI options JSON'
}

$matchScript = Join-Path $PSScriptRoot 'uci_match.ps1'
Assert-FileExists $matchScript 'match harness'

$root = [System.IO.Path]::GetFullPath(
    (Join-Path $OutputDirectory ("{0}-{1}" -f $Label, (Get-Date -Format 'yyyyMMdd-HHmmss'))))
New-Item -ItemType Directory -Path $root -Force | Out-Null
$candidateOptionsCopy = ''
$baselineOptionsCopy = ''
if (-not [string]::IsNullOrWhiteSpace($CandidateOptionsJsonPath)) {
    $candidateOptionsCopy = Join-Path $root 'candidate-options.json'
    Copy-Item -LiteralPath (Resolve-Path -LiteralPath $CandidateOptionsJsonPath).Path `
        -Destination $candidateOptionsCopy
}
if (-not [string]::IsNullOrWhiteSpace($BaselineOptionsJsonPath)) {
    $baselineOptionsCopy = Join-Path $root 'baseline-options.json'
    Copy-Item -LiteralPath (Resolve-Path -LiteralPath $BaselineOptionsJsonPath).Path `
        -Destination $baselineOptionsCopy
}

function Start-ColorRun([string]$Color) {
    $colorDirectory = Join-Path $root $Color
    # Start-Process joins the argument list with spaces without quoting, so
    # every value is quoted explicitly to survive paths that contain spaces.
    $quote = { param([string]$value) '"' + $value + '"' }
    $arguments = @(
        '-NoProfile', '-File', (& $quote $matchScript),
        '-KoiPath', (& $quote (Resolve-Path -LiteralPath $CandidatePath).Path),
        '-OpponentPath', (& $quote (Resolve-Path -LiteralPath $BaselinePath).Path),
        '-ReplayPath', (& $quote (Resolve-Path -LiteralPath $ReplayPath).Path),
        '-OpeningFile', (& $quote (Resolve-Path -LiteralPath $OpeningFile).Path),
        '-Threads', "$Threads",
        '-Hash', "$Hash",
        '-KoiRandomSeed', '0',
        '-KoiOwnBook', 'false',
        '-OpponentOwnBook', 'false',
        '-MaxPlies', "$MaxPlies",
        '-Games', "$Games",
        '-Sprt', '-SprtElo0', "$Elo0", '-SprtElo1', "$Elo1",
        '-SprtMinGames', "$MinGames", '-SprtMaxGames', "$MaxGames",
        '-RunLabel', 'after', '-KoiColor', $Color,
        '-CompactSearchInfo',
        '-OutputDirectory', (& $quote $colorDirectory)
    )
    if ($TimeControl -ne '') {
        $arguments += @('-TimeControl', (& $quote $TimeControl))
    } else {
        $arguments += @('-Nodes', "$Nodes")
    }
    if (-not [string]::IsNullOrWhiteSpace($candidateOptionsCopy)) {
        $arguments += @('-KoiOptionsJsonPath', (& $quote $candidateOptionsCopy))
    }
    if (-not [string]::IsNullOrWhiteSpace($baselineOptionsCopy)) {
        $arguments += @('-OpponentOptionsJsonPath', (& $quote $baselineOptionsCopy))
    }
    $logBase = Join-Path $root $Color
    $startParameters = @{
        FilePath = 'pwsh'
        ArgumentList = $arguments
        PassThru = $true
        RedirectStandardOutput = "$logBase.out.log"
        RedirectStandardError = "$logBase.err.log"
    }
    if ($env:OS -eq 'Windows_NT') {
        $startParameters.WindowStyle = 'Hidden'
    }
    return Start-Process @startParameters
}

function Read-ColorResult([string]$Color) {
    $colorDirectory = Join-Path $root $Color
    $json = Get-ChildItem -LiteralPath $colorDirectory -Filter '*.json' -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($null -eq $json) {
        throw "no SPRT result JSON was written for $Color in $colorDirectory"
    }
    $parsed = Get-Content -LiteralPath $json.FullName -Raw | ConvertFrom-Json
    if ($null -eq $parsed.sprt) {
        throw "SPRT result JSON for $Color has no sprt block: $($json.FullName)"
    }
    return [pscustomobject]@{
        sprt = $parsed.sprt
        games = @($parsed.games)
    }
}

Write-Output "sprt-compare: candidate $CandidatePath vs baseline $BaselinePath"
if ($TimeControl -ne '') {
    Write-Output "sprt-compare: fixed time control $TimeControl, $Games game(s) per opening and color"
} else {
    Write-Output "sprt-compare: $Nodes nodes, $Games game(s) per opening and color"
}
$whiteProcess = Start-ColorRun 'white'
$blackProcess = Start-ColorRun 'black'
Write-Output "sprt-compare: white pid $($whiteProcess.Id), black pid $($blackProcess.Id)"

$whiteProcess.WaitForExit()
$blackProcess.WaitForExit()
if ($whiteProcess.ExitCode -ne 0 -or $blackProcess.ExitCode -ne 0) {
    throw "SPRT color run failed (white exit $($whiteProcess.ExitCode), black exit $($blackProcess.ExitCode)); see $root"
}

$white = Read-ColorResult 'white'
$black = Read-ColorResult 'black'
# PowerShell variable names are case-insensitive, so the combined $games total
# below would overwrite the requested $Games parameter. Snapshot it first.
$requestedGamesPerOpening = $Games
$games = [int]$white.sprt.games + [int]$black.sprt.games
$wins = [int]$white.sprt.wins + [int]$black.sprt.wins
$draws = [int]$white.sprt.draws + [int]$black.sprt.draws
$losses = [int]$white.sprt.losses + [int]$black.sprt.losses
$combined = Get-CombinedSprtResult -WhiteLlr ([double]$white.sprt.llr) -BlackLlr ([double]$black.sprt.llr) `
    -AcceptThreshold $acceptThreshold -RejectThreshold $rejectThreshold
$combinedLlr = [double]$combined.llr

# Duplicate-game assertion: a game is identified by the candidate color plus
# the full recorded move sequence, so a color-reversed replay stays a distinct
# game while a repeated (position, color, seed) match collapses to one game.
$gameRecords = [System.Collections.Generic.List[object]]::new()
foreach ($colorResult in @(
    [pscustomobject]@{ color = 'white'; games = $white.games },
    [pscustomobject]@{ color = 'black'; games = $black.games }
)) {
    foreach ($game in @($colorResult.games)) {
        $moves = @($game.moves | ForEach-Object { [string]$_.move })
        $gameRecords.Add([pscustomobject]@{
            koi_color = $colorResult.color
            move_hash = Get-MoveSequenceHash -KoiColor $colorResult.color -Moves $moves
        })
    }
}
$duplicateAnalysis = Get-DuplicateGameAnalysis -Games @($gameRecords)
$playedGames = $gameRecords.Count

Write-Output ("white: {0} games {1}W/{2}D/{3}L elo {4} llr {5} decision {6}" -f `
    $white.sprt.games, $white.sprt.wins, $white.sprt.draws, $white.sprt.losses, $white.sprt.elo, $white.sprt.llr, $white.sprt.decision)
Write-Output ("black: {0} games {1}W/{2}D/{3}L elo {4} llr {5} decision {6}" -f `
    $black.sprt.games, $black.sprt.wins, $black.sprt.draws, $black.sprt.losses, $black.sprt.elo, $black.sprt.llr, $black.sprt.decision)
Write-Output ("sprt-compare: effective_games {0} distinct of {1} played; requested {2} game(s) per opening and color" -f `
    $duplicateAnalysis.effective_games, $playedGames, $requestedGamesPerOpening)
if ($duplicateAnalysis.duplicate_games_detected) {
    Write-Output (("WARNING: duplicate games detected: {0} of {1} games repeat an earlier color+move sequence; " +
        "the combined SPRT evidence is inflated.") -f $duplicateAnalysis.duplicate_games, $playedGames)
    Write-Warning (("sprt-compare: duplicate games detected: {0} of {1} games repeat an earlier color+move sequence; " +
        "effective_games {2}.") -f $duplicateAnalysis.duplicate_games, $playedGames, $duplicateAnalysis.effective_games)
}

$decision = [string]$combined.decision

$score = if ($games -gt 0) { ([double]$wins + 0.5 * [double]$draws) / [double]$games } else { 0.0 }
$combinedElo = ([double]$white.sprt.elo + [double]$black.sprt.elo) / 2.0
Write-Output ("combined: {0} games {1}W/{2}D/{3}L score {4} elo {5} llr {6} decision {7}" -f `
    $games, $wins, $draws, $losses,
    [Math]::Round($score, 4), [Math]::Round($combinedElo, 1), [Math]::Round($combinedLlr, 3), $decision)
Write-Output "sprt-compare: results under $root"

if ($decision -eq 'accept') { exit 0 }
if ($decision -eq 'reject') { exit 1 }
exit 2
