# Task 4 Report: Strength options, tooling, CI, and documentation

## Scope

Implemented Task 4 from `task-4-brief.md` without changing the architecture
plan, design spec, or SDD ledger. Existing untracked architecture plan/spec
files were preserved and excluded from staging.

The public rules API remains unchanged and no public Koi header includes
`chess.hpp`. `Hash` remains a UCI spin option with default 16 MB and range
1-4096 MB, `Clear Hash` remains a button, and `Threads` remains unadvertised.

## RED evidence

Baseline was built and tested in the x64 Visual Studio developer environment
with the bundled CMake 3.31.6:

```text
ctest --test-dir out/release-vs -C Release --output-on-failure
100% tests passed, 0 tests failed out of 6
```

Focused Task 4 tests were then added before production implementation:

- `search_ordering_tests` failed to compile with expected missing-feature
  error `C1083: Cannot open include file: 'koi/detail/search_ordering.hpp'`.
- `koi_benchmark_process` failed because `koi-bench.exe` did not exist.
- `windows_ci_configuration` failed because `.github/workflows/windows.yml`
  did not exist.
- The strengthened Hash/Clear Hash regression coverage passed immediately in
  the existing implementation: `uci_controller_tests` and
  `koi_search_tests` were 2/2 green. This confirms Task 3's already-shipped
  UCI contract rather than claiming a new behavior was absent.

## GREEN implementation and evidence

### Search ordering

- Added private `koi::detail::SearchMoveOrdering`, owned by `SearchContext`.
- TT moves precede all other candidates; captures use MVV-LVA, then promotion
  value; quiet moves use two killers and color-specific history scores.
- Equal scores use UCI-coordinate ordering, so move order remains stable.
- Static-exchange evaluation was intentionally not added because it is
  explicitly optional; no public search or rules API was changed.

### Tooling and CI

- Added `koi-bench`, a separate deterministic depth-3 benchmark executable.
  It writes only benchmark text to stdout and no UCI protocol text.
- Added a process test that rejects UCI-shaped benchmark output and stderr.
- Added `.github/workflows/windows.yml` with a fail-fast Debug/Release matrix,
  x64 `VsDevCmd.bat`, an x64 compiler check, Ninja/CMake configuration, build,
  and CTest.
- Added a CTest that verifies the workflow's required x64, Debug/Release,
  fail-fast, compiler, and CTest configuration.
- Updated README architecture, search-limit, UCI-option, Debug/Release,
  perft, benchmark, and Lucas acceptance documentation.

Focused GREEN check:

```text
ctest --test-dir out/release-vs -C Release -R
  "search_ordering_tests|koi_benchmark_process|windows_ci_configuration"
100% tests passed, 0 tests failed out of 3
```

Complete x64 MSVC verification:

```text
Release: ctest --test-dir out/release-vs -C Release --output-on-failure
100% tests passed, 0 tests failed out of 9

Debug: ctest --test-dir out/debug-vs -C Debug --output-on-failure
100% tests passed, 0 tests failed out of 9
```

Two direct Release benchmark runs were identical:

```text
Koi benchmark
position startpos depth 3 nodes 651 qnodes 576 tt_hits 22 score 50 move b1c3
position tactics depth 3 nodes 2491 qnodes 6284 tt_hits 46 score -60 move c4b3
```

`git diff --check` completed without whitespace errors. The public-header scan
`rg -n "chess\\.hpp" src\\koi -g "*.hpp" -g "*.h"` returned no matches.

## Changed files

- `.github/workflows/windows.yml`
- `CMakeLists.txt`
- `README.md`
- `src/koi/detail/search_ordering.hpp`
- `src/koi/search_ordering.cpp`
- `src/koi/search_service.cpp`
- `tools/koi_bench.cpp`
- `tests/search_ordering_tests.cpp`
- `tests/koi_bench_process_test.ps1`
- `tests/ci_configuration_test.ps1`
- `tests/koi_search_tests.cpp`
- `tests/uci_controller_tests.cpp`

## Lucas Chess GUI acceptance limitation

Lucas Chess R 6.0.4 is installed locally. I began the requested GUI check, but
the desktop automation reported an inconsistent target after opening the UI;
its screenshot no longer matched the Lucas window returned by accessibility.
I stopped before registering the executable or playing a game to avoid changing
the wrong application. The automated UCI process tests are green, but a human
still needs to register `out\\release-vs\\koi-engine.exe` in Lucas Chess and
play a short game, confirming a legal response to `go`, responsiveness while
thinking, and no duplicate `bestmove` after stop/new-game.
