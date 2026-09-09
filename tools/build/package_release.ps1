[CmdletBinding()]
param(
    [string]$BuildDirectory = (Join-Path $PSScriptRoot '..\..\build\release'),
    [string]$OutputDirectory = (Join-Path $PSScriptRoot '..\..\artifacts\packages')
)

$ErrorActionPreference = 'Stop'
$repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$buildRoot = [System.IO.Path]::GetFullPath($BuildDirectory)
$outputRoot = [System.IO.Path]::GetFullPath($OutputDirectory)
$packageDirectory = Join-Path $outputRoot 'koi-engine-v1.1'
$archivePath = Join-Path $outputRoot 'koi-engine-v1.1.zip'

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

$requiredExecutables = @('koi-engine.exe', 'koi-bench.exe', 'koi-replay.exe', 'koi-perft.exe')
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
$outputPrefix = $outputRoot.TrimEnd('\', '/') + '\'
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

$licenseDirectory = Join-Path $packageDirectory 'licenses'
New-Item -ItemType Directory -Path $licenseDirectory -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $repositoryRoot 'third_party\chess-library\LICENSE') `
    -Destination (Join-Path $licenseDirectory 'chess-library-MIT.txt')
Copy-Item -LiteralPath (Join-Path $repositoryRoot 'third_party\fathom\LICENSE') `
    -Destination (Join-Path $licenseDirectory 'fathom-MIT.txt')

@'
Koi Engine v1.1 installation

Run koi-engine.exe as a UCI engine from En Croissant or another UCI GUI.
The optional user-supplied book.bin belongs beside koi-engine.exe and is not
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

The AVX2 Release binary requires an x64 CPU with AVX2 support.
'@ | Set-Content -LiteralPath (Join-Path $packageDirectory 'INSTALL.txt') -Encoding UTF8

$manifest = [ordered]@{
    schema = 'koi-engine-package-v1'
    version = '1.1.0'
    build_directory = $buildRoot
    executables = @($requiredExecutables | ForEach-Object {
        [ordered]@{ name = $_; sha256 = Get-Sha256Hex (Join-Path $packageDirectory $_) }
    })
    book_included = $false
    tablebases_included = $false
}
$manifest | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $packageDirectory 'package.json') -Encoding UTF8

if (Test-Path -LiteralPath $archivePath) {
    Remove-Item -LiteralPath $archivePath -Force
}
Compress-Archive -Path (Join-Path $packageDirectory '*') -DestinationPath $archivePath -CompressionLevel Optimal

Write-Output "package_directory=$packageDirectory"
Write-Output "archive=$archivePath"
Write-Output "book_included=false"
