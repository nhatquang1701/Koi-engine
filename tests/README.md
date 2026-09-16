# Test layout

Tests are grouped by responsibility while preserving their existing CTest
names and executable target names.

- `unit/` contains core, rules, search, evaluation, and runtime tests.
- `integration/` contains UCI, Cutechess, packaging, tool, and differential
  process tests.
- `python/` contains measurement and NNUE/tuning boundary tests.
- `data/` contains stable opening, position, game, and metadata fixtures.

Temporary test output may use the operating-system temporary directory and must
be cleaned up by the test. Durable reports belong under the repository's
`artifacts/` directory.

## Running the suite

Configure and build a Release tree (from an x64 Visual Studio developer shell),
then run CTest:

```powershell
cmake -S . -B build\release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=cl
cmake --build build\release --config Release
ctest --test-dir build\release -C Release --output-on-failure
```

List the registered tests, or run a single one:

```powershell
ctest --test-dir build\release -N
ctest --test-dir build\release -C Release -R koi_strength_tests --output-on-failure
```

The checkout registers 45 CTest registrations when the opt-in shadow-diff
target is enabled: 44 by default with python-chess, 43 without it
(`elo_oracle_python` is gated on `python-chess`), and 45 with
`-DKOI_BUILD_SHADOW_DIFF=ON`, which CI turns on.

Each C++ test is a standalone executable with its own `main`; there is no shared
runner. `koi_search_tests` and `uci_controller_tests` honor the
`KOI_TEST_FILTER` environment variable, which runs every test whose name
contains the given substring:

```powershell
$env:KOI_TEST_FILTER = "medium timed forcing root"
.\build\release\koi_search_tests.exe
```

## Inventory

C++ unit / integration tests (built as executables under `tests/unit/` and
`tests/integration/`):

- Rules and state: `koi_core_tests`, `koi_rules_tests`, `native_rule_state_tests`,
  `perft_tests`, `koi_shadow_diff_tests` (opt-in via `KOI_BUILD_SHADOW_DIFF`).
- Evaluation: `evaluation_boundary_tests`, `evaluation_architecture_tests`,
  `nnue_boundary_tests`.
- Search: `koi_search_tests`, `search_ordering_tests`, `search_architecture_tests`,
  `search_policy_tests`, `search_runtime_tests`, `static_exchange_tests`,
  `time_manager_tests`, `completion_gate_tests`, `koi_strength_tests`.
- Runtime and boundaries: `koi_cpu_features_tests`, `koi_module_tests`,
  `syzygy_tablebase_tests`, `opening_book_tests`, `uci_controller_tests`,
  `koi_replay_tests`.

PowerShell process tests (`tests/integration/**/*.ps1`), driven through
`pwsh`/`powershell` with the built engine path:

- `koi_engine_process`, `koi_engine_en_croissant_process`,
  `koi_engine_time_safety_process`, `koi_benchmark_process`,
  `koi_uci_match_process`, `koi_stockfish_strength_option`, `koi_uci_match_clock`,
  `cutechess_stability_smoke`, `hash_memory_stability`, `windows_ci_configuration`,
  `install_book_script`, `package_release_layout`.

Python tooling tests (`tests/python/**/*.py`), run as `python -m unittest` with
the repository root as the working directory:

- Measurement: `elo_oracle_python`, `elo_estimate_python`, `stockfish_match_python`,
  `elo_openings_python`, `measurement_phase_python`, `measurement_forensics_python`.
- Evaluation/NNUE: `tune_eval_python`, `nnue_training_python`, `nnue_wrapper_python`,
  `strength_report_python`.

## Known-failing behavior tests

The search implementation is mid-refactor: some behavior tests encode the
intended behavior of the root-forcing-extension and threading work that is not
yet complete. Rather than abort the run on the first mismatch, `koi_search_tests`
maintains a `known_failures` list and reports those entries as `XFAIL` while the
suite stays green:

- A test in the list that fails prints `XFAIL` and does not fail the run.
- A test in the list that passes prints `XPASS` (reported, non-fatal, because a
  few of these tests are timing/threading-sensitive and pass intermittently).
- Any other failure prints `FAIL` and fails the run.

Current entries: `single-PV root forcing extension`, `incomplete root forcing
fallback`, `timed poisoned capture`, `depth-one forcing check`, `threaded
depth-one forcing check`, `root king safety escape`, `threaded root-in-check
parity`, `threaded multipv ordered root ties`, `threaded multipv warmed hash`,
`sparse phase-rich null safety`, `king-zone LMR exclusion`, `opening central
break`, `late move full-depth verification`, `committed PGN tactical fixtures`,
`threaded short forcing root research`, `poisoned capture quiescence`.

Remove an entry once the corresponding engine behavior is reliably fixed; a
green run with zero `XFAIL` lines means the list is empty.

## Environment variables

- `KOI_TEST_FILTER` — substring filter for `koi_search_tests` and
  `uci_controller_tests` test selection.
- `KOI_UCI_TIMEOUT_MS` — per-line timeout used by the PowerShell UCI process
  tests (`uci_process_test.ps1`, `en_croissant_uci_test.ps1`). Defaults to 15000 ms.
- `KOI_REPLAY_PATH` — path to `koi-replay` passed by CMake to `elo_openings_python`.
- `KOI_NNUE_BOUNDARY_EXE` — optional boundary executable used by
  `nnue_training_test.py`.
- `PYTHONDONTWRITEBYTECODE=1` — set by CMake for all Python tests so no
  `__pycache__` directories are produced.

## Optional dependencies and skips

- `python-chess` enables `elo_oracle_python`; without it that test is not
  registered and `measurement_forensics_python` self-skips its legality case.
- PyTorch is required only by the NNUE training boundary test; it skips when the
  package is absent.
- `cutechess_stability_smoke` skips when a Cutechess or Stockfish executable is
  not available.
- `koi_shadow_diff_tests` is built only when `KOI_BUILD_SHADOW_DIFF=ON`.
- When the Python 3 interpreter is unavailable, all Python tests are skipped.

## Timeouts

CMake sets explicit per-test `TIMEOUT` values for the process tests (for example
`koi_engine_process` 60s, `koi_engine_en_croissant_process` 30s,
`koi_engine_time_safety_process` 180s, `koi_benchmark_process` 300s,
`koi_uci_match_clock` 120s) and applies a default
600s timeout to every test that does not declare one. The PowerShell scripts
also enforce their own per-line timeout, configurable through
`KOI_UCI_TIMEOUT_MS`.

## Fixtures

- `data/openings/` — opening corpora (`openings-basic.txt`, `openings-curated-32.txt`).
- `data/positions/` — evaluation and forensic position sets.
- `data/games/` — reference PGNs plus `manifest.json`; the manifest records the
  authoritative SHA-256 and size of each PGN. Most PGNs are reference material
  rather than runtime inputs for a specific test.

## Continuous integration

`.github/workflows/windows.yml` configures Debug and Release Ninja/MSVC trees via
`vswhere`/`VsDevCmd`, builds them, and runs `ctest --output-on-failure` without
installing Python packages. Runs therefore rely on the optional-dependency skips
above; `koi_shadow_diff_tests` is not built in CI.
