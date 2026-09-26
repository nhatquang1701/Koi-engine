$ErrorActionPreference = 'Stop'

$repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
$workflow = Join-Path $repositoryRoot '.github/workflows/windows.yml'
$linuxWorkflow = Join-Path $repositoryRoot '.github/workflows/linux.yml'
$candidateWorkflow = Join-Path $repositoryRoot '.github/workflows/release-candidate-games.yml'
$cmake = Join-Path $repositoryRoot 'CMakeLists.txt'
if (-not (Test-Path -LiteralPath $workflow -PathType Leaf)) {
    throw "Windows CI workflow is missing: $workflow"
}
if (-not (Test-Path -LiteralPath $linuxWorkflow -PathType Leaf)) {
    throw "Linux CI workflow is missing: $linuxWorkflow"
}
if (-not (Test-Path -LiteralPath $candidateWorkflow -PathType Leaf)) {
    throw "Release candidate game workflow is missing: $candidateWorkflow"
}

$content = Get-Content -LiteralPath $workflow -Raw
$linuxContent = Get-Content -LiteralPath $linuxWorkflow -Raw
$candidateContent = Get-Content -LiteralPath $candidateWorkflow -Raw
$cmakeContent = Get-Content -LiteralPath $cmake -Raw
$requirements = Join-Path $repositoryRoot 'tools/measurement/requirements.txt'
if (-not (Test-Path -LiteralPath $requirements -PathType Leaf)) {
    throw "Python requirements file is missing: $requirements"
}
$requirementsContent = Get-Content -LiteralPath $requirements -Raw

# Ubuntu 24.04 runners do not provide a cutechess apt package. Use a pinned,
# hash-checked upstream CLI bundle rather than relying on the runner image.
if ($candidateContent -match '(?m)^\s*sudo apt-get install[^\r\n]*\bcutechess\b') {
    throw 'Release candidate workflow must not install the unavailable cutechess apt package.'
}
if (-not $candidateContent.Contains('Cute_Chess-1.5.1-x86_64.AppImage') -or
    -not $candidateContent.Contains('d9448693e45bd57f1aeb32c46e94466894cd7cc5b6937effd285a02e871387b5') -or
    -not $candidateContent.Contains('APPIMAGE_EXTRACT_AND_RUN=1') -or
    -not $candidateContent.Contains('cli "$@"')) {
    throw 'Release candidate workflow must run the SHA-256-pinned Cute Chess AppImage CLI without FUSE.'
}

# Bash expands an unquoted $false before PowerShell receives the switch. Keep
# the switch literal in the Linux matrix invocation.
if (-not $candidateContent.Contains("'-OwnBook:`$false'")) {
    throw 'Release candidate workflow must quote the OwnBook false switch for Bash.'
}

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

function Deny-WorkflowPattern([string]$Pattern, [string]$Description) {
    if ($content -match $Pattern) {
        throw "Windows CI workflow must not define $Description."
    }
}

function Require-LinuxWorkflowPattern([string]$Pattern, [string]$Description) {
    if ($linuxContent -notmatch $Pattern) {
        throw "Linux CI workflow must define $Description."
    }
}

function Require-RequirementsPattern([string]$Pattern, [string]$Description) {
    if ($requirementsContent -notmatch $Pattern) {
        throw "tools/measurement/requirements.txt must define $Description."
    }
}

function Deny-RequirementsPattern([string]$Pattern, [string]$Description) {
    if ($requirementsContent -match $Pattern) {
        throw "tools/measurement/requirements.txt must not define $Description."
    }
}

Require-WorkflowPattern '(?m)^\s*runs-on:\s*windows-latest\s*$' 'a Windows runner'
# Four independent jobs: a failing Debug, sanitizer, or differential leg must
# not cancel the primary Release suite, and every leg uploads its own
# diagnostics.
Require-WorkflowPattern '(?m)^\s{2}release-full:\s*$' 'the full Release job'
Require-WorkflowPattern '(?m)^\s{2}debug-smoke:\s*$' 'the Debug smoke job'
Require-WorkflowPattern '(?m)^\s{2}sanitizer:\s*$' 'the AddressSanitizer job'
Require-WorkflowPattern '(?m)^\s{2}shadow-diff:\s*$' 'the differential shadow job'
Require-WorkflowPattern '(?m)^\s*timeout-minutes:\s*60\s*$' 'a Release/Debug job timeout'
Require-WorkflowPattern '(?m)^\s*timeout-minutes:\s*30\s*$' 'a differential job timeout'
Require-WorkflowPattern '(?i)tools/measurement/requirements\.txt' 'the shared Python requirements file install'
Deny-WorkflowPattern '(?i)python-chess' 'the removed python-chess dependency'
Deny-WorkflowPattern '(?i)requirements-elo-oracle' 'the removed requirements-elo-oracle.txt file'
Require-RequirementsPattern '(?i)numpy' 'the numpy dependency'
Deny-RequirementsPattern '(?i)python-chess' 'the removed python-chess dependency'
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
Require-WorkflowPattern '(?m)^\s*ctest\s+--test-dir\s+build/ci-release\s+-C\s+Release\s+-j\s+4\s+--repeat\s+until-pass:2\s+--output-on-failure\s+--output-junit\s+build/ci-release/ctest-junit\.xml\s*$' 'the parallel Release CTest command'
Require-WorkflowPattern '(?m)^\s*ctest\s+--test-dir\s+build/ci-debug\s+-C\s+Debug\s+-j\s+4\s+-LE\s+heavy\s+--repeat\s+until-pass:2\s+--output-on-failure\s+--output-junit\s+build/ci-debug/ctest-junit\.xml\s*$' 'the bounded Debug smoke CTest command'
Require-WorkflowPattern '(?m)^\s*ctest\s+--test-dir\s+build/ci-release\s+-C\s+Release\s+-R\s+koi_shadow_diff_tests\s+--output-on-failure\s*$' 'the differential CTest command'
Require-WorkflowPattern '(?m)^\s*cmake\s+-S\s+\.\s+-B\s+build/ci-asan\s+-G\s+Ninja\s+-DCMAKE_BUILD_TYPE=Debug\s+-DCMAKE_CXX_COMPILER=cl\s+-DKOI_SANITIZE=ON\s*$' 'the AddressSanitizer CMake configure command'
Require-WorkflowPattern '(?m)^\s*ctest\s+--test-dir\s+build/ci-asan\s+-C\s+Debug\s+-j\s+2\s+-L\s+unit\s+-LE\s+heavy\s+--output-on-failure\s+--output-junit\s+build/ci-asan/ctest-junit\.xml\s*$' 'the AddressSanitizer CTest command'
Require-WorkflowPattern '(?i)-DKOI_BUILD_SHADOW_DIFF=ON' 'the opt-in differential configure switch'
Require-CMakePattern '(?i)check_ipo_supported' 'an IPO/LTO capability check'
Require-CMakePattern '(?i)CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE\s+ON' 'portable Release IPO/LTO'
Require-CMakePattern '(?i)/arch:AVX2' 'the AVX2 Release optimization flag'
Require-CMakePattern '(?i)/arch:AVX512' 'the AVX-512 Release optimization flag'
Require-CMakePattern '(?i)TB_NO_HW_POP_COUNT' 'the baseline Fathom popcount fallback'
Require-CMakePattern '(?i)KOI_CPU_SELECTOR' 'the automatic CPU variant selector'
Require-CMakePattern '(?i)KOI_GPU_PTX_ARCHS' 'the embedded GPU PTX architecture list'
Require-CMakePattern '(?i)CONFIG:Release.*:/O2' 'the Release compiler optimization level'
Require-CMakePattern '(?i)CONFIG:Release.*:/DNDEBUG' 'the Release assertion configuration'
Require-CMakePattern '(?i)CONFIG:Release' 'a Release-only AVX2 configuration guard'
Require-CMakePattern '(?i)project\s*\(\s*koi_engine\s+VERSION\s+1\.0\.0' 'the v1.0.0 project identity'
Require-CMakePattern '(?i)CMAKE_MSVC_RUNTIME_LIBRARY' 'the static MSVC runtime policy'
Require-CMakePattern '(?i)FILE_SET\s+CXX_MODULES' 'the C++26 named-module source set'
Require-CMakePattern '(?i)KOI_BUILD_SHADOW_DIFF' 'the opt-in differential test switch'
Require-CMakePattern '(?i)elseif\s*\(\s*UNIX\s*\)' 'the x86-64 Linux platform branch'
Require-CMakePattern '(?i)KOI_BUILD_MODULES' 'the C++26 module auto-detection switch'
Require-CMakePattern '(?i)KOI_STATIC_RUNTIME' 'the static libstdc++/libgcc release policy'
Require-CMakePattern '(?i)koi_arch_compile_option' 'the per-compiler architecture flag mapping'
Require-CMakePattern '(?i)-mavx2' 'the GCC/Clang AVX2 optimization flag'
Require-CMakePattern '(?i)-mavx512f' 'the GCC/Clang AVX-512 optimization flag'
Require-CMakePattern '(?i)KOI_SANITIZE' 'the AddressSanitizer build option'
Require-CMakePattern '(?i)fsanitize=address' 'the MSVC AddressSanitizer flag'

# The Linux legs: GCC, Clang, the modules-off fallback, and the portable tarball.
Require-LinuxWorkflowPattern '(?m)^\s*runs-on:\s*ubuntu-24\.04\s*$' 'an Ubuntu runner'
Require-LinuxWorkflowPattern '(?m)^\s{2}linux-gcc:\s*$' 'the GCC job'
Require-LinuxWorkflowPattern '(?m)^\s{2}linux-clang:\s*$' 'the Clang job'
Require-LinuxWorkflowPattern '(?m)^\s{2}linux-modules-off:\s*$' 'the modules-off job'
Require-LinuxWorkflowPattern '(?m)^\s{2}linux-flake:\s*$' 'the no-retry flake job'
Require-LinuxWorkflowPattern '(?m)^\s{2}linux-tarball:\s*$' 'the portable tarball job'
Require-LinuxWorkflowPattern '(?i)KOI_TEST_RETRIES:\s*"1"' 'the no-retry flake setting'
Require-LinuxWorkflowPattern '(?i)koi_search_tests_\[1-4\]of4' 'the repeated search shards in the flake job'
Require-LinuxWorkflowPattern '(?i)g\+\+-14' 'the GCC 14 compiler'
Require-LinuxWorkflowPattern '(?i)clang-18' 'the Clang 18 compiler'
Require-LinuxWorkflowPattern '(?i)-DCMAKE_CXX_COMPILER=g\+\+-14' 'the GCC C++ compiler selection'
Require-LinuxWorkflowPattern '(?i)-DCMAKE_CXX_COMPILER=clang\+\+-18' 'the Clang C++ compiler selection'
Require-LinuxWorkflowPattern '(?i)-DKOI_BUILD_MODULES=OFF' 'the explicit modules-off configure switch'
Require-LinuxWorkflowPattern '(?i)koi_module_tests' 'the modules-off assertion on the module test'
Require-LinuxWorkflowPattern '(?i)ctest\s+--test-dir\s+build/ci-release\s+-C\s+Release' 'the Linux CTest invocation'
Require-LinuxWorkflowPattern '(?i)ubuntu:22\.04' 'the Ubuntu 22.04 tarball container'
Require-LinuxWorkflowPattern '(?i)tools/build/package_release\.ps1' 'the shared packaging script'
Require-LinuxWorkflowPattern '(?i)tests/integration/packaging/package_release_test\.ps1' 'the packaging manifest and archive smoke test'
Require-LinuxWorkflowPattern '(?i)koi-engine-v1\.0\.0-linux-x86_64\.tar\.gz' 'the Linux tarball name'
Require-LinuxWorkflowPattern '(?i)sha256sum\s+-c' 'archive checksum verification'
Require-LinuxWorkflowPattern '(?i)tar\s+-xzf\s+"\$archive"' 'extraction of the release archive'
Require-LinuxWorkflowPattern '(?i)test\s+-x\s+"\$extracted/\$executable"' 'preservation of executable permissions in the archive'
Require-LinuxWorkflowPattern '(?i)timeout\s+15' 'bounded extracted-engine smoke runs'
Require-LinuxWorkflowPattern '(?i)bestmove\s+\[a-h\]\[1-8\]\[a-h\]\[1-8\]' 'legal bestmove validation for extracted engines'
Require-LinuxWorkflowPattern '(?i)grep\s+-Ec\s+"\^bestmove\s+"' 'exactly one bestmove in each extracted-engine smoke'
Require-LinuxWorkflowPattern '(?i)"\$extracted/koi-replay"\s+startpos\s+moves\s+"\$bestmove"' 'legality validation through the extracted replay executable'
Require-LinuxWorkflowPattern '(?i)grep\s+-q\s+"legal\s+1"' 'acceptance of legal extracted-engine moves'
Require-LinuxWorkflowPattern '(?i)-DKOI_ENABLE_GPU_NNUE=OFF' 'the CPU-only tarball configure switch'
Require-LinuxWorkflowPattern '(?i)KOI_CPU_VARIANT=generic' 'the forced generic UCI smoke'
Require-LinuxWorkflowPattern '(?i)actions/upload-artifact@v5' 'test diagnostic artifact upload'
