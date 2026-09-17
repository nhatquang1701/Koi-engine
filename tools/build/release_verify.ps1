param(
    [string]$OutputDirectory = '',
    [string]$CMakePath = 'cmake',
    [string]$Generator = 'Ninja',
    [string]$CxxCompiler = 'cl',
    [int]$BuildJobs = 2
)

$ErrorActionPreference = 'Stop'

$repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $repositoryRoot ('artifacts\verification\release-verify-' +
        [guid]::NewGuid().ToString('N'))
}
$verificationRoot = [System.IO.Path]::GetFullPath($OutputDirectory)
$artifactsRoot = [System.IO.Path]::GetFullPath((Join-Path $repositoryRoot 'artifacts\verification'))
$artifactsPrefix = $artifactsRoot.TrimEnd('\') + '\'
if ($verificationRoot -ieq $artifactsRoot -or -not $verificationRoot.StartsWith($artifactsPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Verification output must be under repository artifacts: $verificationRoot"
}
New-Item -ItemType Directory -Path $verificationRoot -Force | Out-Null

function Write-Log([string]$Path, [object[]]$Lines) {
    if ($Lines.Count -eq 0) {
        Set-Content -LiteralPath $Path -Value '' -Encoding UTF8
    } else {
        Set-Content -LiteralPath $Path -Value ($Lines | ForEach-Object { $_.ToString() }) -Encoding UTF8
    }
}

function Invoke-LoggedCommand([string]$FilePath, [string[]]$Arguments, [string]$LogPath) {
    $stderrPath = $LogPath + '.stderr'
    $lines = @(& $FilePath @Arguments 2> $stderrPath | ForEach-Object { $_.ToString() })
    $exitCode = $LASTEXITCODE
    $stderr = if (Test-Path -LiteralPath $stderrPath -PathType Leaf) {
        @(Get-Content -LiteralPath $stderrPath)
    } else {
        @()
    }
    Write-Log $LogPath (@($lines) + @($stderr))
    if ($exitCode -ne 0) {
        throw "Command failed with exit code $exitCode. See $LogPath"
    }
    return $lines
}

function Invoke-CapturedProcess([string]$FilePath, [string[]]$Arguments,
                                [string]$StdoutPath, [string]$StderrPath) {
    $stdout = @(& $FilePath @Arguments 2> $StderrPath | ForEach-Object { $_.ToString() })
    $exitCode = $LASTEXITCODE
    Write-Log $StdoutPath $stdout
    if ($exitCode -ne 0) {
        throw "Process failed with exit code $exitCode. See $StdoutPath and $StderrPath"
    }
    return $stdout
}

function Assert-BenchmarkRows([object[]]$Lines, [int]$Threads, [bool]$Timed) {
    if ($Lines.Count -lt 3 -or $Lines[0] -cne 'Koi benchmark' -or
        $Lines[1] -notmatch "^config threads $Threads speed 100 timed $([int]$Timed) hash cold$") {
        throw "Invalid benchmark header for Threads=$Threads Timed=$Timed"
    }
    $rows = @($Lines | Where-Object { $_ -like 'position *' })
    if ($rows.Count -ne 64) {
        throw "Expected 64 tactical benchmark rows, got $($rows.Count)"
    }
    foreach ($row in $rows) {
        $suffix = if ($Timed) { ' elapsed_ms [0-9]+ nps [0-9]+$' } else { '$' }
        if ($row -notmatch "^position [a-z0-9_-]+ depth [1-9][0-9]* nodes [0-9]+ qnodes [0-9]+ tt_hits [0-9]+ score -?[0-9]+ expected ([a-h][1-8][a-h][1-8][nbrq]?) move ([a-h][1-8][a-h][1-8][nbrq]?|0000) match 1$suffix") {
            throw "Invalid tactical benchmark row: $row"
        }
    }
    return $rows
}

function Get-NormalizedRows([object[]]$Rows) {
    return ($Rows | ForEach-Object {
        $_ -replace 'nodes [0-9]+ qnodes [0-9]+ tt_hits [0-9]+', 'nodes N qnodes Q tt_hits H'
    }) -join "`n"
}

function Assert-Profile([string]$Path, [int]$Threads, [bool]$Timed) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Missing benchmark profile: $Path"
    }
    $profile = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
    if ($profile.schema -cne 'koi-bench-profile-v1' -or $profile.threads -ne $Threads -or
        $profile.speed -ne 100 -or $profile.timed -ne $Timed -or
        $profile.hash_state -cne 'cold' -or $profile.positions.Count -ne 64) {
        throw "Invalid benchmark profile contract: $Path"
    }
    foreach ($position in $profile.positions) {
        if ($position.hash_state -cne $profile.hash_state) {
            throw "Profile position hash state differs from top-level state: $Path"
        }
        if ($Timed -and $null -eq $position.elapsed_ms) {
            throw "Timed profile position lacks elapsed_ms: $Path"
        }
        if (-not $Timed -and ($null -ne $position.elapsed_ms -or [uint64]$position.nps -ne 0)) {
            throw "Untimed profile contains measured timing: $Path"
        }
    }
    return $profile
}

function Invoke-UciSmoke([string]$EnginePath, [string]$StdoutPath, [string]$StderrPath) {
    $transcript = @('uci', 'isready', 'position startpos', 'go depth 2', 'stop', 'quit')
    $stdout = @($transcript | & $EnginePath 2> $StderrPath | ForEach-Object { $_.ToString() })
    $exitCode = $LASTEXITCODE
    Write-Log $StdoutPath $stdout
    if ($exitCode -ne 0) {
        throw "UCI smoke failed with exit code $exitCode"
    }
    if ((Get-Item -LiteralPath $StderrPath).Length -ne 0 -or $stdout -notcontains 'uciok' -or
        $stdout -notcontains 'readyok') {
        throw "UCI smoke handshake or stderr contract failed"
    }
    $bestmoves = @($stdout | Where-Object { $_ -match '^bestmove ' })
    if ($bestmoves.Count -ne 1 -or $bestmoves[0] -notmatch '^bestmove [a-h][1-8][a-h][1-8][nbrq]?$') {
        throw 'UCI smoke did not produce exactly one legal coordinate bestmove'
    }
    return $stdout
}

try {
    Push-Location $repositoryRoot

    $cmakeVersion = Invoke-LoggedCommand $CMakePath @('--version') (Join-Path $verificationRoot 'cmake-version.txt')
    $versionLine = $cmakeVersion | Where-Object { $_ -match '^cmake version ' } | Select-Object -First 1
    if ($null -eq $versionLine -or $versionLine -notmatch '^cmake version ([0-9]+)\.([0-9]+)') {
        throw 'Unable to determine CMake version'
    }
    $cmakeMajor = [int]$Matches[1]
    $cmakeMinor = [int]$Matches[2]
    if ($cmakeMajor -lt 3 -or ($cmakeMajor -eq 3 -and $cmakeMinor -lt 31)) {
        throw "CMake 3.31 or newer is required; found $versionLine"
    }
    $cmakeCommand = Get-Command $CMakePath -ErrorAction Stop
    $ctestPath = Join-Path (Split-Path -Parent $cmakeCommand.Source) 'ctest.exe'
    if (-not (Test-Path -LiteralPath $ctestPath -PathType Leaf)) {
        $ctestPath = 'ctest'
    }

    $debugBuild = Join-Path $repositoryRoot 'build\debug'
    $releaseBuild = Join-Path $repositoryRoot 'build\release'
    foreach ($configuration in @('Debug', 'Release')) {
        $build = if ($configuration -eq 'Debug') { $debugBuild } else { $releaseBuild }
        $configureLog = Join-Path $verificationRoot ("configure-$configuration.txt")
        $buildLog = Join-Path $verificationRoot ("build-$configuration.txt")
        $ctestLog = Join-Path $verificationRoot ("ctest-$configuration.txt")
        $configureArguments = @('-S', $repositoryRoot, '-B', $build, '-G', $Generator)
        if ($Generator -like 'Visual Studio*') {
            $configureArguments += @('-A', 'x64')
        } else {
            $configureArguments += "-DCMAKE_BUILD_TYPE=$configuration"
            if (-not [string]::IsNullOrWhiteSpace($CxxCompiler)) {
                $configureArguments += "-DCMAKE_CXX_COMPILER=$CxxCompiler"
            }
        }
        Invoke-LoggedCommand $CMakePath $configureArguments $configureLog | Out-Null
        $buildArguments = @('--build', $build, '--config', $configuration)
        if ($Generator -like 'Visual Studio*') {
            $buildArguments += @('--parallel', "$BuildJobs")
        } else {
            $buildArguments += @('--', "-j$BuildJobs")
        }
        Invoke-LoggedCommand $CMakePath $buildArguments $buildLog | Out-Null
        Invoke-LoggedCommand $ctestPath @('--test-dir', $build, '-C', $configuration, '--output-on-failure') $ctestLog | Out-Null
    }

    $releaseBin = $releaseBuild
    $releaseConfigurationDirectory = Join-Path $releaseBuild 'Release'
    if (Test-Path -LiteralPath $releaseConfigurationDirectory -PathType Container) {
        $releaseBin = $releaseConfigurationDirectory
    }
    $benchPath = Join-Path $releaseBin 'koi-bench.exe'
    $enginePath = Join-Path $releaseBin 'koi-engine.exe'
    $replayPath = Join-Path $releaseBin 'koi-replay.exe'
    foreach ($path in @($benchPath, $enginePath, $replayPath)) {
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "Release executable is missing: $path"
        }
    }

    $maximumThreads = [Math]::Max(1, [Math]::Min(64, [Environment]::ProcessorCount))
    $threadCounts = [System.Collections.Generic.List[int]]::new()
    foreach ($candidate in @(1, 2, 4)) {
        if ($candidate -le $maximumThreads -and -not $threadCounts.Contains($candidate)) {
            $threadCounts.Add($candidate)
        }
    }
    if ($threadCounts.Count -eq 0) {
        $threadCounts.Add(1)
    }
    $baselineRows = $null
    $threadSummaries = [System.Collections.Generic.List[string]]::new()
    foreach ($threads in $threadCounts) {
        $stdoutPath = Join-Path $verificationRoot ("bench-threads-$threads.txt")
        $stderrPath = Join-Path $verificationRoot ("bench-threads-$threads.stderr.txt")
        $profilePath = Join-Path $verificationRoot ("bench-threads-$threads.json")
        $lines = Invoke-CapturedProcess $benchPath @('--threads', "$threads", '--speed', '100',
            '--profile-json', $profilePath) $stdoutPath $stderrPath
        $rows = Assert-BenchmarkRows $lines $threads $false
        $profile = Assert-Profile $profilePath $threads $false
        $normalized = Get-NormalizedRows $rows
        if ($null -eq $baselineRows) {
            $baselineRows = $normalized
        } elseif ($normalized -cne $baselineRows) {
            throw "Threads $threads move/score rows differ from the Threads 1 reference"
        }
        $matches = @($rows | Where-Object { $_ -match ' match 1$' }).Count
        $threadSummaries.Add("Threads=$threads rows=$($rows.Count) matches=$matches profile_threads=$($profile.threads)")
    }
    if ($maximumThreads -lt 4) {
        $threadSummaries.Add("Threads=4 skipped; safe fallback used because maximum_threads=$maximumThreads")
    }

    $timedThreads = if ($maximumThreads -ge 4) { 4 } else { $maximumThreads }
    $timedStdoutPath = Join-Path $verificationRoot 'bench-timed.txt'
    $timedStderrPath = Join-Path $verificationRoot 'bench-timed.stderr.txt'
    $timedProfilePath = Join-Path $verificationRoot 'bench-timed.json'
    $timedLines = Invoke-CapturedProcess $benchPath @('--threads', "$timedThreads", '--speed', '100', '--timed',
        '--profile-json', $timedProfilePath) $timedStdoutPath $timedStderrPath
    $timedRows = Assert-BenchmarkRows $timedLines $timedThreads $true
    $timedProfile = Assert-Profile $timedProfilePath $timedThreads $true
    $threadSummaries.Add("Timed Threads=$timedThreads rows=$($timedRows.Count) profile_timed=$($timedProfile.timed)")

    $optionalStdoutPath = Join-Path $verificationRoot 'bench-optional.txt'
    $optionalStderrPath = Join-Path $verificationRoot 'bench-optional.stderr.txt'
    $optionalProfilePath = Join-Path $verificationRoot 'bench-optional.json'
    $optionalLines = Invoke-CapturedProcess $benchPath @('--optional', '--profile-json', $optionalProfilePath) $optionalStdoutPath $optionalStderrPath
    $optionalRows = @($optionalLines | Where-Object { $_ -like 'position *' })
    $optionalProfile = Get-Content -LiteralPath $optionalProfilePath -Raw | ConvertFrom-Json
    if ($optionalRows.Count -ne 128 -or $optionalProfile.positions.Count -ne 128 -or
        $optionalProfile.suite -cne 'optional_strength') {
        throw 'Optional benchmark did not produce its complete 128-position report'
    }
    $threadSummaries.Add("Optional rows=$($optionalRows.Count) profile_positions=$($optionalProfile.positions.Count)")

    $smokeLines = Invoke-UciSmoke $enginePath (Join-Path $verificationRoot 'uci-smoke.txt') (Join-Path $verificationRoot 'uci-smoke.stderr.txt')
    $replayStdoutPath = Join-Path $verificationRoot 'replay.txt'
    $replayStderrPath = Join-Path $verificationRoot 'replay.stderr.txt'
    $replayLines = Invoke-CapturedProcess $replayPath @('startpos', 'moves', 'g1f3', 'g8f6', 'f3g1', 'f6g8',
        'g1f3', 'g8f6', 'f3g1', 'f6g8') $replayStdoutPath $replayStderrPath
    if ($replayLines -notcontains 'legal 1' -or $replayLines -notcontains 'result 1/2-1/2' -or
        $replayLines -notcontains 'termination rule draw') {
        throw 'Replay report did not classify the legal repetition as a rule draw'
    }

    $enCroissantDirectory = Join-Path $verificationRoot 'en-croissant-style-24ply'
    New-Item -ItemType Directory -Path $enCroissantDirectory -Force | Out-Null
    $enCroissantThreads = if ($maximumThreads -ge 4) { 4 } else { $maximumThreads }
    $matchScript = Join-Path $repositoryRoot 'tools\stability\uci_match.ps1'
    # Use the current PowerShell host instead of a hardcoded powershell.exe so
    # the gate works on hosts that only ship pwsh.
    $matchOutput = Invoke-LoggedCommand (Get-Process -Id $PID).Path @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $matchScript,
        '-KoiPath', $enginePath, '-OpponentPath', $enginePath, '-ReplayPath', $replayPath,
        '-Depth', '2', '-Games', '1', '-MaxPlies', '24', '-TimeoutMilliseconds', '5000',
        '-Threads', "$enCroissantThreads", '-Speed', '100', '-Hash', '512', '-KoiOwnBook', 'false',
        '-OutputDirectory', $enCroissantDirectory) (Join-Path $verificationRoot 'en-croissant-style-command.txt')
    $enCroissantJson = @(Get-ChildItem -LiteralPath $enCroissantDirectory -Filter '*.json' -File)
    $enCroissantPgn = @(Get-ChildItem -LiteralPath $enCroissantDirectory -Filter '*.pgn' -File)
    if ($enCroissantJson.Count -ne 1 -or $enCroissantPgn.Count -ne 1) {
        throw 'En Croissant-style scenario did not produce exactly one JSON and PGN report'
    }
    $enCroissantReport = Get-Content -LiteralPath $enCroissantJson[0].FullName -Raw | ConvertFrom-Json
    $enCroissantGame = $enCroissantReport.games[0]
    if ($enCroissantReport.configuration.hash_mb -ne 512 -or $enCroissantReport.configuration.threads -ne $enCroissantThreads -or
        $enCroissantReport.configuration.speed -ne 100 -or $enCroissantGame.moves.Count -lt 20 -or
        @($enCroissantGame.moves | Where-Object { $_.replay_legal -ne $true }).Count -ne 0 -or
        $enCroissantGame.process_status.koi -ne 'clean shutdown' -or
        $enCroissantGame.process_status.opponent -ne 'clean shutdown') {
        throw 'En Croissant-style scenario did not preserve configuration, 20+ legal plies, and clean shutdown'
    }

    Write-Output "verification_root=$verificationRoot"
    Write-Output "cmake=$versionLine generator=$Generator compiler=$CxxCompiler"
    Write-Output 'Debug configure/build/CTest=PASS; Release configure/build/CTest=PASS'
    Write-Output ('UCI smoke lines=' + @($smokeLines).Count + ' bestmove=1 stderr=0')
    Write-Output ('Replay legal=1 result=1/2-1/2 termination=rule draw')
    foreach ($summary in $threadSummaries) {
        Write-Output $summary
    }
    Write-Output ("En Croissant-style plies=$($enCroissantGame.moves.Count) hash=$($enCroissantReport.configuration.hash_mb) threads=$($enCroissantReport.configuration.threads) speed=$($enCroissantReport.configuration.speed) replay_legal=all process_status=clean")
    Write-Output "En Croissant JSON=$($enCroissantJson[0].FullName)"
    Write-Output 'Stockfish CPL/match data: unavailable; no Elo claim'
    Write-Output 'En Croissant GUI: unavailable; manual GUI gate not claimed'
} finally {
    Pop-Location
}
