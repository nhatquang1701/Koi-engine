# Contributing to Koi Engine

Koi Engine is a Windows x64 and Linux x86-64 UCI chess engine written in C++26, built with MSVC or GCC 14+/Clang 18+ and CMake/Ninja. Contributions are welcome.

## Prerequisites

- Windows x64 with an x64 MSVC toolchain, or Linux x86-64 with GCC 14+ or
  Clang 18+.
  - On Windows, run commands from an **x64 Native Tools Command Prompt** or
    **x64 Developer PowerShell**; GCC/Clang/MinGW are not supported there.
  - On Linux, CMake 3.31+ is required but most distributions ship older
    versions, so install a newer CMake (`python3 -m pip install --upgrade cmake`
    or the Kitware APT repository) before configuring.
- CMake 3.31+
- Ninja (required for the C++26 named-module build)
- PowerShell (`pwsh` or `powershell`) for the process tests
- Optional: Python 3 for measurement tooling and Python tests:
  `python -m pip install -r ./tools/measurement/requirements.txt`

## Build

Windows:

```powershell
cmake -S . -B build\release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=cl
cmake --build build\release --config Release
```

Linux (use `clang-18`/`clang++-18` for a Clang build):

```bash
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=gcc-14 -DCMAKE_CXX_COMPILER=g++-14
cmake --build build/release --config Release
```

Debug builds: substitute `build\debug` and `Debug` (or `build/debug` and
`Debug`). C++26 named modules are auto-detected; pass
`-DKOI_BUILD_MODULES=OFF` when the compiler cannot build them. The engine
binary is `build\release\koi-engine.exe` (`build/release/koi-engine` on
Linux). It automatically starts the AVX2 or AVX-512 sibling
(`koi-engine-avx2` / `koi-engine-avx512`) when the CPU supports it, and
`KOI_CPU_VARIANT=generic|avx2|avx512` forces one build. Keep the selector
silent on stdout and stderr; GUIs treat engine chatter as protocol output.

Quick UCI smoke test:

```powershell
@('uci','isready','position startpos','go depth 2','stop','quit') | & .\build\release\koi-engine.exe
```

## Test

```powershell
ctest --test-dir build\release -C Release -j 8 --output-on-failure   # full suite
ctest --test-dir build\release -C Release -R koi_strength_tests --output-on-failure
.\tools\test\run_tests.ps1 -Label unit                                # fast unit subset
```

On Linux the same commands use `build/release` (forward slashes). The
PowerShell process tests run under `pwsh` when it is installed; without it the
C++ and Python suites still run.

- Do not weaken or delete assertions or `XFAIL` entries to get a green run; an unexpected pass (`XPASS`) fails the run by design.
- `Threads = 1` is the deterministic configuration. Threaded tests must assert invariants, not byte equality.
- Full release gate: `.\tools\build\release_verify.ps1`.

## Making changes

- Branch from `koi-engine-v1` (for example `codex/my-change`).
- Commit with a descriptive imperative message (for example `Fix mate score off-by-one at root`).
- Open a PR. CI runs on Windows (`.github/workflows/windows.yml`: Release
  CTest, a Debug smoke subset, and the shadow-diff job) and on Linux
  (`.github/workflows/linux.yml`: GCC 14 and Clang 18 CTest, a modules-off
  build, and a portable tarball job).

## Rules

- Keep the UCI handshake byte-identical unless a change explicitly modifies it; update the handshake fixture, tests, and README together.
- stdout must stay protocol-clean: exactly one `bestmove` per search.
- The classical evaluator stays the default; NNUE remains opt-in with classical fallback.
- Elo, CPL, and NPS figures are reports, never CI thresholds or claims.
- Do not commit build trees, generated corpora, trained networks, `book.bin`, tablebase data, or `.opencode/`. Durable reports go under `artifacts/`.
- Keep third-party licenses intact; the vendored Stockfish oracle (`third_party/stockfish-19`, GPL-3.0) must not be redistributed as part of Koi.

## Documentation

- `README.md` - build, UCI behavior, tooling, measurement
- `tests/README.md` - test inventory, labels, environment variables, determinism policy
- `tools/README.md` - tooling and artifact workflows
- `docs/README.md` - documentation map

## License

Contributions are licensed under the MIT License (`LICENSE`), Copyright (c) 2026 nhatquang1701.
