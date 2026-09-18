$ErrorActionPreference = 'Stop'

$repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..'))
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
# Three independent jobs: a failing Debug or differential leg must not cancel
# the primary Release suite, and every leg uploads its own diagnostics.
Require-WorkflowPattern '(?m)^\s{2}release-full:\s*$' 'the full Release job'
Require-WorkflowPattern '(?m)^\s{2}debug-smoke:\s*$' 'the Debug smoke job'
Require-WorkflowPattern '(?m)^\s{2}shadow-diff:\s*$' 'the differential shadow job'
Require-WorkflowPattern '(?m)^\s*timeout-minutes:\s*60\s*$' 'a Release/Debug job timeout'
Require-WorkflowPattern '(?m)^\s*timeout-minutes:\s*30\s*$' 'a differential job timeout'
Require-WorkflowPattern '(?i)python-chess' 'the optional python-chess dependency install'
Require-WorkflowPattern '(?i)numpy' 'the optional numpy dependency install'
Require-WorkflowPattern '(?i)actions/checkout@v5' 'the current checkout action'
Require-WorkflowPattern '(?i)actions/setup-python@v6' 'the current Python setup action'
Require-WorkflowPattern '(?i)actions/upload-artifact@v5' 'test diagnostic artifact upload'
Require-WorkflowPattern '(?i)LastTest\.log' 'the LastTest.log diagnostic upload'
Require-WorkflowPattern '(?i)vswhere\.exe.*-latest.*-property\s+installationPath' 'vswhere Visual Studio discovery'
Require-WorkflowPattern '(?i)VsDevCmd\.bat' 'a discovered VsDevCmd path'
Require-WorkflowPattern '(?m)^\s*call\s+"%VSDEVCMD%"\s+-arch=x64\s+-host_arch=x64\s*$' 'x64 developer-environment setup'
Require-WorkflowPattern '(?m)^\s*cl\s+2>&1\s+\|\s+findstr\s+/C:"x64"\s+>nul\s*$' 'an x64 compiler assertion'
Require-WorkflowPattern '(?m)^\s*cmake\s+-S\s+\.\s+-B\s+build/ci-release\s+-G\s+Ninja\s+-DCMAKE_BUILD_TYPE=Release\s+-DCMAKE_CXX_COMPILER=cl\s*$' 'the Release CMake configure command'
Require-WorkflowPattern '(?m)^\s*cmake\s+-S\s+\.\s+-B\s+build/ci-debug\s+-G\s+Ninja\s+-DCMAKE_BUILD_TYPE=Debug\s+-DCMAKE_CXX_COMPILER=cl\s*$' 'the Debug CMake configure command'
Require-WorkflowPattern '(?m)^\s*cmake\s+--build\s+build/ci-release\s+--config\s+Release\s*$' 'the Release build command'
Require-WorkflowPattern '(?m)^\s*cmake\s+--build\s+build/ci-debug\s+--config\s+Debug\s*$' 'the Debug build command'
Require-WorkflowPattern '(?m)^\s*ctest\s+--test-dir\s+build/ci-release\s+-C\s+Release\s+-j\s+4\s+--output-on-failure\s+--output-junit\s+build/ci-release/ctest-junit\.xml\s*$' 'the parallel Release CTest command'
Require-WorkflowPattern '(?m)^\s*ctest\s+--test-dir\s+build/ci-debug\s+-C\s+Debug\s+-j\s+4\s+-LE\s+heavy\s+--output-on-failure\s+--output-junit\s+build/ci-debug/ctest-junit\.xml\s*$' 'the bounded Debug smoke CTest command'
Require-WorkflowPattern '(?m)^\s*ctest\s+--test-dir\s+build/ci-release\s+-C\s+Release\s+-R\s+koi_shadow_diff_tests\s+--output-on-failure\s*$' 'the differential CTest command'
Require-WorkflowPattern '(?i)-DKOI_BUILD_SHADOW_DIFF=ON' 'the opt-in differential configure switch'
Require-CMakePattern '(?i)check_ipo_supported' 'an IPO/LTO capability check'
Require-CMakePattern '(?i)CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE\s+ON' 'portable Release IPO/LTO'
Require-CMakePattern '(?i)AVX2' 'the required AVX2 Release optimization flag'
Require-CMakePattern '(?i)CONFIG:Release.*:/O2' 'the Release compiler optimization level'
Require-CMakePattern '(?i)CONFIG:Release.*:/DNDEBUG' 'the Release assertion configuration'
Require-CMakePattern '(?i)CONFIG:Release' 'a Release-only AVX2 configuration guard'
Require-CMakePattern '(?i)project\s*\(\s*koi_engine\s+VERSION\s+1\.1\.0' 'the v1.1.0 project identity'
Require-CMakePattern '(?i)CMAKE_MSVC_RUNTIME_LIBRARY' 'the static MSVC runtime policy'
Require-CMakePattern '(?i)FILE_SET\s+CXX_MODULES' 'the C++26 named-module source set'
Require-CMakePattern '(?i)KOI_BUILD_SHADOW_DIFF' 'the opt-in differential test switch'
