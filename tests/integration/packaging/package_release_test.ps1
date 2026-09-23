[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$BuildDirectory,
    [Parameter(Mandatory = $true)]
    [string]$PackageScript
)

$ErrorActionPreference = 'Stop'
# Use the current PowerShell host so the test runs wherever pwsh is available.
$powerShellExecutable = (Get-Process -Id $PID).Path
$outputDirectory = Join-Path ([System.IO.Path]::GetTempPath()) ('koi-package-test-' + [guid]::NewGuid().ToString('N'))
try {
    & $powerShellExecutable -NoProfile -ExecutionPolicy Bypass -File $PackageScript `
        -BuildDirectory $BuildDirectory -OutputDirectory $outputDirectory | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "package script exited with $LASTEXITCODE"
    }

    $packageDirectory = Join-Path $outputDirectory 'koi-engine-v1.1'
    $archivePath = Join-Path $outputDirectory 'koi-engine-v1.1.zip'
    foreach ($required in @('koi-engine.exe', 'koi-engine-avx2.exe', 'koi-engine-avx512.exe',
                            'koi-bench.exe', 'koi-replay.exe', 'koi-perft.exe',
                            'README.md', 'licenses\chess-library-MIT.txt', 'licenses\fathom-MIT.txt')) {
        if (-not (Test-Path -LiteralPath (Join-Path $packageDirectory $required) -PathType Leaf)) {
            throw "package is missing $required"
        }
    }
    if (Test-Path -LiteralPath (Join-Path $packageDirectory 'book.bin')) {
        throw 'package must not embed user-supplied book.bin'
    }
    if (-not (Test-Path -LiteralPath $archivePath -PathType Leaf)) {
        throw 'package archive was not produced'
    }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [System.IO.Compression.ZipFile]::OpenRead($archivePath)
    try {
        $names = @($archive.Entries | ForEach-Object { $_.FullName })
        foreach ($expected in @('koi-engine.exe', 'koi-engine-avx2.exe', 'koi-engine-avx512.exe')) {
            if ($names -notcontains $expected) {
                throw "package archive is missing $expected"
            }
        }
        if ($names -contains 'book.bin') {
            throw 'package archive has an invalid executable/book layout'
        }
    } finally {
        $archive.Dispose()
    }
    Write-Output 'PASS release package layout'
} finally {
    if (Test-Path -LiteralPath $outputDirectory) {
        Remove-Item -LiteralPath $outputDirectory -Recurse -Force
    }
}
