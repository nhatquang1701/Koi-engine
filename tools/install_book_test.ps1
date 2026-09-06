[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$installerPath = Join-Path $PSScriptRoot 'install_book.ps1'
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
