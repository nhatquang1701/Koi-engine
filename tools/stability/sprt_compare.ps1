param(
    [Parameter(Mandatory = $true)]
    [string]$CandidatePath,
    [Parameter(Mandatory = $true)]
    [string]$BaselinePath,
    [string]$ReplayPath = '',
    [string]$OpeningFile = (Join-Path $PSScriptRoot '../../tests/data/openings/openings-curated-32.txt'),
    [int]$Nodes = 20000,
    [string]$TimeControl = '',
    [int]$Games = 2,
    [int]$MinGames = 40,
    [int]$MaxGames = 64,
    [double]$Elo0 = 0,
    [double]$Elo1 = 5,
    [int]$Threads = 1,
    [int]$Hash = 64,
    [string]$OutputDirectory = 'artifacts/matches/sprt-compare',
    [string]$Label = 'compare'
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

function Assert-FileExists([string]$Path, [string]$Description) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Description is missing: $Path"
    }
}

Assert-FileExists $CandidatePath 'candidate engine'
Assert-FileExists $BaselinePath 'baseline engine'
Assert-FileExists $ReplayPath 'replay tool'
Assert-FileExists $OpeningFile 'opening file'

$matchScript = Join-Path $PSScriptRoot 'uci_match.ps1'
Assert-FileExists $matchScript 'match harness'

$root = Join-Path $OutputDirectory ("{0}-{1}" -f $Label, (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $root -Force | Out-Null

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
        '-Games', "$Games",
        '-Sprt', '-SprtElo0', "$Elo0", '-SprtElo1', "$Elo1",
        '-SprtMinGames', "$MinGames", '-SprtMaxGames', "$MaxGames",
        '-RunLabel', 'after', '-KoiColor', $Color,
        '-OutputDirectory', (& $quote $colorDirectory)
    )
    if ($TimeControl -ne '') {
        $arguments += @('-TimeControl', (& $quote $TimeControl))
    } else {
        $arguments += @('-Nodes', "$Nodes")
    }
    $logBase = Join-Path $root $Color
    return Start-Process pwsh -ArgumentList $arguments -PassThru -WindowStyle Hidden `
        -RedirectStandardOutput "$logBase.out.log" -RedirectStandardError "$logBase.err.log"
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
    return $parsed.sprt
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
$games = [int]$white.games + [int]$black.games
$wins = [int]$white.wins + [int]$black.wins
$draws = [int]$white.draws + [int]$black.draws
$losses = [int]$white.losses + [int]$black.losses
$combined = Get-CombinedSprtResult -WhiteLlr ([double]$white.llr) -BlackLlr ([double]$black.llr) `
    -AcceptThreshold $acceptThreshold -RejectThreshold $rejectThreshold
$combinedLlr = [double]$combined.llr

Write-Output ("white: {0} games {1}W/{2}D/{3}L elo {4} llr {5} decision {6}" -f `
    $white.games, $white.wins, $white.draws, $white.losses, $white.elo, $white.llr, $white.decision)
Write-Output ("black: {0} games {1}W/{2}D/{3}L elo {4} llr {5} decision {6}" -f `
    $black.games, $black.wins, $black.draws, $black.losses, $black.elo, $black.llr, $black.decision)

$decision = [string]$combined.decision

$score = if ($games -gt 0) { ([double]$wins + 0.5 * [double]$draws) / [double]$games } else { 0.0 }
$combinedElo = ([double]$white.elo + [double]$black.elo) / 2.0
Write-Output ("combined: {0} games {1}W/{2}D/{3}L score {4} elo {5} llr {6} decision {7}" -f `
    $games, $wins, $draws, $losses,
    [Math]::Round($score, 4), [Math]::Round($combinedElo, 1), [Math]::Round($combinedLlr, 3), $decision)
Write-Output "sprt-compare: results under $root"

if ($decision -eq 'accept') { exit 0 }
if ($decision -eq 'reject') { exit 1 }
exit 2
