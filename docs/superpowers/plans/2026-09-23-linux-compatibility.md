# Linux compatibility (x86-64, GCC 14+ / Clang 18+)

Status: in progress. Owner: Koi Engine.

## Goal

Koi Engine builds natively on x86-64 Linux with GCC 14+ or Clang 18+, keeps
full feature parity with the Windows build, and ships a portable release
tarball. ARM64 is out of scope.

Scope decisions (confirmed):

- Full parity: engine, C++ tests, Python measurement tooling, PowerShell
  harnesses under `pwsh`, and the NNUE Studio GUI.
- Compilers: GCC 14+ and Clang 18+.
- C++26 modules: auto-detected; the build degrades gracefully to
  `KOI_BUILD_MODULES=OFF` when unsupported.
- Release artifacts: `koi-engine-vX.Y-linux-x86_64.tar.gz` built in an
  Ubuntu 22.04 container with GCC 14 and a static libstdc++/libgcc.
- CPU selection: the same three-binary selector as Windows
  (`koi-engine`, `koi-engine-avx2`, `koi-engine-avx512`) with
  `KOI_CPU_VARIANT`.
- GPU: optional NNUE inference through `dlopen("libcuda.so.1")`, same
  embedded PTX set (`61;75;86;89;120`), same CPU fallback.
- CMake 3.31 remains the minimum; distro instructions cover installing a
  newer CMake.

## Design decisions

- `koi_add_core_target(target arch_option)` takes a semantic arch
  (`avx2`, `avx512`, or empty) and maps it to `/arch:...` on MSVC and
  `-mavx2` / `-mavx512f -mavx512bw -mavx512cd -mavx512dq -mavx512vl` on
  GCC/Clang. The literal `/arch:AVX2` and `/arch:AVX512` strings stay in
  the MSVC branch so the CI configuration test keeps matching.
- Linux self-gating binaries use the portable CPU queries, so the release
  gates are no longer compiled out on GCC/Clang.
- The selector resolves its own path through `koi::current_executable_path()`
  (`/proc/self/exe` on Linux) and re-execs the sibling with `execv`.
- `/STACK:16777216` cannot be reproduced on glibc, so Linux sets a 16 MiB
  default thread stack with `pthread_setattr_default_np()` before threads
  start; `ulimit -s` is documented for the main thread.
- GPU module loading tries the newest embedded PTX the device can run and
  falls back to older modules when a load fails (covers drivers that are
  too old for `compute_120`).

## Phases

1. **Build system portability** (`CMakeLists.txt`, `cmake/`).
2. **Engine source portability** (`src/`): executable path, CPU feature
   detection, selector, stack sizing, memory snapshot, CRLF tolerance.
3. **GPU on Linux**: `libcuda.so.1`, newest-loadable module.
4. **Tooling + Studio GUI**: `studio_core.py`, bullet helpers, build and
   release scripts, POSIX launcher.
5. **Tests**: harness include, CPU feature/variant expectations, process
   tests under `pwsh`, Python artifact paths, CMake registration.
6. **CI**: `.github/workflows/linux.yml` with GCC, Clang, modules-off, and
   tarball jobs; configuration assertions.
7. **Docs + verification**: README/CONTRIBUTING/tests/tools docs, Windows
   regression, CI to green.

## Verification

- Windows: full Release CTest, `release_verify.ps1`, package layout test.
- Linux: CI-only (no local Linux host). GCC and Clang jobs run the full
  CTest suite; the tarball job builds the portable artifact and runs a UCI
  smoke with the automatic and generic variants.
- GPU Linux: structural verification plus the existing CPU fallback; CI
  runners have no GPU.

## Risks

- GCC C++26 modules are experimental; the GCC CI job may run with
  `KOI_BUILD_MODULES=OFF` while the Clang job keeps them enabled.
- `nvcc` 12.9 may reject GCC 15/16 as a host compiler; the GPU build is
  optional and documented.
- No local Linux host means the first CI runs may surface portability
  issues; the plan assumes a few push/fix cycles.
