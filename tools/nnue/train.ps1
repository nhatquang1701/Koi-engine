<#
.SYNOPSIS
Headless training entry point for the Koi NNUE Studio pipeline.

.DESCRIPTION
Thin wrapper around `tools/nnue/koi_nnue_studio.py --run <preset>` so training
can be started from a terminal, Task Scheduler or CI without opening the GUI.
Progress is written to `artifacts/training/runs/<stamp>-train-<backend>/`.

.PARAMETER Preset
Training preset: quick (1 epoch), standard (10 epochs) or thorough.

.PARAMETER Detach
Launch the run in the background and return immediately.

.EXAMPLE
pwsh -NoProfile -File tools/nnue/train.ps1 -Preset quick
pwsh -NoProfile -File tools/nnue/train.ps1 -Preset thorough -Detach
#>

param(
    [ValidateSet('quick', 'standard', 'thorough')]
    [string]$Preset = 'standard',

    [switch]$Detach,

    [string]$Backend = 'koi',

    [string]$Corpus,

    [int]$Epochs,

    [int]$Rows
)

$ErrorActionPreference = 'Stop'
$repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$studio = Join-Path $repositoryRoot 'tools\nnue\koi_nnue_studio.py'
if (-not (Test-Path -LiteralPath $studio -PathType Leaf)) {
    throw "koi_nnue_studio.py is missing: $studio"
}

$python = (Get-Command python.exe -ErrorAction Stop).Source
$arguments = @($studio, '--run', $Preset, '--backend', $Backend)
if ($Detach) {
    $arguments += '--detach'
}
if (-not [string]::IsNullOrWhiteSpace($Corpus)) {
    $arguments += @('--corpus', $Corpus)
}
if ($PSBoundParameters.ContainsKey('Epochs')) {
    $arguments += @('--epochs', $Epochs)
}
if ($PSBoundParameters.ContainsKey('Rows')) {
    $arguments += @('--rows', $Rows)
}

Write-Output ("python " + ($arguments -join ' '))
& $python @arguments
exit $LASTEXITCODE
