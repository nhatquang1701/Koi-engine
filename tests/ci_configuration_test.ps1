$ErrorActionPreference = 'Stop'

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$workflow = Join-Path $repositoryRoot '.github\workflows\windows.yml'
if (-not (Test-Path -LiteralPath $workflow -PathType Leaf)) {
    throw "Windows CI workflow is missing: $workflow"
}

$content = Get-Content -LiteralPath $workflow -Raw
foreach ($required in @(
    'windows-latest',
    'VsDevCmd.bat',
    '-arch=x64',
    'CMAKE_CXX_COMPILER=cl',
    '- Debug',
    '- Release',
    'ctest --test-dir',
    'fail-fast: true'
)) {
    if (-not $content.Contains($required)) {
        throw "Windows CI workflow must contain: $required"
    }
}
