# Test layout

Tests are grouped by responsibility while preserving their existing CTest
names and executable target names.

- `unit/` contains core, rules, search, evaluation, and runtime tests.
- `integration/` contains UCI, Cutechess, packaging, tool, and differential
  process tests.
- `python/` contains measurement and NNUE/tuning boundary tests.
- `data/` contains stable opening, position, game, and metadata fixtures.
- `support/` contains the shared C++ harness (`koi_test_support.hpp`), the
  Polyglot book fixture helpers (`polyglot_book_support.hpp`), and the
  PowerShell UCI/match modules (`UciSession.psm1`, `MatchSupport.psm1`).

Temporary test output may use the operating-system temporary directory and must
be cleaned up by the test. Durable reports belong under the repository's
`artifacts/` directory. Every fixture that another process could observe uses a
unique (GUID or process-unique) name so the suite is safe under `ctest -j`.

## Running the suite

Configure and build a Release tree (from an x64 Visual Studio developer shell),
then run CTest, preferably in parallel:

```powershell
cmake -S . -B build\release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=cl
cmake --build build\release --config Release
ctest --test-dir build\release -C Release -j 8 --output-on-failure
```

`tools/test/run_tests.ps1` wraps that flow (build + parallel CTest + JUnit and
`LastTest.log` capture). When `cl.exe` is not on `PATH`, pass the developer
environment bootstrap:

```powershell
pwsh -NoProfile -File .\tools\test\run_tests.ps1 `
  -EnvironmentScript C:\path\to\vcvars64.cmd `
  -Label unit -Parallel 8
```

List the registered tests, or run a single one:

```powershell
ctest --test-dir build\release -N
ctest --test-dir build\release -C Release -R koi_strength_tests --output-on-failure
```

The default Release configuration registers **58 tests** (python-chess
installed, `KOI_BUILD_SHADOW_DIFF=OFF`). The count varies
with optional dependencies: `elo_oracle_python` requires python-chess,
`koi_shadow_diff_tests` requires `-DKOI_BUILD_SHADOW_DIFF=ON`, and
`cutechess_stability_smoke` is only registered when `cutechess-cli.exe` is
found. `cutechess_stability_diagnostics` is always registered and exercises the
fabricated-engine harness.

The previously monolithic `koi_search_tests` CTest entry is now four shards
(`koi_search_tests_1of4` … `koi_search_tests_4of4`) so the heaviest suite
parallelizes with the rest of the run. `ctest -R koi_search_tests` still
matches all four.

## The C++ harness

Every C++ test executable uses `tests/support/koi_test_support.hpp`: it defines
`koi::test::TestCase`, `koi::test::run_tests(tests, argc, argv, options)`,
`require`/`require_value`, `fixture_path`, environment helpers, and
`TempDirectory`. `run_tests` provides:

- one result line per case: `PASS`, `FAIL`, `XFAIL`, `XPASS`, `SKIP`, plus
  `XFAIL-UNSEEN` for known-failure entries that were not selected; and a final
  `koi-test-summary run=.. pass=.. fail=.. xfail=.. xpass=.. skip=.. unseen=.. intermittent=..`
  line.
- selection: `--filter=<substring>`, `--shard=<i>/<n>` (0-based shard index),
  `--list`, `--quiet`; environment equivalents `KOI_TEST_FILTER` and
  `KOI_TEST_SHARD`.
- retries for cases declared timing-sensitive: attempts come from
  `KOI_TEST_RETRIES` (default 1); a retried case must pass one attempt.
- an unexpected pass (`XPASS`) **fails the run**, so fixed known-failure entries
  are removed promptly. `KOI_ALLOW_XPASS=1` is available for triage only, and
  cases declared `intermittent` are exempt in both directions (see
  "Known-failing behavior tests").
- real skips (`koi::test::skip`) are recorded as `SKIP` instead of silently
  passing.

Per-case examples:

```powershell
$env:KOI_TEST_FILTER = "medium timed forcing root"
.\build\release\koi_search_tests.exe
```

## Inventory

C++ unit / integration tests (27 executables under `tests/unit/` and
`tests/integration/`):

- Rules and state: `koi_core_tests`, `koi_rules_tests`, `native_rule_state_tests`,
  `perft_tests`, `koi_shadow_diff_tests` (opt-in via `KOI_BUILD_SHADOW_DIFF`).
- Evaluation: `evaluation_boundary_tests`, `evaluation_architecture_tests`,
  `nnue_boundary_tests`, `classical_evaluator_tests`, `evaluation_features_tests`.
- Search: `koi_search_tests` (four CTest shards), `search_ordering_tests`,
  `search_architecture_tests`, `search_policy_tests`, `search_runtime_tests`,
  `search_service_tests`, `static_exchange_tests`, `time_manager_tests`,
  `transposition_table_tests`, `completion_gate_tests`, `koi_strength_tests`.
- Runtime and boundaries: `koi_cpu_features_tests`, `koi_module_tests`,
  `syzygy_tablebase_tests`, `opening_book_tests`, `uci_controller_tests`,
  `koi_replay_tests`.

PowerShell process tests (`tests/integration/**/*.ps1`), driven through
`pwsh` with the built engine path:

- `koi_engine_process`, `koi_engine_en_croissant_process`,
  `koi_engine_time_safety_process`, `koi_benchmark_process`,
  `koi_uci_match_process`, `koi_stockfish_strength_option`, `koi_uci_match_clock`,
  `cutechess_stability_diagnostics`, `cutechess_stability_smoke`,
  `hash_memory_stability`, `windows_ci_configuration`, `install_book_script`,
  `package_release_layout`.

Python tooling tests (`tests/python/**/*.py`), run as `python -m unittest` with
the repository root as the working directory:

- Measurement: `elo_oracle_python`, `elo_estimate_python`, `stockfish_match_python`,
  `elo_openings_python`, `measurement_phase_python`, `measurement_forensics_python`.
- Evaluation/NNUE: `tune_eval_python`, `nnue_training_python`, `nnue_wrapper_python`,
  `strength_report_python`, `koi_dataset_python`, `gen_training_data_python`,
  `koi_trainer_python` (cross-language v4 parity against the boundary executable),
  `tune_classical_python` (ridge-fit recovery plus a `koi-eval-features` CSV
  round trip when the Release tool is built),
  `nnue_studio_python` (progress parser, backend CLI contract, GUI smoke, and a
  torch-gated tiny training selftest), `nnue_studio_ui_python` (run-list and
  telemetry helpers, validation gating, network naming, numeric guards), and
  `bullet_data_python` (bulletformat conversion, exporter weight layout, and
  wrapper line parsing).

## Labels and scheduling

Every test carries CTest labels; combine them with `-L`/`-LE`, for example
`ctest -L unit -LE heavy` for the fast unit set.

- `unit` — the fast C++ suites (plus the search shards and strength gate, which
  are also `heavy`).
- `evaluation`, `search`, `runtime`, `rules` — focused subsets.
- `integration` / `tools` — the tool-level tests.
- `process` — tests that launch engine or tool processes; sub-labels `uci`,
  `matches`, `tools`, `runtime`, `packaging`.
- `python` — Python unittest suites (`elo_oracle_python` is also `measurement`).
- `heavy` — long-running tests. `koi_search_tests_*of4`, `koi_strength_tests`,
  `koi_engine_time_safety_process`, `koi_benchmark_process`, and
  `cutechess_stability_smoke`. `koi_uci_match_clock` and the heavy tests declare
  `PROCESSORS 2` so an oversubscribed `ctest -j` still schedules them sanely.
- `koi_engine_process` declares `RUN_SERIAL TRUE` because it installs a
  temporary `book.bin` next to the engine binary.

## Known-failing behavior tests

The search implementation is mid-refactor: some behavior tests encode the
intended behavior of the root-forcing-extension and threading work that is not
yet complete. Rather than abort the run on the first mismatch, `koi_search_tests`
maintains a `known_failures` list and reports those entries as `XFAIL` while the
suite stays green:

- A listed test that fails prints `XFAIL` and does not fail the run.
- A listed test that passes prints `XPASS` and **does** fail the run, so an
  entry is removed as soon as the engine is fixed. `KOI_ALLOW_XPASS=1`
  downgrades that back to informational output during triage.
- `XFAIL-UNSEEN` reports a listed name that was not selected by the current
  filter; it is only fatal on an unfiltered, unsharded run.
- Any other failure prints `FAIL` and fails the run.

Current entries: `single-PV root forcing extension`, `depth-one forcing check`,
`threaded depth-one forcing check`, `root king safety escape`, `threaded multipv
ordered root ties`, `threaded multipv warmed hash`, `sparse phase-rich null
safety`, `king-zone LMR exclusion`, `opening central break`, `late move
full-depth verification`, `committed PGN tactical fixtures`, and `poisoned
capture quiescence`.

A second, deliberately tiny list (`intermittent`) holds cases whose outcome
flips with host scheduling. Both their `XFAIL` and `XPASS` are reported but
neither is fatal, so the suite stays deterministic while the gap stays visible;
the goal is to make each deterministic and move it back to `known_failures`
(or delete it once the engine is fixed). Current entries: `incomplete root
forcing fallback`, `threaded short forcing root research` (scheduling-dependent),
and `timed poisoned capture` (configuration-dependent).

Remove an entry once the corresponding engine behavior is reliably fixed.

## Determinism

- `Threads = 1` is the deterministic configuration. `Threads > 1` runs Lazy
  SMP: helper threads search the same root against the shared transposition
  table, so threaded results are intentionally nondeterministic. Threaded
  cases therefore either pin thread-count-independent invariants (legality,
  coverage, completed depth) or live in the `known_failures`/`intermittent`
  lists when they assert a specific move or score.
- Timing-sensitive cases are declared via `TestRunOptions::timing_sensitive`.
  With `KOI_TEST_RETRIES=2` (or `tools/test/run_tests.ps1`'s
  `-RepeatUntilPass`, which adds `--repeat until-pass:2` at the CTest level)
  they are retried before failing.
- The short-oracle rook-lift case no longer runs a 100 ms clocked search; it
  asserts the same reviewed move with a deterministic depth-2 search
  (`f7g8`-family rejection at depth 2, threads 1).
- A test-only virtual clock seam was evaluated and deliberately not added: the
  search runner reads `std::chrono::steady_clock` directly across many call
  sites, so a seam would either not control the assertions it was meant for or
  would require production changes. Instead the two structural offenders were
  made deterministic and the remaining timing cases use the retry mechanism.
  See the 2026-09-17 verification record for the full rationale.

## Environment variables

- `KOI_TEST_FILTER` — substring filter honored by every C++ harness executable.
- `KOI_TEST_SHARD` — `i/n` shard selection equivalent to `--shard`.
- `KOI_TEST_RETRIES` — attempts for timing-sensitive cases (default 1).
- `KOI_ALLOW_XPASS` — set to `1` to downgrade XPASS from fatal to informational.
- `KOI_TEST_TIMEOUT_SECONDS` — base CTest timeout; explicit process-test
  timeouts are multiplied by `KOI_TIMEOUT_SCALE` (CMake cache variable,
  default `auto`: 3 for Debug builds, 1 otherwise; override with
  `-DKOI_TIMEOUT_SCALE=N`).
- `KOI_UCI_TIMEOUT_MS` — per-line timeout used by the PowerShell UCI process
  tests (`uci_process_test.ps1`, `en_croissant_uci_test.ps1`). Defaults to 15000 ms.
- `KOI_REPLAY_PATH` — path to `koi-replay` passed by CMake to `elo_openings_python`.
- `KOI_NNUE_BOUNDARY_EXE` — optional boundary executable used by
  `koi_trainer_test.py` (wired by CMake) and optionally by
  `nnue_training_test.py`.
- `PYTHONDONTWRITEBYTECODE=1` — set by CMake for all Python tests so no
  `__pycache__` directories are produced.

## Optional dependencies and skips

- `python-chess` enables `elo_oracle_python`; without it that test is not
  registered and `measurement_forensics_python` self-skips its legality case.
- PyTorch is required only by the NNUE training boundary test; it skips when the
  package is absent.
- `cutechess_stability_smoke` is registered only when `cutechess-cli.exe` is
  found; otherwise CMake prints a status message and the real smoke is omitted.
  `cutechess_stability_diagnostics` always runs.
- `koi_shadow_diff_tests` is built only when `KOI_BUILD_SHADOW_DIFF=ON`.
- When the Python 3 interpreter is unavailable, all Python tests are skipped.

## Timeouts

CMake sets explicit per-test `TIMEOUT` values through `koi_set_timeout(...)`
(for example `koi_engine_process` 60s, `koi_engine_en_croissant_process` 30s,
`koi_engine_time_safety_process` 180s, `koi_benchmark_process` 300s,
`koi_uci_match_clock` 120s, `cutechess_stability_smoke` 180s) and applies a
default `KOI_TEST_TIMEOUT_SECONDS` timeout to every test that does not declare
one. All explicit values scale with `KOI_TIMEOUT_SCALE`, so Debug trees get 3x
headroom automatically. The PowerShell scripts also enforce their own per-line
timeout, configurable through `KOI_UCI_TIMEOUT_MS`.

## Fixtures

- `data/openings/` — opening corpora (`openings-basic.txt`, `openings-curated-32.txt`).
- `data/positions/` — evaluation and forensic position sets.
- `data/endgames/` — endgame regression positions (`endgame-positions.txt`)
  consumed by `classical_evaluator_tests`, which checks perspective symmetry,
  the bounded endgame scale factors, and dead-position totals.
- `data/games/` — reference PGNs plus `manifest.json`; the manifest records the
  authoritative SHA-256 and size of each PGN. Most PGNs are reference material
  rather than runtime inputs for a specific test.
- `data/uci/handshake.txt` — the single-source UCI handshake transcript used by
  both `uci_controller_tests` and `uci_process_test.ps1`. `{max_threads}` and
  `{empty}` placeholders are substituted by the consumers.

## Continuous integration

`.github/workflows/windows.yml` has three independent jobs (no `fail-fast`
cancellation):

- `release-full` installs `python-chess`, builds Release, runs the parallel
  suite (`ctest -C Release -j 4 --output-junit ...`), and uploads the JUnit
  report plus `LastTest.log`.
- `debug-smoke` builds Debug and runs the fast subset (`-LE heavy`) with the
  3x scaled timeouts.
- `shadow-diff` builds with `-DKOI_BUILD_SHADOW_DIFF=ON` and runs
  `koi_shadow_diff_tests`.
