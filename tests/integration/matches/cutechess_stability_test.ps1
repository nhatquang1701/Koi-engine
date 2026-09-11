param(
    [Parameter(Mandatory = $true)]
    [string]$EnginePath,
    [string]$CutechessPath = 'C:\Program Files (x86)\Cute Chess\cutechess-cli.exe',
    [string]$OpponentPath = '',
    [string]$StabilityScript = ''
)

$ErrorActionPreference = 'Stop'

$repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..'))
if ([string]::IsNullOrWhiteSpace($OpponentPath)) {
    $OpponentPath = Join-Path $repositoryRoot 'third_party\stockfish-19\stockfish-windows-x86-64-universal\stockfish\stockfish-windows-x86-64-universal.exe'
}
if ([string]::IsNullOrWhiteSpace($StabilityScript)) {
    $StabilityScript = Join-Path $repositoryRoot 'tools\stability\cutechess_stability.ps1'
}
$CampaignScript = Join-Path $repositoryRoot 'tools\stability\cutechess_stability_campaign.ps1'
if (-not (Test-Path -LiteralPath $StabilityScript -PathType Leaf)) {
    throw "Cutechess stability script is missing: $StabilityScript"
}
if (-not (Test-Path -LiteralPath $CampaignScript -PathType Leaf)) {
    throw "Cutechess stability campaign script is missing: $CampaignScript"
}

function New-FakeCutechess([string]$Path, [ValidateSet('success', 'one', 'crash', 'disconnect', 'no-result', 'timeout', 'time-forfeit')][string]$Mode) {
    $lines = switch ($Mode) {
        'success' {
            @(
                '@echo off',
                'echo Started game 1 (Koi vs Opponent)',
                'echo Finished game 1 (Koi vs Opponent)',
                'echo Started game 2 (Opponent vs Koi)',
                'echo Finished game 2 (Opponent vs Koi)',
                'exit /b 0'
            )
        }
        'one' {
            @(
                '@echo off',
                'echo Started game 1 (Koi vs Opponent)',
                'echo Finished game 1 (Koi vs Opponent)',
                'exit /b 0'
            )
        }
        'crash' {
            @(
                '@echo off',
                'echo Started game 1 (Koi vs Opponent)',
                'echo partial stderr 1>&2',
                'echo partial stdout',
                'exit /b 1'
            )
        }
        'disconnect' {
            @(
                '@echo off',
                'echo Started game 1 (Koi vs Opponent)',
                'echo engine disconnect',
                'exit /b 1'
            )
        }
        'no-result' {
            @(
                '@echo off',
                'echo no-result stderr 1>&2',
                'exit /b 0'
            )
        }
        'timeout' {
            @(
                '@echo off',
                'echo Started game 1 (Koi vs Opponent)',
                'echo timeout stderr 1>&2',
                '%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe -NoProfile -Command "Start-Sleep -Seconds 10"',
                'exit /b 0'
            )
        }
        'time-forfeit' {
            @(
                '@echo off',
                'echo Started game 1 (Koi vs Opponent)',
                'echo Finished game 1 (Koi vs Opponent): 0-1 {White loses on time}',
                'exit /b 0'
            )
        }
    }
    Set-Content -LiteralPath $Path -Value $lines -Encoding ASCII
}

function Get-ReportPath([object[]]$Output) {
    $path = @($Output | Where-Object { $_ -like 'report *' } |
        ForEach-Object { $_.Substring(7) })[0]
    if ([string]::IsNullOrWhiteSpace($path) -or
        -not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw 'Cutechess stability run must report a JSON artifact path.'
    }
    return $path
}

function Assert-DiagnosticHarnessArtifacts {
    $fixtureRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("koi-cutechess-stability-fixture-" + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $fixtureRoot -Force | Out-Null
    try {
        $fixtureKoi = Join-Path $fixtureRoot 'koi.exe'
        $fixtureOpponent = Join-Path $fixtureRoot 'opponent.exe'
        $fixtureOpening = Join-Path $fixtureRoot 'opening.pgn'
        $fixtureFen = Join-Path $fixtureRoot 'positions.fen'
        $fakeCutechess = Join-Path $fixtureRoot 'fake-cutechess.cmd'
        Set-Content -LiteralPath $fixtureKoi -Value 'fixture-koi' -Encoding ASCII
        Set-Content -LiteralPath $fixtureOpponent -Value 'fixture-opponent' -Encoding ASCII
        Set-Content -LiteralPath $fixtureOpening -Value '[Event "fixture"]' -Encoding ASCII
        Set-Content -LiteralPath $fixtureFen -Value 'start | rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1' -Encoding ASCII

        New-FakeCutechess $fakeCutechess 'success'
        $successDirectory = Join-Path $fixtureRoot 'success'
        $successOutput = @(& $StabilityScript -KoiPath $fixtureKoi -OpponentPath $fixtureOpponent `
            -CutechessPath $fakeCutechess -OpeningFile $fixtureOpening -OutputDirectory $successDirectory `
            -Games 2 -MaxMoves 4 -TimeControl '1+0' -Hash 16 -Threads 1 -Speed 100 `
            -OwnBook:$false -TimeoutMilliseconds 5000)
        if ($LASTEXITCODE -ne 0) {
            throw "Fixture stability run exited with ${LASTEXITCODE}: $($successOutput -join ' | ')"
        }
        $successReportPath = Get-ReportPath $successOutput
        $successReport = Get-Content -LiteralPath $successReportPath -Raw | ConvertFrom-Json
        $expectedKoiHash = (Get-FileHash -LiteralPath $fixtureKoi -Algorithm SHA256).Hash
        $expectedOpponentHash = (Get-FileHash -LiteralPath $fixtureOpponent -Algorithm SHA256).Hash
        $expectedCutechessHash = (Get-FileHash -LiteralPath $fakeCutechess -Algorithm SHA256).Hash
        $expectedOpeningHash = (Get-FileHash -LiteralPath $fixtureOpening -Algorithm SHA256).Hash
        $provenance = @($successReport.provenance.executables)
        $inputs = @($successReport.provenance.inputs)
        if ($provenance.Count -ne 3 -or
            (@($provenance | Where-Object { $_.name -eq 'Koi' -and $_.path -eq (Resolve-Path $fixtureKoi).Path -and $_.sha256 -eq $expectedKoiHash }).Count -ne 1) -or
            (@($provenance | Where-Object { $_.name -eq 'Opponent' -and $_.path -eq (Resolve-Path $fixtureOpponent).Path -and $_.sha256 -eq $expectedOpponentHash }).Count -ne 1) -or
            (@($provenance | Where-Object { $_.name -eq 'Cutechess' -and $_.path -eq (Resolve-Path $fakeCutechess).Path -and $_.sha256 -eq $expectedCutechessHash }).Count -ne 1) -or
            (@($inputs | Where-Object { $_.kind -eq 'opening' -and $_.path -eq (Resolve-Path $fixtureOpening).Path -and $_.sha256 -eq $expectedOpeningHash }).Count -ne 1) -or
            $successReport.results.termination_classification -ne 'completed' -or
            [string]::IsNullOrWhiteSpace($successReport.configuration.command) -or
            $successReport.system.os -eq $null -or $successReport.system.cpu -eq $null) {
            throw 'Stability report must contain absolute executable/input provenance, command, system data, and completion classification.'
        }
        $requiredArtifacts = @(
            'manifest', 'cutechess_command', 'cutechess_version', 'stdout', 'stderr',
            'koi_stdout', 'koi_stderr', 'opponent_stdout', 'opponent_stderr',
            'protocol_events', 'per_ply_positions', 'koi_debug', 'failure', 'exit_status'
        )
        foreach ($artifactName in $requiredArtifacts) {
            $artifactPath = $successReport.artifacts.$artifactName
            if ([string]::IsNullOrWhiteSpace($artifactPath) -or
                -not (Test-Path -LiteralPath $artifactPath -PathType Leaf)) {
                throw "Stability artifact '$artifactName' must be preserved for a completed run."
            }
        }
        $commandArtifact = Get-Content -LiteralPath $successReport.artifacts.cutechess_command -Raw | ConvertFrom-Json
        if ($commandArtifact.global_debug -ne $false -or
            (@($commandArtifact.arguments | Where-Object { $_ -ceq '-debug' }).Count -ne 0)) {
            throw 'The stability harness must not use Cutechess global -debug.'
        }
        foreach ($jsonLine in Get-Content -LiteralPath $successReport.artifacts.protocol_events) {
            if (-not [string]::IsNullOrWhiteSpace($jsonLine)) {
                $null = $jsonLine | ConvertFrom-Json
            }
        }

        New-FakeCutechess $fakeCutechess 'one'
        $oneDirectory = Join-Path $fixtureRoot 'one'
        $oneOutput = @(& $StabilityScript -KoiPath $fixtureKoi -OpponentPath $fixtureOpponent `
            -CutechessPath $fakeCutechess -OutputDirectory $oneDirectory -Games 1 -MaxMoves 4 `
            -TimeControl '1+0' -Hash 16 -Threads 1 -Speed 100 -OwnBook:$false -TimeoutMilliseconds 5000)
        if ($LASTEXITCODE -ne 0) {
            throw "An odd one-game run must allow the single Cutechess-assigned color: $($oneOutput -join ' | ')"
        }
        $oneReport = Get-Content -LiteralPath (Get-ReportPath $oneOutput) -Raw | ConvertFrom-Json
        if ($oneReport.results.started_games -ne 1 -or $oneReport.results.finished_games -ne 1 -or
            [math]::Abs([int]$oneReport.results.koi_white - [int]$oneReport.results.koi_black) -gt 1) {
            throw 'An odd one-game run must retain its actual color assignment without claiming imbalance.'
        }

        New-FakeCutechess $fakeCutechess 'success'
        $fenDirectory = Join-Path $fixtureRoot 'fen'
        $fenOutput = @(& $StabilityScript -KoiPath $fixtureKoi -OpponentPath $fixtureOpponent `
            -CutechessPath $fakeCutechess -FenFile $fixtureFen -OutputDirectory $fenDirectory `
            -Games 2 -MaxMoves 1 -TimeControl '1+0' -Hash 16 -Threads 1 -Speed 100 `
            -OwnBook:$false -TimeoutMilliseconds 5000)
        if ($LASTEXITCODE -ne 0) {
            throw "FEN fixture run exited with ${LASTEXITCODE}: $($fenOutput -join ' | ')"
        }
        $fenReport = Get-Content -LiteralPath (Get-ReportPath $fenOutput) -Raw | ConvertFrom-Json
        $expectedFenHash = (Get-FileHash -LiteralPath $fixtureFen -Algorithm SHA256).Hash
        if (@($fenReport.provenance.inputs | Where-Object {
                $_.kind -eq 'fen' -and $_.path -eq (Resolve-Path $fixtureFen).Path -and $_.sha256 -eq $expectedFenHash
            }).Count -ne 1 -or $fenReport.configuration.converted_opening_file -notlike '*.epd') {
            throw 'FEN provenance and converted opening artifacts must be recorded.'
        }

        $campaignPlanDirectory = Join-Path $fixtureRoot 'campaign-plan'
        $campaignOutput = @(& $CampaignScript -KoiPath $fixtureKoi -StockfishPath $fixtureOpponent `
            -CutechessPath $fakeCutechess -OutputDirectory $campaignPlanDirectory -Games 12 -PlanOnly)
        if ($LASTEXITCODE -ne 0) {
            throw "Campaign plan exited with ${LASTEXITCODE}: $($campaignOutput -join ' | ')"
        }
        $campaignReportPath = Join-Path $campaignPlanDirectory 'campaign-report.json'
        $campaignSchedulePath = Join-Path $campaignPlanDirectory 'schedule.json'
        if (-not (Test-Path -LiteralPath $campaignReportPath -PathType Leaf) -or
            -not (Test-Path -LiteralPath $campaignSchedulePath -PathType Leaf)) {
            throw 'Campaign plan must preserve its report and deterministic schedule.'
        }
        $campaignReport = Get-Content -LiteralPath $campaignReportPath -Raw | ConvertFrom-Json
        $campaignSchedule = Get-Content -LiteralPath $campaignSchedulePath -Raw | ConvertFrom-Json
        if ($campaignReport.status -ne 'planned' -or
            $campaignReport.requested_games -ne 12 -or
            @($campaignSchedule.games).Count -ne 12 -or
            $campaignSchedule.allocation.stockfish_games -ne 9 -or
            $campaignSchedule.allocation.koi_koi_games -ne 1 -or
            $campaignSchedule.allocation.book_games -ne 2 -or
            @($campaignSchedule.games | Where-Object { $_.book_status -eq 'book_unavailable' }).Count -ne 2 -or
            @($campaignSchedule.games | Where-Object { $_.source_kind -eq 'opening' }).Count -lt 2 -or
            @($campaignSchedule.games | Where-Object { $_.source_kind -eq 'fen' }).Count -lt 2 -or
            @($campaignSchedule.games | Where-Object { $_.threads -eq 1 }).Count -ne 4 -or
            @($campaignSchedule.games | Where-Object { $_.threads -eq 2 }).Count -ne 4 -or
            @($campaignSchedule.games | Where-Object { $_.threads -eq 4 }).Count -ne 4) {
            throw 'Campaign plan must allocate pairings, substitutions, sources, and threads deterministically.'
        }

        New-FakeCutechess $fakeCutechess 'crash'
        $crashDirectory = Join-Path $fixtureRoot 'crash'
        $crashOutput = @(& $StabilityScript -KoiPath $fixtureKoi -OpponentPath $fixtureOpponent `
            -CutechessPath $fakeCutechess -OutputDirectory $crashDirectory -Games 2 `
            -MaxMoves 4 -TimeControl '1+0' -Hash 16 -Threads 1 -Speed 100 `
            -OwnBook:$false -TimeoutMilliseconds 5000)
        if ($LASTEXITCODE -eq 0) {
            throw 'A crashing Cutechess process must fail the stability run.'
        }
        $crashReportPath = Get-ReportPath $crashOutput
        $crashReport = Get-Content -LiteralPath $crashReportPath -Raw | ConvertFrom-Json
        $crashTranscript = Get-Content -LiteralPath $crashReport.artifacts.transcript -Raw
        if ($crashReport.results.termination_classification -ne 'crash' -or
            $crashTranscript -notlike '*partial stdout*' -or
            $crashTranscript -notlike '*partial stderr*' -or
            -not (Test-Path -LiteralPath $crashReport.artifacts.stdout -PathType Leaf) -or
            -not (Test-Path -LiteralPath $crashReport.artifacts.stderr -PathType Leaf)) {
            throw 'A crash must preserve partial stdout/stderr and classify the termination.'
        }

        New-FakeCutechess $fakeCutechess 'disconnect'
        $disconnectDirectory = Join-Path $fixtureRoot 'disconnect'
        $disconnectOutput = @(& $StabilityScript -KoiPath $fixtureKoi -OpponentPath $fixtureOpponent `
            -CutechessPath $fakeCutechess -OutputDirectory $disconnectDirectory -Games 2 `
            -MaxMoves 4 -TimeControl '1+0' -Hash 16 -Threads 1 -Speed 100 `
            -OwnBook:$false -TimeoutMilliseconds 5000)
        if ($LASTEXITCODE -eq 0 -or
            (Get-Content -LiteralPath (Get-ReportPath $disconnectOutput) -Raw | ConvertFrom-Json).results.termination_classification -ne 'disconnect') {
            throw 'An engine disconnect must be classified and fail the stability run.'
        }

        New-FakeCutechess $fakeCutechess 'no-result'
        $noResultDirectory = Join-Path $fixtureRoot 'no-result'
        $noResultOutput = @(& $StabilityScript -KoiPath $fixtureKoi -OpponentPath $fixtureOpponent `
            -CutechessPath $fakeCutechess -OutputDirectory $noResultDirectory -Games 2 `
            -MaxMoves 4 -TimeControl '1+0' -Hash 16 -Threads 1 -Speed 100 `
            -OwnBook:$false -TimeoutMilliseconds 5000)
        $noResultReport = Get-Content -LiteralPath (Get-ReportPath $noResultOutput) -Raw | ConvertFrom-Json
        if ($LASTEXITCODE -eq 0 -or $noResultReport.results.termination_classification -ne 'no-result' -or
            (Get-Content -LiteralPath $noResultReport.artifacts.transcript -Raw) -notlike '*no-result stderr*') {
            throw 'A no-result run must preserve its partial transcript and classify the termination.'
        }

        New-FakeCutechess $fakeCutechess 'timeout'
        $timeoutDirectory = Join-Path $fixtureRoot 'timeout'
        $timeoutOutput = @(& $StabilityScript -KoiPath $fixtureKoi -OpponentPath $fixtureOpponent `
            -CutechessPath $fakeCutechess -OutputDirectory $timeoutDirectory -Games 2 `
            -MaxMoves 4 -TimeControl '1+0' -Hash 16 -Threads 1 -Speed 100 `
            -OwnBook:$false -TimeoutMilliseconds 100)
        if ($LASTEXITCODE -eq 0) {
            throw 'A timed-out Cutechess process must fail the stability run.'
        }
        $timeoutReportPath = Get-ReportPath $timeoutOutput
        $timeoutReport = Get-Content -LiteralPath $timeoutReportPath -Raw | ConvertFrom-Json
        if ($timeoutReport.results.termination_classification -ne 'timeout' -or
            (Get-Content -LiteralPath $timeoutReport.artifacts.transcript -Raw) -notlike '*Started game 1*') {
            throw 'A timeout must preserve the partial transcript and classify the termination.'
        }

        New-FakeCutechess $fakeCutechess 'time-forfeit'
        $timeForfeitDirectory = Join-Path $fixtureRoot 'time-forfeit'
        $timeForfeitOutput = @(& $StabilityScript -KoiPath $fixtureKoi -OpponentPath $fixtureOpponent `
            -CutechessPath $fakeCutechess -OutputDirectory $timeForfeitDirectory -Games 1 `
            -MaxMoves 4 -TimeControl '1+0' -Hash 16 -Threads 1 -Speed 100 `
            -OwnBook:$false -TimeoutMilliseconds 5000)
        if ($LASTEXITCODE -eq 0) {
            throw 'A Cutechess loses-on-time result must fail the stability run.'
        }
        $timeForfeitReport = Get-Content -LiteralPath (Get-ReportPath $timeForfeitOutput) -Raw | ConvertFrom-Json
        if ($timeForfeitReport.results.failures -eq 0 -or
            $timeForfeitReport.results.termination_classification -ne 'incomplete' -or
            (@($timeForfeitReport.results.failure_lines | Where-Object { $_ -match '(?i)loses on time' }).Count -eq 0)) {
            throw 'A Cutechess loses-on-time result must be recorded as a stability failure.'
        }
    }
    finally {
        if (Test-Path -LiteralPath $fixtureRoot) {
            Remove-Item -LiteralPath $fixtureRoot -Recurse -Force
        }
    }
}

Assert-DiagnosticHarnessArtifacts

if (-not (Test-Path -LiteralPath $CutechessPath -PathType Leaf)) {
    Write-Output "SKIP Cutechess is not installed: $CutechessPath"
    exit 0
}
if (-not (Test-Path -LiteralPath $OpponentPath -PathType Leaf)) {
    Write-Output "SKIP the configured opponent is not installed: $OpponentPath"
    exit 0
}
if (-not (Test-Path -LiteralPath $EnginePath -PathType Leaf)) {
    Write-Output "SKIP Koi engine is not installed: $EnginePath"
    exit 0
}

$outputDirectory = Join-Path ([System.IO.Path]::GetTempPath()) ("koi-cutechess-stability-test-" + [guid]::NewGuid().ToString('N'))
$fenFile = Join-Path $outputDirectory 'positions.fen'
try {
    $output = & $StabilityScript -KoiPath $EnginePath -OpponentPath $OpponentPath `
        -CutechessPath $CutechessPath -OutputDirectory $outputDirectory -Games 2 `
        -MaxMoves 4 -TimeControl '1+0' -Hash 16 -Threads 1 -Speed 100 `
        -OwnBook:$false
    if ($LASTEXITCODE -ne 0) {
        throw "Cutechess stability smoke exited with ${LASTEXITCODE}: $($output -join ' | ')"
    }

    $reportPath = Get-ReportPath $output

    $report = Get-Content -LiteralPath $reportPath -Raw | ConvertFrom-Json
    if ($report.schema -ne 'koi-cutechess-stability-v1' -or
        $report.configuration.games -ne 2 -or
        $report.results.finished_games -ne 2 -or
        $report.results.failures -ne 0 -or
        $report.results.koi_white -ne 1 -or
        $report.results.koi_black -ne 1) {
        throw 'Cutechess stability smoke must complete two balanced, failure-free games.'
    }

    New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
    Set-Content -LiteralPath $fenFile -Value @(
        '# A six-field FEN is converted to a Cutechess EPD opening.'
        'start | rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1'
    ) -Encoding UTF8
    $fenOutputDirectory = Join-Path $outputDirectory 'fen-run'
    $fenOutput = & $StabilityScript -KoiPath $EnginePath -OpponentPath $OpponentPath `
        -CutechessPath $CutechessPath -FenFile $fenFile -OutputDirectory $fenOutputDirectory `
        -Games 2 -MaxMoves 1 -TimeControl '1+0' -Hash 16 -Threads 1 -Speed 100 `
        -OwnBook:$false
    if ($LASTEXITCODE -ne 0) {
        throw "Cutechess FEN smoke exited with ${LASTEXITCODE}: $($fenOutput -join ' | ')"
    }
    $fenReportPath = @($fenOutput | Where-Object { $_ -like 'report *' } |
        ForEach-Object { $_.Substring(7) })[0]
    if ([string]::IsNullOrWhiteSpace($fenReportPath) -or
        -not (Test-Path -LiteralPath $fenReportPath -PathType Leaf)) {
        throw 'Cutechess FEN smoke must report a JSON artifact path.'
    }
    $fenReport = Get-Content -LiteralPath $fenReportPath -Raw | ConvertFrom-Json
    if ($fenReport.configuration.converted_opening_file -notlike '*.epd' -or
        $fenReport.results.started_games -ne 2 -or
        $fenReport.results.finished_games -ne 2 -or
        $fenReport.results.failures -ne 0) {
        throw 'Cutechess FEN smoke must convert and play two failure-free games.'
    }
}
finally {
    if (Test-Path -LiteralPath $outputDirectory) {
        Remove-Item -LiteralPath $outputDirectory -Recurse -Force
    }
}
