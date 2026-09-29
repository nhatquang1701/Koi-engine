param(
    [Parameter(Mandatory = $true)]
    [string]$RunTestsPath,
    [Parameter(Mandatory = $true)]
    [string]$CMakePath
)

$ErrorActionPreference = 'Stop'
$repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
$temporaryRoot = [System.IO.Path]::GetFullPath([System.IO.Path]::Combine(
    [System.IO.Path]::GetTempPath(), 'koi-run-tests-artifact-' + [guid]::NewGuid().ToString('N')))
$temporaryBuild = Join-Path $temporaryRoot 'build'
$relativeOutput = Join-Path 'build' ('run-tests-artifact-' + [guid]::NewGuid().ToString('N'))
$outputDirectory = [System.IO.Path]::GetFullPath((Join-Path $repositoryRoot $relativeOutput))
$junitPath = Join-Path $outputDirectory 'ctest-junit.xml'
$lastTestPath = Join-Path $outputDirectory 'LastTest.log'
$fakeCTestPath = Join-Path $temporaryRoot 'fake-ctest.ps1'
$powerShellPath = (Get-Command pwsh.exe -ErrorAction Stop).Source

function Assert-ContainedPath([string]$Path, [string]$Parent) {
    $fullPath = [System.IO.Path]::GetFullPath($Path)
    $parentPath = [System.IO.Path]::GetFullPath($Parent).TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
    if (-not $fullPath.StartsWith($parentPath, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to remove a path outside '$Parent': $fullPath"
    }
}

New-Item -ItemType Directory -Path $temporaryBuild -Force | Out-Null
$fakeCTest = @'
$arguments = $args
$testDirIndex = [Array]::IndexOf($arguments, '--test-dir')
$junitIndex = [Array]::IndexOf($arguments, '--output-junit')
if ($testDirIndex -lt 0 -or $junitIndex -lt 0) {
    throw 'The runner did not pass the build and JUnit output paths to CTest.'
}
$testDirectory = [System.IO.Path]::GetFullPath($arguments[$testDirIndex + 1])
$junitArgument = $arguments[$junitIndex + 1]
$junitFile = if ([System.IO.Path]::IsPathRooted($junitArgument)) {
    [System.IO.Path]::GetFullPath($junitArgument)
} else {
    [System.IO.Path]::GetFullPath((Join-Path $testDirectory $junitArgument))
}
$temporaryLog = Join-Path $testDirectory 'Testing/Temporary/LastTest.log'
New-Item -ItemType Directory -Path ([System.IO.Path]::GetDirectoryName($temporaryLog)) -Force | Out-Null
New-Item -ItemType Directory -Path ([System.IO.Path]::GetDirectoryName($junitFile)) -Force | Out-Null
[System.IO.File]::WriteAllText($temporaryLog, 'End testing.')
[System.IO.File]::WriteAllText($junitFile, '<testsuites tests="0" failures="0" />')
$global:LASTEXITCODE = 0
'@
Set-Content -LiteralPath $fakeCTestPath -Value $fakeCTest -Encoding utf8NoBOM

try {
    Push-Location $repositoryRoot
    try {
        & $powerShellPath -NoProfile -File $RunTestsPath `
            -BuildDirectory $temporaryBuild -Configuration Release -NoBuild `
            -Parallel 1 -RepeatUntilPass 1 -CMakePath $CMakePath `
            -CTestPath $fakeCTestPath -OutputDirectory $relativeOutput
        if ($LASTEXITCODE -ne 0) {
            throw "run_tests.ps1 exited with $LASTEXITCODE"
        }
    } finally {
        Pop-Location
    }

    if (-not (Test-Path -LiteralPath $junitPath -PathType Leaf)) {
        throw "CTest did not leave ctest-junit.xml in the selected output directory '$outputDirectory'."
    }
    if (-not (Test-Path -LiteralPath $lastTestPath -PathType Leaf)) {
        throw "The runner did not copy LastTest.log to '$outputDirectory'."
    }
    if ((Get-Content -LiteralPath $junitPath -Raw) -notmatch '<testsuites') {
        throw 'The JUnit output is missing its XML document.'
    }
    Write-Output 'run_tests.ps1 preserves JUnit and LastTest.log in the selected output directory.'
} finally {
    $repositoryBuild = Join-Path $repositoryRoot 'build'
    if (Test-Path -LiteralPath $outputDirectory) {
        Assert-ContainedPath $outputDirectory $repositoryBuild
        Remove-Item -LiteralPath $outputDirectory -Recurse -Force
    }
    if (Test-Path -LiteralPath $temporaryRoot) {
        Assert-ContainedPath $temporaryRoot ([System.IO.Path]::GetTempPath())
        Remove-Item -LiteralPath $temporaryRoot -Recurse -Force
    }
}
