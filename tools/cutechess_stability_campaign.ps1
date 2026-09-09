param(
    [Parameter(Mandatory = $true)]
    [string]$KoiPath,
    [Parameter(Mandatory = $true)]
    [string]$StockfishPath,
    [string]$CutechessPath = 'C:\Program Files (x86)\Cute Chess\cutechess-cli.exe',
    [string]$OutputDirectory = '',
    [int]$Games = 500,
    [int]$MaxMoves = 200,
    [string]$TimeControl = '1+0',
    [int]$Hash = 512,
    [int]$Speed = 100,
    [string]$BookFile = 'book.bin',
    [ValidateRange(0, 40)]
    [int]$BookDepth = 16,
    [string]$OpeningFile = '',
    [string]$FenFile = '',
    [int]$Seed = 20260908,
    [ValidateRange(1, 86400000)]
    [int]$GameTimeoutMilliseconds = 180000,
    [switch]$PlanOnly,
    [switch]$EnableDebug
)

$ErrorActionPreference = 'Stop'

function Assert-File([string]$Path, [string]$Description) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Description is missing: $Path"
    }
    return (Resolve-Path -LiteralPath $Path).Path
}

function Get-Hash([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
}

function Write-JsonFile([string]$Path, $Value) {
    $json = $Value | ConvertTo-Json -Depth 24
    [System.IO.File]::WriteAllText($Path, $json, [System.Text.UTF8Encoding]::new($false))
}

function Write-Utf8File([string]$Path, [string]$Text) {
    [System.IO.File]::WriteAllText($Path, $Text, [System.Text.UTF8Encoding]::new($false))
}

function Add-JsonLine([string]$Path, $Value) {
    $json = ($Value | ConvertTo-Json -Compress -Depth 24) + [Environment]::NewLine
    [System.IO.File]::AppendAllText($Path, $json, [System.Text.UTF8Encoding]::new($false))
}

function Read-OpeningEntries([string]$Path) {
    $entries = [System.Collections.Generic.List[object]]::new()
    foreach ($line in Get-Content -LiteralPath $Path) {
        $trimmed = $line.Trim()
        if ($trimmed.Length -eq 0 -or $trimmed.StartsWith('#')) {
            continue
        }
        $match = [regex]::Match(
            $trimmed,
            '^(?<name>[^|#\s]+)\s*\|\s*(?<moves>[a-h][1-8][a-h][1-8][nbrq]?(?:\s+[a-h][1-8][a-h][1-8][nbrq]?)*?)\s*$')
        if (-not $match.Success) {
            throw "Invalid opening line: $trimmed"
        }
        $entries.Add([pscustomobject]@{
                name = $match.Groups['name'].Value
                moves = $match.Groups['moves'].Value
                line = $trimmed
            })
    }
    if ($entries.Count -eq 0) {
        throw "Opening file contains no entries: $Path"
    }
    return @($entries)
}

function Read-FenEntries([string]$Path) {
    $entries = [System.Collections.Generic.List[object]]::new()
    foreach ($line in Get-Content -LiteralPath $Path) {
        $trimmed = $line.Trim()
        if ($trimmed.Length -eq 0 -or $trimmed.StartsWith('#')) {
            continue
        }
        $match = [regex]::Match($trimmed, '^(?<name>[^|#\s]+)\s*\|\s*(?<fen>.+)$')
        if (-not $match.Success) {
            throw "Invalid FEN line: $trimmed"
        }
        $fen = ($match.Groups['fen'].Value -split '\s+') -join ' '
        if (@($fen -split '\s+').Count -ne 6) {
            throw "FEN entry '$($match.Groups['name'].Value)' must contain exactly six fields."
        }
        $entries.Add([pscustomobject]@{
                name = $match.Groups['name'].Value
                fen = $fen
                line = $trimmed
            })
    }
    if ($entries.Count -eq 0) {
        throw "FEN file contains no entries: $Path"
    }
    return @($entries)
}

function Get-SourcePath([string]$RepositoryRoot, [string]$Requested, [string]$DefaultRelative) {
    if ([string]::IsNullOrWhiteSpace($Requested)) {
        return (Resolve-Path -LiteralPath (Join-Path $RepositoryRoot $DefaultRelative)).Path
    }
    return Assert-File $Requested 'Input source'
}

function New-Allocation([int]$RequestedGames, [bool]$BookAvailable) {
    if ($RequestedGames -eq 500) {
        $stockfish = 400
        $koiKoi = 50
        $book = 50
    } else {
        $stockfish = [math]::Floor($RequestedGames * 0.80)
        $koiKoi = [math]::Floor($RequestedGames * 0.10)
        $book = $RequestedGames - $stockfish - $koiKoi
    }
    return [ordered]@{
        stockfish_games = [int]$stockfish
        koi_koi_games = [int]$koiKoi
        book_games = [int]$book
        book_available = $BookAvailable
        book_unavailable_substitution = -not $BookAvailable
    }
}

function New-Schedule([int]$RequestedGames, [int]$ScheduleSeed, $Allocation, $OpeningEntries, $FenEntries,
                      [string]$BookResolvedPath) {
    $schedule = [System.Collections.Generic.List[object]]::new()
    $groupCounts = @{
        stockfish = 0
        koi_koi = 0
        book = 0
    }
    $threadCycle = @(1, 2, 4)
    for ($index = 0; $index -lt $RequestedGames; $index++) {
        $ordinal = $index + 1
        if ($index -lt $Allocation.stockfish_games) {
            $group = 'stockfish'
            $opponentKind = 'stockfish-19'
            $bookRequested = $false
        } elseif ($index -lt ($Allocation.stockfish_games + $Allocation.koi_koi_games)) {
            $group = 'koi_koi'
            $opponentKind = 'koi'
            $bookRequested = $false
        } else {
            $group = 'book'
            $opponentKind = 'stockfish-19'
            $bookRequested = $true
        }
        $groupCounts[$group]++
        $koiColor = if (($groupCounts[$group] % 2) -eq 1) { 'white' } else { 'black' }

        # Rotate through start positions, curated openings, and FEN roots. This
        # gives the campaign deterministic non-start coverage while retaining a
        # meaningful start-position sample.
        $sourceKind = 'startpos'
        $sourceEntry = $null
        $sourceSlot = $index % 4
        if ($sourceSlot -eq 1 -and $OpeningEntries.Count -gt 0) {
            $sourceKind = 'opening'
            $sourceEntry = $OpeningEntries[(($index * 17) + $ScheduleSeed) % $OpeningEntries.Count]
        } elseif ($sourceSlot -eq 2 -and $FenEntries.Count -gt 0) {
            $sourceKind = 'fen'
            $sourceEntry = $FenEntries[(($index * 13) + $ScheduleSeed) % $FenEntries.Count]
        } elseif ($sourceSlot -eq 3 -and $OpeningEntries.Count -gt 0) {
            $sourceKind = 'opening'
            $sourceEntry = $OpeningEntries[(($index * 19) + $ScheduleSeed) % $OpeningEntries.Count]
        }

        $bookEnabled = $bookRequested -and $Allocation.book_available
        $bookStatus = if (-not $bookRequested) {
            'not_requested'
        } elseif ($bookEnabled) {
            'enabled'
        } else {
            'book_unavailable'
        }
        $schedule.Add([ordered]@{
                ordinal = $ordinal
                group = $group
                opponent = $opponentKind
                koi_color = $koiColor
                threads = $threadCycle[$index % $threadCycle.Count]
                source_kind = $sourceKind
                source_name = if ($null -eq $sourceEntry) { 'startpos' } else { $sourceEntry.name }
                source_text = if ($null -eq $sourceEntry) { $null } elseif ($sourceKind -eq 'opening') { $sourceEntry.line } else { $sourceEntry.line }
                book_requested = $bookRequested
                book_enabled = $bookEnabled
                book_status = $bookStatus
                book_path = if ($bookEnabled) { $BookResolvedPath } else { $null }
                game_directory = ('game-{0:D4}' -f $ordinal)
            })
    }
    return @($schedule)
}

if ($Games -lt 1) { throw 'Games must be at least 1.' }
if ($MaxMoves -lt 1) { throw 'MaxMoves must be at least 1.' }
if ($Hash -lt 1 -or $Hash -gt 4096) { throw 'Hash must be between 1 and 4096 MB.' }
if ($Speed -lt 1 -or $Speed -gt 100) { throw 'Speed must be between 1 and 100.' }

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$stabilityScript = Join-Path $repositoryRoot 'tools\cutechess_stability.ps1'
$stabilityScript = Assert-File $stabilityScript 'Stability harness'
$koi = Assert-File $KoiPath 'Koi executable'
$stockfish = Assert-File $StockfishPath 'Stockfish executable'
$cutechess = Assert-File $CutechessPath 'Cutechess executable'

if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path 'C:\Koi-results\stability' ('campaign-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
}
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$campaignRoot = (Resolve-Path -LiteralPath $OutputDirectory).Path
$inputsRoot = Join-Path $campaignRoot 'inputs'
New-Item -ItemType Directory -Path $inputsRoot -Force | Out-Null

$opening = Get-SourcePath $repositoryRoot $OpeningFile 'tests\data\elo-openings-32.txt'
$fen = Get-SourcePath $repositoryRoot $FenFile 'tests\data\evaluation-positions.txt'
$openingEntries = Read-OpeningEntries $opening
$fenEntries = Read-FenEntries $fen
$bookResolved = if ([System.IO.Path]::IsPathRooted($BookFile)) {
    $BookFile
} else {
    Join-Path (Split-Path -Parent $koi) $BookFile
}
$bookAvailable = Test-Path -LiteralPath $bookResolved -PathType Leaf
$allocation = New-Allocation $Games $bookAvailable
$schedule = New-Schedule $Games $Seed $allocation $openingEntries $fenEntries $bookResolved

$manifestPath = Join-Path $campaignRoot 'campaign-manifest.json'
$schedulePath = Join-Path $campaignRoot 'schedule.json'
$gamesPath = Join-Path $campaignRoot 'games.jsonl'
$reportPath = Join-Path $campaignRoot 'campaign-report.json'
$failurePath = Join-Path $campaignRoot 'campaign-failure.json'
$campaignStartedUtc = (Get-Date).ToUniversalTime().ToString('o')
$sourceProvenance = @(
    [ordered]@{ kind = 'opening'; path = $opening; sha256 = Get-Hash $opening },
    [ordered]@{ kind = 'fen'; path = $fen; sha256 = Get-Hash $fen }
)
$executableProvenance = @(
    [ordered]@{ name = 'Koi'; path = $koi; sha256 = Get-Hash $koi },
    [ordered]@{ name = 'Stockfish 19'; path = $stockfish; sha256 = Get-Hash $stockfish },
    [ordered]@{ name = 'Cutechess'; path = $cutechess; sha256 = Get-Hash $cutechess }
)
$manifest = [ordered]@{
    schema = 'koi-cutechess-stability-campaign-v1'
    status = if ($PlanOnly) { 'planned' } else { 'running' }
    started_utc = $campaignStartedUtc
    seed = $Seed
    configuration = [ordered]@{
        games = $Games
        max_moves = $MaxMoves
        time_control = $TimeControl
        hash_mb = $Hash
        speed = $Speed
        book_file = $BookFile
        book_resolved_path = $bookResolved
        book_available = $bookAvailable
        book_depth = $BookDepth
        game_timeout_milliseconds = $GameTimeoutMilliseconds
        diagnostic_mode = if ($EnableDebug) { 'koi-file-only' } else { 'disabled' }
    }
    allocation = $allocation
    executables = $executableProvenance
    inputs = $sourceProvenance
    artifacts = [ordered]@{
        manifest = $manifestPath
        schedule = $schedulePath
        games = $gamesPath
        report = $reportPath
        failure = $failurePath
    }
}
Write-JsonFile $schedulePath ([ordered]@{
        schema = 'koi-cutechess-stability-schedule-v1'
        seed = $Seed
        generated_utc = $campaignStartedUtc
        allocation = $allocation
        games = $schedule
    })
Write-JsonFile $manifestPath $manifest
if (-not (Test-Path -LiteralPath $gamesPath -PathType Leaf)) {
    New-Item -ItemType File -Path $gamesPath -Force | Out-Null
}

$records = [System.Collections.Generic.List[object]]::new()
$failed = $false
$failureRecord = $null

if (-not $PlanOnly) {
    foreach ($item in $schedule) {
        $gameDirectory = Join-Path $campaignRoot $item.game_directory
        New-Item -ItemType Directory -Path $gameDirectory -Force | Out-Null
        $sourcePath = $null
        if ($item.source_kind -eq 'opening') {
            $sourcePath = Join-Path $inputsRoot ('{0:D4}-opening.txt' -f $item.ordinal)
            Write-Utf8File $sourcePath ($item.source_text + [Environment]::NewLine)
        } elseif ($item.source_kind -eq 'fen') {
            $sourcePath = Join-Path $inputsRoot ('{0:D4}-fen.txt' -f $item.ordinal)
            Write-Utf8File $sourcePath ($item.source_text + [Environment]::NewLine)
        }

        $opponent = if ($item.group -eq 'koi_koi') { $koi } else { $stockfish }
        $harnessParameters = @{
            KoiPath = $koi
            OpponentPath = $opponent
            CutechessPath = $cutechess
            OutputDirectory = $gameDirectory
            Games = 1
            MaxMoves = $MaxMoves
            TimeControl = $TimeControl
            Hash = $Hash
            Threads = $item.threads
            Speed = $Speed
            OwnBook = [bool]$item.book_enabled
            BookFile = $BookFile
            BookDepth = $BookDepth
            KoiColor = $item.koi_color
            TimeoutMilliseconds = $GameTimeoutMilliseconds
        }
        if ($null -ne $sourcePath -and $item.source_kind -eq 'opening') {
            $harnessParameters.OpeningFile = $sourcePath
        } elseif ($null -ne $sourcePath -and $item.source_kind -eq 'fen') {
            $harnessParameters.FenFile = $sourcePath
        }
        if ($EnableDebug) {
            $harnessParameters.EnableDebug = $true
        }

        $runOutputPath = Join-Path $gameDirectory 'campaign-run-output.log'
        $runOutput = @(& $stabilityScript @harnessParameters 2>&1 |
            Tee-Object -FilePath $runOutputPath)
        $runExitCode = $LASTEXITCODE
        $gameReportPath = Join-Path $gameDirectory 'stability-report.json'
        $gameReport = $null
        if (Test-Path -LiteralPath $gameReportPath -PathType Leaf) {
            try { $gameReport = Get-Content -LiteralPath $gameReportPath -Raw | ConvertFrom-Json } catch { }
        }

        $diagnosticViolations = [System.Collections.Generic.List[string]]::new()
        $debugSources = @(
            [pscustomobject]@{ label = 'koi'; path = (Join-Path $gameDirectory 'koi-debug.jsonl') }
            [pscustomobject]@{ label = 'opponent'; path = (Join-Path $gameDirectory 'opponent-debug.jsonl') }
        )
        foreach ($debugSource in $debugSources) {
            if (Test-Path -LiteralPath $debugSource.path -PathType Leaf) {
                foreach ($line in Get-Content -LiteralPath $debugSource.path) {
                    try {
                        $record = $line | ConvertFrom-Json
                        if ($record.event -eq 'completion_validation') {
                            if ($record.fallback_used -eq $true) { $diagnosticViolations.Add("$($debugSource.label):completion_fallback") }
                            if ($record.disposition -in @('quarantine', 'fallback')) { $diagnosticViolations.Add("$($debugSource.label):completion_$($record.disposition)") }
                            if ($record.root_key_match -eq $false -or $record.identity_match -eq $false) { $diagnosticViolations.Add("$($debugSource.label):completion_identity_mismatch") }
                        }
                        if ($record.native_shadow_consistent -eq $false) { $diagnosticViolations.Add("$($debugSource.label):native_shadow_mismatch") }
                    } catch {
                        $diagnosticViolations.Add("$($debugSource.label):malformed_debug_record")
                    }
                }
            }
        }
        $diagnosticViolations = @($diagnosticViolations | Sort-Object -Unique)
        $runFailure = $runExitCode -ne 0 -or $null -eq $gameReport -or
            $gameReport.termination_classification -ne 'completed' -or
            $gameReport.results.started_games -ne 1 -or
            $gameReport.results.finished_games -ne 1 -or
            $gameReport.results.failures -ne 0 -or
            $diagnosticViolations.Count -ne 0
        $gameRecord = [ordered]@{
            ordinal = $item.ordinal
            group = $item.group
            opponent = $item.opponent
            koi_color = $item.koi_color
            threads = $item.threads
            source_kind = $item.source_kind
            source_name = $item.source_name
            book_status = $item.book_status
            run_exit_code = $runExitCode
            termination = if ($null -eq $gameReport) { 'missing-report' } else { $gameReport.termination_classification }
            started_games = if ($null -eq $gameReport) { 0 } else { $gameReport.results.started_games }
            finished_games = if ($null -eq $gameReport) { 0 } else { $gameReport.results.finished_games }
            harness_failures = if ($null -eq $gameReport) { 1 } else { $gameReport.results.failures }
            diagnostic_violations = @($diagnosticViolations)
            source_path = $sourcePath
            artifact_directory = $gameDirectory
            report = $gameReportPath
            failure = $runFailure
        }
        $records.Add($gameRecord)
        Add-JsonLine $gamesPath $gameRecord
        $completedCount = $records.Count
        $failedCount = @($records | Where-Object { $_.failure }).Count
        $manifest.status = if ($runFailure) { 'failed' } else { 'running' }
        $manifest.completed_games = $completedCount
        $manifest.failed_games = $failedCount
        $manifest.generated_utc = (Get-Date).ToUniversalTime().ToString('o')
        Write-JsonFile $manifestPath $manifest
        Write-JsonFile $reportPath ([ordered]@{
                schema = 'koi-cutechess-stability-campaign-report-v1'
                status = if ($runFailure) { 'failed' } else { 'running' }
                requested_games = $Games
                completed_games = $completedCount
                failed_games = $failedCount
                records = @($records)
            })
        Write-Output ("game {0}/{1} group={2} source={3} threads={4} result={5} failures={6}" -f
            $item.ordinal, $Games, $item.group, $item.source_kind, $item.threads, $gameRecord.termination, $gameRecord.failure)
        if ($runFailure) {
            $failed = $true
            $failureRecord = [ordered]@{
                schema = 'koi-cutechess-stability-campaign-failure-v1'
                failed_game = $gameRecord
                schedule_entry = $item
                run_output = $runOutput
                generated_utc = (Get-Date).ToUniversalTime().ToString('o')
            }
            Write-JsonFile $failurePath $failureRecord
            break
        }
    }
}

$manifest.status = if ($PlanOnly) { 'planned' } elseif ($failed) { 'failed' } elseif ($records.Count -eq $Games) { 'completed' } else { 'interrupted' }
$manifest.completed_games = $records.Count
$manifest.failed_games = @($records | Where-Object { $_.failure }).Count
$manifest.generated_utc = (Get-Date).ToUniversalTime().ToString('o')
Write-JsonFile $manifestPath $manifest
Write-JsonFile $reportPath ([ordered]@{
        schema = 'koi-cutechess-stability-campaign-report-v1'
        status = $manifest.status
        requested_games = $Games
        completed_games = $records.Count
        failed_games = @($records | Where-Object { $_.failure }).Count
        allocation = $allocation
        artifacts = $manifest.artifacts
        records = @($records)
    })

Write-Output "schedule $schedulePath"
Write-Output "report $reportPath"
Write-Output "campaign status=$($manifest.status) completed=$($records.Count) requested=$Games failures=$(@($records | Where-Object { $_.failure }).Count)"
if ($failed) { exit 1 }
if ($PlanOnly) { exit 0 }
if ($records.Count -ne $Games) { exit 1 }
exit 0
