#requires -Version 5.1

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [ValidateNotNullOrEmpty()]
    [string]$EnginePath,

    # Optional offline test seams. SourceFile installs a local asset instead of
    # downloading, and ExpectedSha256 overrides the pinned hash. The pinned
    # values below stay authoritative when the parameters are omitted.
    [string]$SourceFile = '',

    [string]$ExpectedSha256 = '',

    [switch]$Force
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

# This is a one-time installer pin. Koi itself never downloads or updates books.
$BookSourceRepository = 'https://github.com/Dash1971/chess-opening-book-builder'
$BookReleaseTag = 'books-2026-05-v1'
$BookReleaseUrl = "$BookSourceRepository/releases/tag/$BookReleaseTag"
$BookAssetName = 'lichess_1900_rapid_2026-05.bin'
$BookDownloadUri = "$BookSourceRepository/releases/download/$BookReleaseTag/$BookAssetName"
$BookLicense = 'CC0 1.0 Universal'
if ([string]::IsNullOrWhiteSpace($ExpectedSha256)) {
    $ExpectedSha256 = '56abc70e5291b4338356009d380e565fd85eab8067f6bf34927b5807ff231370'
}
$DestinationName = 'book.bin'

function Write-InstallerMessage {
    param([Parameter(Mandatory = $true)][string]$Message)

    Write-Output ("Koi book installer: " + $Message)
}

function Test-ReparsePoint {
    param([Parameter(Mandatory = $true)][System.IO.FileSystemInfo]$Item)

    return (([int]$Item.Attributes -band [int][System.IO.FileAttributes]::ReparsePoint) -ne 0)
}

function Resolve-EngineExecutable {
    param([Parameter(Mandatory = $true)][string]$Path)

    if ([string]::IsNullOrWhiteSpace($Path)) {
        throw 'EnginePath is required.'
    }

    try {
        $item = Get-Item -LiteralPath $Path -Force -ErrorAction Stop
    }
    catch {
        throw "EnginePath is missing or inaccessible: $Path"
    }

    if ($item.PSProvider.Name -ne 'FileSystem' -or $item.PSIsContainer) {
        throw "EnginePath must identify a regular filesystem executable file: $Path"
    }

    if (-not ($item -is [System.IO.FileInfo])) {
        throw "EnginePath must identify a regular filesystem executable file: $Path"
    }

    if (-not [string]::Equals($item.Extension, '.exe', [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "EnginePath must point to a .exe file: $Path"
    }

    if (Test-ReparsePoint -Item $item) {
        throw "EnginePath must not be a symbolic link or reparse point: $Path"
    }

    try {
        return [System.IO.Path]::GetFullPath($item.FullName)
    }
    catch {
        throw "EnginePath could not be resolved securely: $Path"
    }
}

function Get-ExistingBook {
    param([Parameter(Mandatory = $true)][string]$Path)

    try {
        $item = Get-Item -LiteralPath $Path -Force -ErrorAction SilentlyContinue
    }
    catch {
        throw "The destination book path is inaccessible: $Path"
    }

    if ($null -eq $item) {
        return $null
    }

    if ($item.PSProvider.Name -ne 'FileSystem' -or $item.PSIsContainer -or
        -not ($item -is [System.IO.FileInfo])) {
        throw "The destination path must be a regular file or not exist: $Path"
    }

    if (Test-ReparsePoint -Item $item) {
        throw "The destination book must not be a symbolic link or reparse point: $Path"
    }

    return $item
}

function Get-BookSha256 {
    param([Parameter(Mandatory = $true)][string]$Path)

    # Use the .NET implementation equivalent to Get-FileHash -Algorithm SHA256.
    # Some hosted Windows PowerShell environments shadow that cmdlet with a
    # function that cannot hash files, while the .NET API is stable in both
    # Windows PowerShell 5.1 and PowerShell 7.
    $sha256 = [System.Security.Cryptography.SHA256]::Create()
    $stream = $null
    try {
        $stream = [System.IO.File]::OpenRead($Path)
        $hash = [System.BitConverter]::ToString($sha256.ComputeHash($stream)).Replace('-', '').ToLowerInvariant()
        return $hash
    }
    catch {
        throw "Unable to calculate the SHA-256 for: $Path"
    }
    finally {
        if ($null -ne $stream) {
            $stream.Dispose()
        }
        $sha256.Dispose()
    }
}

function New-TemporaryDownloadPath {
    param([Parameter(Mandatory = $true)][string]$Directory)

    for ($attempt = 0; $attempt -lt 10; $attempt++) {
        $candidate = Join-Path -Path $Directory -ChildPath ('.koi-book-' + [guid]::NewGuid().ToString('N') + '.download')
        if (-not (Test-Path -LiteralPath $candidate)) {
            return $candidate
        }
    }

    throw "Unable to reserve a temporary download path beside the engine: $Directory"
}

$resolvedEnginePath = Resolve-EngineExecutable -Path $EnginePath
$engineDirectory = [System.IO.Path]::GetDirectoryName($resolvedEnginePath)

if ([string]::IsNullOrWhiteSpace($engineDirectory)) {
    throw "Unable to determine the engine directory: $resolvedEnginePath"
}

try {
    $engineDirectoryItem = Get-Item -LiteralPath $engineDirectory -Force -ErrorAction Stop
}
catch {
    throw "The engine directory is inaccessible: $engineDirectory"
}

if ($engineDirectoryItem.PSProvider.Name -ne 'FileSystem' -or -not $engineDirectoryItem.PSIsContainer) {
    throw "The engine directory is not a local filesystem directory: $engineDirectory"
}

if (Test-ReparsePoint -Item $engineDirectoryItem) {
    throw "The engine directory must not be a symbolic link or reparse point: $engineDirectory"
}

$destinationPath = Join-Path -Path $engineDirectoryItem.FullName -ChildPath $DestinationName
$existingBook = Get-ExistingBook -Path $destinationPath

if ($null -ne $existingBook) {
    $existingHash = Get-BookSha256 -Path $existingBook.FullName
    if ([string]::Equals($existingHash, $ExpectedSha256, [System.StringComparison]::OrdinalIgnoreCase)) {
        Write-InstallerMessage "$DestinationName is already installed beside the engine. SHA-256 verified: $ExpectedSha256"
        return
    }

    if (-not $Force) {
        throw "Refusing to overwrite a different existing $DestinationName. Use -Force only after verifying that replacement is intended."
    }

    Write-InstallerMessage "Existing $DestinationName differs from the pinned asset; -Force will replace it after verification."
}

$temporaryDownloadPath = $null
$replacementBackupPath = $null

try {
    $temporaryDownloadPath = New-TemporaryDownloadPath -Directory $engineDirectoryItem.FullName

    if (-not [string]::IsNullOrWhiteSpace($SourceFile)) {
        Write-InstallerMessage "Installing the supplied local asset instead of downloading $BookAssetName."
        try {
            Copy-Item -LiteralPath $SourceFile -Destination $temporaryDownloadPath -Force -ErrorAction Stop
        }
        catch {
            throw "Unable to read the supplied local book source: $($_.Exception.Message)"
        }
    }
    else {
        Write-InstallerMessage "Downloading $BookAssetName from the pinned $BookReleaseTag release."
        Write-InstallerMessage "Provenance: $BookLicense; release $BookReleaseUrl"

        try {
            # GitHub requires modern TLS on supported Windows installations. The hash
            # check below remains authoritative even when the download follows a CDN redirect.
            try {
                $currentProtocol = [System.Net.ServicePointManager]::SecurityProtocol
                if (($currentProtocol -band [System.Net.SecurityProtocolType]::Tls12) -eq 0) {
                    [System.Net.ServicePointManager]::SecurityProtocol = $currentProtocol -bor [System.Net.SecurityProtocolType]::Tls12
                }
            }
            catch {
                # PowerShell 7 may use a handler that does not expose this legacy setting.
            }

            Invoke-WebRequest -Uri $BookDownloadUri -OutFile $temporaryDownloadPath -UseBasicParsing -MaximumRedirection 5 -Headers @{
                'User-Agent' = 'Koi-book-installer/1.0'
            }
        }
        catch {
            throw "Unable to download the pinned opening book: $($_.Exception.Message)"
        }
    }

    $downloadedBook = Get-Item -LiteralPath $temporaryDownloadPath -Force -ErrorAction Stop
    if ($downloadedBook.PSProvider.Name -ne 'FileSystem' -or $downloadedBook.PSIsContainer -or
        -not ($downloadedBook -is [System.IO.FileInfo]) -or $downloadedBook.Length -le 0 -or
        (Test-ReparsePoint -Item $downloadedBook)) {
        throw 'The downloaded asset is not a non-empty regular file.'
    }

    $downloadedHash = Get-BookSha256 -Path $downloadedBook.FullName
    if (-not [string]::Equals($downloadedHash, $ExpectedSha256, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "SHA-256 verification failed. Expected $ExpectedSha256 but received $downloadedHash."
    }

    # Re-check the destination after downloading so another process cannot cause
    # an unapproved overwrite while the network request was in progress.
    $existingBook = Get-ExistingBook -Path $destinationPath
    if ($null -ne $existingBook) {
        $existingHash = Get-BookSha256 -Path $existingBook.FullName
        if ([string]::Equals($existingHash, $ExpectedSha256, [System.StringComparison]::OrdinalIgnoreCase)) {
            Write-InstallerMessage "$DestinationName was installed by another process; its SHA-256 is verified."
            return
        }

        if (-not $Force) {
            throw "Refusing to overwrite a different existing $DestinationName. Use -Force only after verifying that replacement is intended."
        }

        $replacementBackupPath = Join-Path -Path $engineDirectoryItem.FullName -ChildPath ('.koi-book-' + [guid]::NewGuid().ToString('N') + '.backup')
        if (Test-Path -LiteralPath $replacementBackupPath) {
            throw "Unable to reserve a temporary replacement path beside the engine: $replacementBackupPath"
        }

        [System.IO.File]::Replace($temporaryDownloadPath, $destinationPath, $replacementBackupPath, $true)
        Remove-Item -LiteralPath $replacementBackupPath -Force -ErrorAction Stop
        $replacementBackupPath = $null
    }
    else {
        [System.IO.File]::Move($temporaryDownloadPath, $destinationPath)
    }

    $temporaryDownloadPath = $null
    Write-InstallerMessage "Installed $destinationPath"
    Write-InstallerMessage "SHA-256 verified: $ExpectedSha256"
    Write-InstallerMessage 'The engine has no runtime network dependency; it will read this local book.bin.'
}
finally {
    if ($null -ne $temporaryDownloadPath -and (Test-Path -LiteralPath $temporaryDownloadPath -PathType Leaf)) {
        Remove-Item -LiteralPath $temporaryDownloadPath -Force -ErrorAction SilentlyContinue
    }

    if ($null -ne $replacementBackupPath -and (Test-Path -LiteralPath $replacementBackupPath -PathType Leaf)) {
        Remove-Item -LiteralPath $replacementBackupPath -Force -ErrorAction SilentlyContinue
    }
}
