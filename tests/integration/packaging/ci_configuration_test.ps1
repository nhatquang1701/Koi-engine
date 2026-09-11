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
Require-WorkflowPattern '(?ms)^\s*strategy:\s*\r?\n\s*fail-fast:\s*true\s*\r?\n\s*matrix:' 'a fail-fast matrix strategy'
Require-WorkflowPattern '(?ms)configuration:\s*Debug\s*\r?\n\s*build_dir:\s*build/ci-debug' 'the canonical CI Debug build directory'
Require-WorkflowPattern '(?ms)configuration:\s*Release\s*\r?\n\s*build_dir:\s*build/ci-release' 'the canonical CI Release build directory'
Require-WorkflowPattern '(?i)vswhere\.exe.*-latest.*-property\s+installationPath' 'vswhere Visual Studio discovery'
Require-WorkflowPattern '(?i)VsDevCmd\.bat' 'a discovered VsDevCmd path'
Require-WorkflowPattern '(?m)^\s*call\s+"%VSDEVCMD%"\s+-arch=x64\s+-host_arch=x64\s*$' 'x64 developer-environment setup'
Require-WorkflowPattern '(?m)^\s*cl\s+2>&1\s+\|\s+findstr\s+/C:"x64"\s+>nul\s*$' 'an x64 compiler assertion'
Require-WorkflowPattern '(?m)^\s*cmake\s+-S\s+\.\s+-B\s+\$\{\{ matrix\.build_dir \}\}\s+-G\s+Ninja\s+-DCMAKE_BUILD_TYPE=\$\{\{ matrix\.configuration \}\}\s+-DCMAKE_CXX_COMPILER=cl\s*$' 'the x64 CMake configure command'
Require-WorkflowPattern '(?m)^\s*cmake\s+--build\s+\$\{\{ matrix\.build_dir \}\}\s+--config\s+\$\{\{ matrix\.configuration \}\}\s*$' 'the build command'
Require-WorkflowPattern '(?m)^\s*ctest\s+--test-dir\s+\$\{\{ matrix\.build_dir \}\}\s+-C\s+\$\{\{ matrix\.configuration \}\}\s+--output-on-failure\s*$' 'the CTest command'
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
