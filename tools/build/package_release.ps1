[CmdletBinding()]
param(
    [string]$BuildDirectory = (Join-Path $PSScriptRoot '../../build/release'),
    [string]$OutputDirectory = (Join-Path $PSScriptRoot '../../artifacts/packages')
)

$ErrorActionPreference = 'Stop'
$repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$buildRoot = [System.IO.Path]::GetFullPath($BuildDirectory)
$outputRoot = [System.IO.Path]::GetFullPath($OutputDirectory)
$isWindowsHost = $env:OS -eq 'Windows_NT'
$binarySuffix = if ($isWindowsHost) { '.exe' } else { '' }
$platformTag = if ($isWindowsHost) { '' } else { '-linux-x86_64' }
$packageName = "koi-engine-v1.0.0$platformTag"
$packageDirectory = Join-Path $outputRoot $packageName
$archivePath = if ($isWindowsHost) {
    Join-Path $outputRoot "$packageName.zip"
} else {
    Join-Path $outputRoot "$packageName.tar.gz"
}

function Get-Sha256Hex {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $sha256 = [System.Security.Cryptography.SHA256]::Create()
    try {
        $bytes = [System.IO.File]::ReadAllBytes($Path)
        return ([System.BitConverter]::ToString($sha256.ComputeHash($bytes))).Replace('-', '').ToLowerInvariant()
    } finally {
        $sha256.Dispose()
    }
}

if (-not (Test-Path -LiteralPath $buildRoot -PathType Container)) {
    throw "Build directory does not exist: $buildRoot"
}
New-Item -ItemType Directory -Path $outputRoot -Force | Out-Null

$requiredExecutables = @("koi-engine$binarySuffix", "koi-engine-avx2$binarySuffix",
                         "koi-engine-avx512$binarySuffix", "koi-bench$binarySuffix",
                         "koi-replay$binarySuffix", "koi-perft$binarySuffix")
function Test-RequiredExecutables {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Directory
    )

    foreach ($name in $requiredExecutables) {
        if (-not (Test-Path -LiteralPath (Join-Path $Directory $name) -PathType Leaf)) {
            return $false
        }
    }
    return $true
}

$binaryDirectory = $buildRoot
if (-not (Test-RequiredExecutables -Directory $binaryDirectory)) {
    $multiConfigReleaseDirectory = Join-Path $buildRoot 'Release'
    if (-not (Test-RequiredExecutables -Directory $multiConfigReleaseDirectory)) {
        throw "Release executables are missing from '$buildRoot' or '$multiConfigReleaseDirectory'."
    }
    $binaryDirectory = [System.IO.Path]::GetFullPath($multiConfigReleaseDirectory)
}
$readmePath = Join-Path $repositoryRoot 'README.md'
if (-not (Test-Path -LiteralPath $readmePath -PathType Leaf)) {
    throw "README is missing: $readmePath"
}

# Only the exact generated package directory is replaced.  The resolved target
# is checked to stay below the explicitly selected output directory.
$outputPrefix = $outputRoot.TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
$resolvedPackage = [System.IO.Path]::GetFullPath($packageDirectory)
if (-not $resolvedPackage.StartsWith($outputPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to replace package outside the output directory: $resolvedPackage"
}
if (Test-Path -LiteralPath $packageDirectory) {
    Remove-Item -LiteralPath $packageDirectory -Recurse -Force
}
New-Item -ItemType Directory -Path $packageDirectory -Force | Out-Null

foreach ($name in $requiredExecutables) {
    Copy-Item -LiteralPath (Join-Path $binaryDirectory $name) -Destination (Join-Path $packageDirectory $name)
}
Copy-Item -LiteralPath $readmePath -Destination (Join-Path $packageDirectory 'README.md')
Copy-Item -LiteralPath (Join-Path $repositoryRoot 'LICENSE') `
    -Destination (Join-Path $packageDirectory 'LICENSE')

$licenseDirectory = Join-Path $packageDirectory 'licenses'
New-Item -ItemType Directory -Path $licenseDirectory -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $repositoryRoot 'third_party/chess-library/LICENSE') `
    -Destination (Join-Path $licenseDirectory 'chess-library-MIT.txt')
Copy-Item -LiteralPath (Join-Path $repositoryRoot 'third_party/fathom/LICENSE') `
    -Destination (Join-Path $licenseDirectory 'fathom-MIT.txt')

$engineName = "koi-engine$binarySuffix"
$avx2Name = "koi-engine-avx2$binarySuffix"
$avx512Name = "koi-engine-avx512$binarySuffix"
@"
Koi Engine v1.0.0 installation

Run $engineName as a UCI engine from En Croissant or another UCI GUI.
$engineName starts the fastest build this CPU supports: it launches the
$avx512Name or $avx2Name sibling when it is present and the CPU supports those
instructions, and otherwise runs its own baseline build in place. Set
KOI_CPU_VARIANT=generic, avx2, or avx512 to force one build.

The optional user-supplied book.bin belongs beside $engineName and is not
included in this package. Syzygy tablebase files are also user-supplied;
configure SyzygyPath in the GUI when they are available.

Recommended starting options:
  Hash=512
  Threads=1
  Speed=100
  OwnBook=true
  BookFile=book.bin
  BookDepth=16
  BookRandom=false

Every build requires an x86-64 CPU. The AVX2 build requires AVX2 and the
AVX-512 build requires AVX-512; the baseline build has no instruction-set
requirement. The optional GPU NNUE inference needs an NVIDIA GPU (Pascal
sm_61 or newer) with a CUDA 12.x-capable driver and is opt-in with
KOI_GPU_NNUE=1.
"@ | Set-Content -LiteralPath (Join-Path $packageDirectory 'INSTALL.txt') -Encoding UTF8

$sourceCommit = $env:GITHUB_SHA
if ($sourceCommit -notmatch '^[0-9a-f]{40,64}$') {
    $sourceCommitOutput = @(& git -C $repositoryRoot rev-parse HEAD 2>$null)
    $gitExitCode = $LASTEXITCODE
    $sourceCommit = if ($sourceCommitOutput.Count -gt 0) { $sourceCommitOutput[0].Trim() } else { 'unknown' }
    if ($gitExitCode -ne 0 -or $sourceCommit -notmatch '^[0-9a-f]{40,64}$') {
        $sourceCommit = 'unknown'
    }
}
if ($sourceCommit -notmatch '^[0-9a-f]{40,64}$') {
    throw 'Release packaging requires a source commit; set GITHUB_SHA or run from a Git checkout.'
}
$statusOutput = @(& git -C $repositoryRoot status --porcelain --untracked-files=all 2>$null)
$gitStatusExitCode = $LASTEXITCODE
if ($gitStatusExitCode -ne 0) {
    throw 'Unable to determine whether the source checkout is dirty.'
}
$sourceDirty = $statusOutput.Count -gt 0
$packageFiles = @(Get-ChildItem -LiteralPath $packageDirectory -File -Recurse |
    Where-Object { $_.FullName -ne (Join-Path $packageDirectory 'package.json') } |
    Sort-Object { $_.FullName.Substring($packageDirectory.Length + 1).Replace('\', '/') } |
    ForEach-Object {
        $relativePath = $_.FullName.Substring($packageDirectory.Length + 1).Replace('\', '/')
        [ordered]@{
            path = $relativePath
            size_bytes = $_.Length
            sha256 = Get-Sha256Hex $_.FullName
        }
    })
$manifest = [ordered]@{
    schema = 'koi-engine-package-v1'
    version = '1.0.0'
    platform = if ($isWindowsHost) { 'windows-x86_64' } else { 'linux-x86_64' }
    build_configuration = 'Release'
    files = $packageFiles
    book_included = $false
    tablebases_included = $false
    provenance = [ordered]@{
        source_commit = $sourceCommit
        source_dirty = [bool]$sourceDirty
        source_ref = if (-not [string]::IsNullOrWhiteSpace($env:GITHUB_REF)) { $env:GITHUB_REF } else { $null }
        created_utc = [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ')
    }
}
$manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $packageDirectory 'package.json') -Encoding UTF8

if (Test-Path -LiteralPath $archivePath) {
    Remove-Item -LiteralPath $archivePath -Force
}
if ($isWindowsHost) {
    Compress-Archive -Path (Join-Path $packageDirectory '*') -DestinationPath $archivePath -CompressionLevel Optimal
} else {
    & tar -czf $archivePath -C $packageDirectory .
    if ($LASTEXITCODE -ne 0) {
        throw "tar failed with exit code $LASTEXITCODE."
    }
}
$archiveHash = Get-Sha256Hex -Path $archivePath
Set-Content -LiteralPath "$archivePath.sha256" -Value "$archiveHash  $([System.IO.Path]::GetFileName($archivePath))" -Encoding ASCII

Write-Output "package_directory=$packageDirectory"
Write-Output "archive=$archivePath"
Write-Output "archive_sha256=$archiveHash"
Write-Output "book_included=false"
