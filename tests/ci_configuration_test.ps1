$ErrorActionPreference = 'Stop'

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$workflow = Join-Path $repositoryRoot '.github\workflows\windows.yml'
$cmake = Join-Path $repositoryRoot 'CMakeLists.txt'
if (-not (Test-Path -LiteralPath $workflow -PathType Leaf)) {
    throw "Windows CI workflow is missing: $workflow"
}

$content = Get-Content -LiteralPath $workflow -Raw
$cmakeContent = Get-Content -LiteralPath $cmake -Raw

function Require-WorkflowPattern([string]$Pattern, [string]$Description) {
    if ($content -notmatch $Pattern) {
        throw "Windows CI workflow must define $Description."
    }
}

function Require-CMakePattern([string]$Pattern, [string]$Description) {
    if ($cmakeContent -notmatch $Pattern) {
        throw "CMakeLists.txt must define $Description."
    }
}

Require-WorkflowPattern '(?m)^\s*runs-on:\s*windows-latest\s*$' 'a Windows runner'
Require-WorkflowPattern '(?ms)^\s*strategy:\s*\r?\n\s*fail-fast:\s*true\s*\r?\n\s*matrix:' 'a fail-fast matrix strategy'
Require-WorkflowPattern '(?ms)^\s*configuration:\s*\r?\n\s*-\s*Debug\s*\r?\n\s*-\s*Release\s*$' 'Debug and Release matrix entries'
Require-WorkflowPattern '(?i)vswhere\.exe.*-latest.*-property\s+installationPath' 'vswhere Visual Studio discovery'
Require-WorkflowPattern '(?i)VsDevCmd\.bat' 'a discovered VsDevCmd path'
Require-WorkflowPattern '(?m)^\s*call\s+"%VSDEVCMD%"\s+-arch=x64\s+-host_arch=x64\s*$' 'x64 developer-environment setup'
Require-WorkflowPattern '(?m)^\s*cl\s+2>&1\s+\|\s+findstr\s+/C:"x64"\s+>nul\s*$' 'an x64 compiler assertion'
Require-WorkflowPattern '(?m)^\s*cmake\s+-S\s+\.\s+-B\s+out\\ci-\$\{\{ matrix\.configuration \}\}\s+-G\s+Ninja\s+-DCMAKE_BUILD_TYPE=\$\{\{ matrix\.configuration \}\}\s+-DCMAKE_CXX_COMPILER=cl\s*$' 'the x64 CMake configure command'
Require-WorkflowPattern '(?m)^\s*cmake\s+--build\s+out\\ci-\$\{\{ matrix\.configuration \}\}\s+--config\s+\$\{\{ matrix\.configuration \}\}\s*$' 'the build command'
Require-WorkflowPattern '(?m)^\s*ctest\s+--test-dir\s+out\\ci-\$\{\{ matrix\.configuration \}\}\s+-C\s+\$\{\{ matrix\.configuration \}\}\s+--output-on-failure\s*$' 'the CTest command'
Require-CMakePattern '(?i)check_ipo_supported' 'an IPO/LTO capability check'
Require-CMakePattern '(?i)CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE\s+ON' 'portable Release IPO/LTO'
Require-CMakePattern '(?i)AVX2' 'the required AVX2 Release optimization flag'
Require-CMakePattern '(?i)CONFIG:Release' 'a Release-only AVX2 configuration guard'
