# Contributing to Koi Engine

Koi Engine is a Windows x64 UCI chess engine written in C++26, built with MSVC and CMake/Ninja. Contributions are welcome.

## Prerequisites

- Windows x64 with an x64 MSVC toolchain. Run commands from an **x64 Native Tools Command Prompt** or **x64 Developer PowerShell**. GCC/Clang/MinGW are not supported.
- CMake 3.31+
- Ninja
- PowerShell (`pwsh` or `powershell`)
- Optional: Python 3 for measurement tooling and Python tests:
  `python -m pip install -r .\tools\measurement\requirements-elo-oracle.txt`

## Build

```powershell
cmake -S . -B build\release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=cl
cmake --build build\release --config Release
```

Debug builds: substitute `build\debug` and `Debug`. The engine binary is `build\release\koi-engine.exe`.

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

- Do not weaken or delete assertions or `XFAIL` entries to get a green run; an unexpected pass (`XPASS`) fails the run by design.
- `Threads = 1` is the deterministic configuration. Threaded tests must assert invariants, not byte equality.
- Full release gate: `.\tools\build\release_verify.ps1`.

## Making changes

- Branch from `koi-engine-v1` (for example `codex/my-change`).
- Commit with a descriptive imperative message (for example `Fix mate score off-by-one at root`).
- Open a PR. CI (`.github/workflows/windows.yml`) runs Release CTest, a Debug smoke subset, and the shadow-diff job.

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
