[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$installerPath = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../../tools/build/install_book.ps1'))
$failures = [System.Collections.Generic.List[string]]::new()

function Assert-True {
    param(
        [bool]$Condition,
        [string]$Message
    )

    if (-not $Condition) {
        $failures.Add($Message)
    }
}

function Assert-Contains {
    param(
        [string]$Text,
        [string]$Expected,
        [string]$Message
    )

    Assert-True ($Text.IndexOf($Expected, [System.StringComparison]::OrdinalIgnoreCase) -ge 0) $Message
}

function Get-TestSha256 {
    param([Parameter(Mandatory = $true)][string]$Path)

    $sha256 = [System.Security.Cryptography.SHA256]::Create()
    try {
        $bytes = [System.IO.File]::ReadAllBytes($Path)
        return ([System.BitConverter]::ToString($sha256.ComputeHash($bytes)) -replace '-', '').ToLowerInvariant()
    }
    finally {
        $sha256.Dispose()
    }
}

Assert-True (Test-Path -LiteralPath $installerPath -PathType Leaf) 'install_book.ps1 must exist.'

if (Test-Path -LiteralPath $installerPath -PathType Leaf) {
    $source = Get-Content -LiteralPath $installerPath -Raw
    Assert-Contains $source 'Mandatory = $true' 'EnginePath must be mandatory.'
    Assert-Contains $source '[string]$EnginePath' 'Installer must expose EnginePath.'
    Assert-Contains $source '[string]$SourceFile' 'Installer must expose the offline SourceFile seam.'
    Assert-Contains $source '[string]$ExpectedSha256' 'Installer must expose the ExpectedSha256 seam.'
    Assert-Contains $source '[switch]$Force' 'Installer must expose the optional Force switch.'
    Assert-Contains $source 'books-2026-05-v1' 'Installer must pin the approved release.'
    Assert-Contains $source 'releases/tag/$BookReleaseTag' 'Installer must retain the pinned release URL.'
    Assert-Contains $source 'lichess_1900_rapid_2026-05.bin' 'Installer must pin the approved asset.'
    Assert-Contains $source '56abc70e5291b4338356009d380e565fd85eab8067f6bf34927b5807ff231370' 'Installer must pin the approved SHA-256.'
    Assert-Contains $source 'Invoke-WebRequest' 'Installer must download the asset only during installation.'
    Assert-Contains $source 'SHA256' 'Installer must verify the downloaded hash.'
    Assert-Contains $source 'finally' 'Installer must clean up temporary files.'

    $tempRoot = Join-Path ([System.IO.Path]::GetTempPath()) ('koi-install-book-test-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $tempRoot | Out-Null

    try {
        $missingEngine = Join-Path $tempRoot 'missing.exe'
        $missingFailed = $false
        $missingOutput = ''
        try {
            & $installerPath -EnginePath $missingEngine | Out-Null
        }
        catch {
            $missingFailed = $true
            $missingOutput = ($_ | Out-String)
        }
        Assert-True $missingFailed 'A missing EnginePath must fail.'
        Assert-Contains $missingOutput 'EnginePath' 'Missing EnginePath errors must identify the input.'

        $enginePath = Join-Path $tempRoot 'koi-engine.exe'
        [System.IO.File]::WriteAllBytes($enginePath, [byte[]](0x4B, 0x4F, 0x49))
        $bookPath = Join-Path $tempRoot 'book.bin'
        $originalBytes = [byte[]](0x01, 0x02, 0x03, 0x04)
        [System.IO.File]::WriteAllBytes($bookPath, $originalBytes)
        $originalHash = Get-TestSha256 -Path $bookPath

        $refusalFailed = $false
        $refusalOutput = ''
        try {
            & $installerPath -EnginePath $enginePath | Out-Null
        }
        catch {
            $refusalFailed = $true
            $refusalOutput = ($_ | Out-String)
        }
        Assert-True $refusalFailed 'A different existing book must fail without Force.'
        Assert-Contains $refusalOutput 'Refusing to overwrite' 'Overwrite refusal must be explicit.'
        $currentHash = Get-TestSha256 -Path $bookPath
        Assert-True ($currentHash -eq $originalHash) 'Overwrite refusal must leave the existing book unchanged.'

        # Successful offline install using the documented test seams.
        $successRoot = Join-Path $tempRoot 'success'
        New-Item -ItemType Directory -Path $successRoot | Out-Null
        $successEngine = Join-Path $successRoot 'koi-engine.exe'
        [System.IO.File]::WriteAllBytes($successEngine, [byte[]](0x4B, 0x4F, 0x49))
        $fixturePath = Join-Path $tempRoot 'fixture-book.bin'
        [System.IO.File]::WriteAllBytes($fixturePath, [byte[]](0x50, 0x4F, 0x4C, 0x59))
        $fixtureHash = Get-TestSha256 -Path $fixturePath

        $installOutput = & $installerPath -EnginePath $successEngine -SourceFile $fixturePath -ExpectedSha256 $fixtureHash
        $installedPath = Join-Path $successRoot 'book.bin'
        Assert-True (Test-Path -LiteralPath $installedPath -PathType Leaf) 'A verified local install must publish book.bin.'
        if (Test-Path -LiteralPath $installedPath -PathType Leaf) {
            Assert-True ((Get-TestSha256 -Path $installedPath) -eq $fixtureHash) 'The installed book must match the verified source hash.'
        }
        Assert-Contains ($installOutput | Out-String) 'Installed' 'A successful install must report the installed path.'

        # A second run must detect the already-installed verified book.
        $repeatOutput = & $installerPath -EnginePath $successEngine -SourceFile $fixturePath -ExpectedSha256 $fixtureHash
        Assert-Contains ($repeatOutput | Out-String) 'already installed' 'Reinstalling a verified book must be a no-op.'

        # Force must replace a different existing book with the verified asset.
        [System.IO.File]::WriteAllBytes($installedPath, [byte[]](0xAA, 0xBB))
        $forceOutput = & $installerPath -EnginePath $successEngine -SourceFile $fixturePath -ExpectedSha256 $fixtureHash -Force
        Assert-True ((Get-TestSha256 -Path $installedPath) -eq $fixtureHash) 'Force must replace a different existing book.'
        Assert-Contains ($forceOutput | Out-String) 'Installed' 'A forced install must report the installed path.'
    }
    finally {
        if (Test-Path -LiteralPath $tempRoot) {
            Remove-Item -LiteralPath $tempRoot -Recurse -Force
        }
    }
}

if ($failures.Count -gt 0) {
    $failures | ForEach-Object { Write-Error $_ }
    exit 1
}

Write-Output 'install_book tests passed.'
