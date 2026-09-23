<#
.SYNOPSIS
Create a Desktop or Start Menu shortcut for the Koi NNUE Studio.

.PARAMETER Location
Desktop (default) or StartMenu.

.PARAMETER Name
Shortcut name without the .lnk extension.

.EXAMPLE
pwsh -NoProfile -File tools/nnue/create-shortcut.ps1
pwsh -NoProfile -File tools/nnue/create-shortcut.ps1 -Location StartMenu
#>

param(
    [ValidateSet('Desktop', 'StartMenu')]
    [string]$Location = 'Desktop',

    [string]$Name = 'Koi NNUE Studio'
)

$ErrorActionPreference = 'Stop'
$repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$target = Join-Path $repositoryRoot 'Koi NNUE Studio.cmd'
if (-not (Test-Path -LiteralPath $target -PathType Leaf)) {
    throw "Studio launcher is missing: $target"
}

$folder = if ($Location -ceq 'Desktop') {
    [Environment]::GetFolderPath('Desktop')
} else {
    Join-Path ([Environment]::GetFolderPath('StartMenu')) 'Programs'
}
New-Item -ItemType Directory -Force -Path $folder | Out-Null
$linkPath = Join-Path $folder ($Name + '.lnk')

$shell = New-Object -ComObject WScript.Shell
$shortcut = $shell.CreateShortcut($linkPath)
$shortcut.TargetPath = $target
$shortcut.WorkingDirectory = $repositoryRoot
$shortcut.Description = 'Train, validate and install Koi NNUE networks'
$shortcut.IconLocation = "$env:SystemRoot\System32\shell32.dll,13"
$shortcut.Save()

Write-Output "shortcut $linkPath"
