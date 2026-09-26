[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$BuildDirectory,
    [Parameter(Mandatory = $true)]
    [string]$PackageScript
)

$ErrorActionPreference = 'Stop'
$powerShellExecutable = (Get-Process -Id $PID).Path
$isWindowsHost = $env:OS -eq 'Windows_NT'
$platformTag = if ($isWindowsHost) { '' } else { '-linux-x86_64' }
$binarySuffix = if ($isWindowsHost) { '.exe' } else { '' }
$packageName = "koi-engine-v1.0.0$platformTag"
$archivePathName = if ($isWindowsHost) { "$packageName.zip" } else { "$packageName.tar.gz" }
$outputDirectory = Join-Path ([System.IO.Path]::GetTempPath()) ('koi-package-test-' + [guid]::NewGuid().ToString('N'))

function Get-TestSha256 {
    param([Parameter(Mandatory = $true)][string]$Path)

    $sha256 = [System.Security.Cryptography.SHA256]::Create()
    try {
        return ([System.BitConverter]::ToString($sha256.ComputeHash([System.IO.File]::ReadAllBytes($Path)))).Replace('-', '').ToLowerInvariant()
    } finally {
        $sha256.Dispose()
    }
}

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function Invoke-BoundedProcess {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [string[]]$Arguments = @(),
        [string[]]$InputLines = @(),
        [string]$CpuVariant = '',
        [int]$TimeoutMilliseconds = 15000
    )

    $startInfo = New-Object System.Diagnostics.ProcessStartInfo
    $startInfo.FileName = $FilePath
    $startInfo.Arguments = $Arguments -join ' '
    $startInfo.WorkingDirectory = Split-Path -Parent $FilePath
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardInput = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $startInfo.EnvironmentVariables.Remove('KOI_CPU_VARIANT')
    if (-not [string]::IsNullOrWhiteSpace($CpuVariant)) {
        $startInfo.EnvironmentVariables['KOI_CPU_VARIANT'] = $CpuVariant
    }

    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $startInfo
    try {
        if (-not $process.Start()) { throw "Could not start $FilePath" }
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        foreach ($line in $InputLines) { $process.StandardInput.WriteLine($line) }
        $process.StandardInput.Close()
        if (-not $process.WaitForExit($TimeoutMilliseconds)) {
            & taskkill.exe /PID $process.Id /T /F 2>$null | Out-Null
            if (-not $process.HasExited) { $process.Kill() }
            $process.WaitForExit()
            throw "$FilePath exceeded its $TimeoutMilliseconds ms timeout"
        }
        $process.WaitForExit()
        return [pscustomobject]@{
            ExitCode = $process.ExitCode
            StandardOutput = $stdoutTask.Result
            StandardError = $stderrTask.Result
        }
    } finally {
        $process.Dispose()
    }
}

try {
    & $powerShellExecutable -NoProfile -ExecutionPolicy Bypass -File $PackageScript `
        -BuildDirectory $BuildDirectory -OutputDirectory $outputDirectory | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "package script exited with $LASTEXITCODE" }

    $packageDirectory = Join-Path $outputDirectory $packageName
    $archivePath = Join-Path $outputDirectory $archivePathName
    $archiveHashPath = "$archivePath.sha256"
    $requiredExecutables = @("koi-engine$binarySuffix", "koi-engine-avx2$binarySuffix",
        "koi-engine-avx512$binarySuffix", "koi-bench$binarySuffix",
        "koi-replay$binarySuffix", "koi-perft$binarySuffix")
    $requiredFiles = @($requiredExecutables + @('README.md', 'LICENSE', 'INSTALL.txt', 'package.json',
        'licenses\chess-library-MIT.txt', 'licenses\fathom-MIT.txt'))
    foreach ($required in $requiredFiles) {
        Assert-True (Test-Path -LiteralPath (Join-Path $packageDirectory $required) -PathType Leaf) "package is missing $required"
    }
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $packageDirectory 'book.bin'))) 'package must not embed user-supplied book.bin'
    Assert-True (Test-Path -LiteralPath $archivePath -PathType Leaf) 'package archive was not produced'
    Assert-True (Test-Path -LiteralPath $archiveHashPath -PathType Leaf) 'archive SHA-256 sidecar was not produced'

    $manifestText = Get-Content -LiteralPath (Join-Path $packageDirectory 'package.json') -Raw
    $manifest = $manifestText | ConvertFrom-Json
    Assert-True ($manifest.schema -ceq 'koi-engine-package-v1') 'package manifest schema is incorrect'
    Assert-True ($manifest.version -ceq '1.0.0') 'package manifest version is not 1.0.0'
    Assert-True ($manifest.platform -ceq $(if ($isWindowsHost) { 'windows-x86_64' } else { 'linux-x86_64' })) 'manifest platform does not match the host'
    Assert-True ($manifest.build_configuration -ceq 'Release') 'manifest build configuration is not Release'
    Assert-True ($manifest.provenance.source_commit -match '^[0-9a-f]{40,64}$') 'manifest source commit is missing or invalid'
    $expectedCommit = $env:GITHUB_SHA
    if ($expectedCommit -notmatch '^[0-9a-f]{40,64}$') {
        $expectedCommit = (& git rev-parse HEAD).Trim()
    }
    Assert-True ($manifest.provenance.source_commit -ceq $expectedCommit) 'manifest source commit does not match the checkout'
    Assert-True ($manifestText -match '"created_utc"\s*:\s*"[^"]+Z"') 'manifest creation time is not UTC'
    Assert-True ($manifest.provenance.source_dirty -is [bool]) 'manifest source dirty flag is missing'
    Assert-True ($manifest.book_included -eq $false) 'manifest must state that the optional book is excluded'
    Assert-True ($manifest.tablebases_included -eq $false) 'manifest must state that tablebases are excluded'

    $manifestFiles = @($manifest.files)
    Assert-True ($manifestFiles.Count -eq ($requiredFiles.Count - 1)) 'manifest must checksum each payload file except package.json'
    $expectedManifestPaths = @($requiredFiles | Where-Object { $_ -cne 'package.json' } | ForEach-Object { $_.Replace('\', '/') } | Sort-Object)
    $actualManifestPaths = @($manifestFiles | ForEach-Object { [string]$_.path } | Sort-Object)
    Assert-True (($expectedManifestPaths -join "`n") -ceq ($actualManifestPaths -join "`n")) 'manifest file list does not match package payloads'
    foreach ($file in $manifestFiles) {
        $relativePath = $file.path.Replace('/', [System.IO.Path]::DirectorySeparatorChar)
        $payloadPath = Join-Path $packageDirectory $relativePath
        Assert-True (Test-Path -LiteralPath $payloadPath -PathType Leaf) "manifest references missing file $($file.path)"
        Assert-True ((Get-TestSha256 -Path $payloadPath) -ceq $file.sha256) "manifest SHA-256 does not match $($file.path)"
        Assert-True ((Get-Item -LiteralPath $payloadPath).Length -eq $file.size_bytes) "manifest size does not match $($file.path)"
    }

    $archiveHash = Get-TestSha256 -Path $archivePath
    $sidecar = (Get-Content -LiteralPath $archiveHashPath -Raw).Trim()
    Assert-True ($sidecar -match "^$archiveHash\s+\*?$([regex]::Escape($archivePathName))$") 'archive SHA-256 sidecar does not match the archive'

    $extractionDirectory = Join-Path $outputDirectory 'extracted'
    New-Item -ItemType Directory -Path $extractionDirectory -Force | Out-Null
    if ($isWindowsHost) {
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        [System.IO.Compression.ZipFile]::ExtractToDirectory($archivePath, $extractionDirectory)
    } else {
        & tar -xzf $archivePath -C $extractionDirectory
        if ($LASTEXITCODE -ne 0) { throw "tar extraction failed with exit code $LASTEXITCODE" }
    }

    foreach ($required in $requiredFiles) {
        Assert-True (Test-Path -LiteralPath (Join-Path $extractionDirectory $required) -PathType Leaf) "extracted archive is missing $required"
    }
    $replayPath = Join-Path $extractionDirectory "koi-replay$binarySuffix"
    $replayOutput = @(& $replayPath 'startpos' 'moves' 'g1f3' 'g8f6' 'f3g1' 'f6g8' 'g1f3' 'g8f6' 'f3g1' 'f6g8')
    if ($LASTEXITCODE -ne 0) { throw "extracted replay smoke failed with exit code $LASTEXITCODE" }
    Assert-True ($replayOutput -contains 'legal 1') 'extracted replay smoke did not report a legal position'
    Assert-True ($replayOutput -contains 'result 1/2-1/2') 'extracted replay smoke did not report the repetition draw'

    if ($isWindowsHost) {
        $enginePath = Join-Path $extractionDirectory 'koi-engine.exe'
        $replayPath = Join-Path $extractionDirectory 'koi-replay.exe'
        $uciTranscript = @('uci', 'isready', 'position startpos', 'go depth 2', 'stop', 'quit')
        foreach ($variant in @('', 'generic')) {
            $label = if ([string]::IsNullOrWhiteSpace($variant)) { 'automatic' } else { $variant }
            $smoke = Invoke-BoundedProcess -FilePath $enginePath -InputLines $uciTranscript -CpuVariant $variant
            Assert-True ($smoke.ExitCode -eq 0) "$label extracted engine exited with $($smoke.ExitCode)"
            Assert-True ([string]::IsNullOrWhiteSpace($smoke.StandardError)) "$label extracted engine wrote to stderr: $($smoke.StandardError)"
            $smokeLines = @($smoke.StandardOutput -split "`r?`n" | Where-Object { $_ -ne '' })
            Assert-True ($smokeLines -contains 'uciok') "$label extracted engine did not complete the UCI handshake"
            Assert-True ($smokeLines -contains 'readyok') "$label extracted engine did not complete the readiness handshake"
            $bestmoveLines = @($smokeLines | Where-Object { $_ -match '^bestmove\s+' })
            Assert-True ($bestmoveLines.Count -eq 1) "$label extracted engine produced $($bestmoveLines.Count) bestmove lines"
            Assert-True ($bestmoveLines[0] -match '^bestmove\s+([a-h][1-8][a-h][1-8][nbrq]?)$') "$label extracted engine produced an invalid bestmove line"
            $bestmove = $Matches[1]
            $replay = Invoke-BoundedProcess -FilePath $replayPath -Arguments @('startpos', 'moves', $bestmove) -TimeoutMilliseconds 5000
            Assert-True ($replay.ExitCode -eq 0) "$label extracted replay exited with $($replay.ExitCode)"
            Assert-True ([string]::IsNullOrWhiteSpace($replay.StandardError)) "$label extracted replay wrote to stderr: $($replay.StandardError)"
            Assert-True (@($replay.StandardOutput -split "`r?`n") -contains 'legal 1') "$label bestmove is not legal from startpos: $bestmove"
        }
    }

    Write-Output 'PASS v1.0.0 release package manifest, checksums, provenance, and extracted archive smoke'
} finally {
    if (Test-Path -LiteralPath $outputDirectory) {
        Remove-Item -LiteralPath $outputDirectory -Recurse -Force
    }
}
