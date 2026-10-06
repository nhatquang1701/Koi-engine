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

Configure and build a Release tree (from an x64 Visual Studio developer shell
on Windows, with the system GCC 14+/Clang 18+ on Linux, or with Homebrew LLVM
clang 19+ on macOS arm64), then run CTest, preferably in parallel:

```powershell
cmake -S . -B build\release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=cl
cmake --build build\release --config Release
ctest --test-dir build\release -C Release -j 8 --output-on-failure
```

```bash
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=gcc-14 -DCMAKE_CXX_COMPILER=g++-14
cmake --build build/release --config Release
ctest --test-dir build/release -C Release -j 8 --output-on-failure
```

On macOS arm64 (Apple Silicon), use Homebrew LLVM clang 19+ (or AppleClang
18+) with `-DKOI_BUILD_MODULES=OFF -DKOI_STATIC_RUNTIME=OFF`; the CTest
commands are identical. GPU NNUE is CUDA-only and unavailable on macOS, so the
CPU NNUE and classical evaluator paths are exercised there.

The PowerShell process tests are registered only when `pwsh` is found; the C++
and Python suites are registered on Windows, Linux, and macOS.

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

The current Windows Release configuration with Python 3 registers **81 tests**
(`KOI_BUILD_SHADOW_DIFF=OFF`). The count varies
with optional dependencies:
`koi_shadow_diff_tests` requires `-DKOI_BUILD_SHADOW_DIFF=ON`, and
`cutechess_stability_smoke` is only registered when `cutechess-cli.exe` is
found. `cutechess_stability_diagnostics` is always registered and exercises the
fabricated-engine harness. The Windows-only packaging and stability tests are
registered only on Windows, and `koi_module_tests` is registered only when
C++26 named modules are built (pass `-DKOI_BUILD_MODULES=OFF` to skip them).

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
  `XFAIL-UNSEEN` for known-failure entries that are missing or were selected
  but did not run; and a final
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

C++ unit / integration executables under `tests/unit/` and `tests/integration/`:

- Rules and state: `koi_core_tests`, `koi_rules_tests`, `native_rule_state_tests`,
  `perft_tests`, `koi_shadow_diff_tests` (opt-in via `KOI_BUILD_SHADOW_DIFF`).
- Evaluation: `evaluation_boundary_tests`, `evaluation_architecture_tests`,
  `nnue_boundary_tests`, `classical_evaluator_tests`, `evaluation_features_tests`.
- Search: `koi_search_tests` (four CTest shards), `search_ordering_tests`,
  `search_architecture_tests`, `search_policy_tests`, `search_runtime_tests`,
  `search_service_tests`, `static_exchange_tests`, `time_manager_tests`,
  `transposition_table_tests`, `completion_gate_tests`, `koi_strength_tests`,
  `koi_soak_tests` (bounded stability soak: long replays past the snapshot
  window, hash resizes under a live search, ponder cycles, and a thread-count
  sweep),
  `gpu_nnue_tests` (GPU NNUE bit-exact parity plus the PTX variant table; the
  parity cases skip without a CUDA device).
- Runtime and boundaries: `koi_cpu_features_tests`, `koi_cpu_variant_tests`
  (variant parsing, executable names, automatic selection, and the override
  rules), `koi_module_tests`,
  `syzygy_tablebase_tests`, `opening_book_tests`, `uci_controller_tests`
  (including legal MultiPV/WDL, searchmoves, and lifecycle cases),
  `koi_replay_tests`.

PowerShell process tests (`tests/integration/**/*.ps1`), driven through
`pwsh` with the built engine path:

- `koi_engine_process`, `koi_engine_perft_responsiveness`,
  `koi_engine_variant_process` (three engine binaries,
  automatic selection, and the `KOI_CPU_VARIANT` overrides),
  `koi_engine_en_croissant_process`,
  `koi_engine_time_safety_process`, `koi_benchmark_process`,
  `koi_uci_match_process`, `koi_stockfish_strength_option`, `koi_uci_match_clock`,
  `cutechess_stability_diagnostics`, `cutechess_stability_smoke`,
  `run_tests_artifact_path`, `hash_memory_stability`, `windows_ci_configuration`,
  `install_book_script`,
  `package_release_layout`.

Python tooling tests (`tests/python/**/*.py`), run as `python -m unittest` with
the repository root as the working directory:

- Measurement: `elo_oracle_python`, `elo_estimate_python`, `stockfish_match_python`,
  `elo_openings_python`, `measurement_phase_python`, `measurement_forensics_python`.
- Release evidence: `release_candidate_games_python` validates recorded game
  artifacts, `release_strength_openings_python` checks the fixed opening sample,
  and `strength_bound_python` checks the paired strength decision and provenance.
- Evaluation/NNUE: `tune_eval_python`, `nnue_training_python`, `nnue_wrapper_python`,
  `strength_report_python`, `koi_dataset_python` (koi-dataset-v1 and v2 encoders
  with four-group records), `gen_training_data_python`,
  `koi_trainer_python` (cross-language v4 and v5 parity against the boundary
  executable),
  `tune_classical_python` (ridge-fit recovery plus a `koi-eval-features` CSV
  round trip when the Release tool is built), and
  `bullet_data_python` (bulletformat conversion, exporter weight layouts for v4
  and v5, and wrapper line parsing).
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
- `koi_engine_process` and `koi_engine_perft_responsiveness` declare
  `RUN_SERIAL TRUE`; the former installs a temporary `book.bin` next to the
  engine binary, and the latter runs a cancellable deep perft process.

## Search failure reporting

`koi_search_tests` retains `known_failures` and `intermittent` registries for
explicit triage. Both registries are empty for the 1.0.0 full-release gate, so
every search behavior failure is fatal. The harness reports:

- A listed test that fails prints `XFAIL` and does not fail the run.
- A listed test that passes prints `XPASS` and **does** fail the run, so an
  entry is removed as soon as the engine is fixed. `KOI_ALLOW_XPASS=1`
  downgrades that back to informational output during triage.
- `XFAIL-UNSEEN` reports a listed name that no longer exists in the registry, or
  one whose case the current filter and shard select but that did not run; both
  are fatal, so the four search shards still keep the list honest.
- Any other failure prints `FAIL` and fails the run.

The former 12 known failures and three intermittent cases now run as ordinary
tests. Fixed-depth tactical assertions retain reviewed moves or tactical
properties; clocked and threaded cases assert legal, complete, coherent
results under the schedule they actually receive.

## Determinism

- `Threads = 1` is the deterministic configuration. `Threads > 1` runs Lazy
  SMP: helper threads search the same root against the shared transposition
  table, so threaded results are intentionally nondeterministic. Threaded
  cases therefore pin thread-count-independent invariants such as legality,
  coverage, completed depth, ranked MultiPV lines, and a coherent bestmove/PV.
- Timing-sensitive cases are declared via `TestRunOptions::timing_sensitive`.
  With `KOI_TEST_RETRIES=2` (or `tools/test/run_tests.ps1`'s
  `-RepeatUntilPass`, which adds `--repeat until-pass:2` at the CTest level)
  they are retried before failing. The main Release CI job sets
  `KOI_TEST_RETRIES=3` for shared-runner tolerance; independent Windows and
  Linux flake jobs and the local full-release gate use `KOI_TEST_RETRIES=1`.
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

- The in-tree `koi_chess` package supplies the rules, PGN, and UCI clients for
  `elo_oracle_python`, `koi_dataset_python`, and the NNUE exporter tests; no
  third-party chess package is required.
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

`.github/workflows/windows.yml` has four independent jobs (no `fail-fast`
cancellation):

- `release-full` installs the measurement requirements (`numpy`), builds Release, runs the parallel
  suite (`ctest -C Release -j 4 --output-junit ...`), and uploads the JUnit
  report plus `LastTest.log`.
- `debug-smoke` builds Debug and runs the fast subset (`-LE heavy`) with the
  3x scaled timeouts.
- `sanitizer` builds Debug with `-DKOI_SANITIZE=ON` (MSVC
  `/fsanitize=address`, dynamic CRT, no LTO) and runs the fast subset, so
  memory errors and use-after-free regressions fail CI.
- `shadow-diff` builds with `-DKOI_BUILD_SHADOW_DIFF=ON` and runs
  `koi_shadow_diff_tests`.

`.github/workflows/linux.yml` runs the same suite on Linux:

- `linux-gcc` and `linux-clang` build Release with GCC 14 and Clang 18 and run
  the full CTest suite.
- `linux-modules-off` configures with `-DKOI_BUILD_MODULES=OFF`, asserts that
  the module test is not registered, and runs the unit subset.
- `linux-flake` repeats the search, transposition-table, controller, service,
  and time-manager suites three times with `KOI_TEST_RETRIES=1` and no
  `--repeat`, so a first-attempt flake cannot hide behind a retry.
- `linux-tarball` builds inside an Ubuntu 22.04 container, packages the
  portable `koi-engine-v1.0.0-linux-x86_64.tar.gz` with
  `tools/build/package_release.ps1`, verifies and extracts the archive, then
  runs bounded UCI smokes with the automatic and `KOI_CPU_VARIANT=generic`
  variants.

`.github/workflows/macos.yml` runs one Apple Silicon leg:

- `macos-arm64` installs Homebrew LLVM clang and builds Release with
  `-DKOI_BUILD_MODULES=OFF -DKOI_ENABLE_GPU_NNUE=OFF -DKOI_STATIC_RUNTIME=OFF`,
  then runs the same unit, heavy-search, process, and Python label groups as
  the Linux legs. The x86-only AVX-512 binary smoke and the three-binary
  variant process test are omitted, and diagnostics upload on failure.

## Local macOS arm64 heavy-test worker

An Apple Silicon Mac (for example a Mac mini M4) can run the heavy search and
process suites, the speed gate, and Elo A/B matches so the Windows machine
stays free for GPU work (Lc0 labeling, NNUE training). Prerequisites: Xcode
command line tools, Homebrew LLVM clang 19+, Ninja, CMake, Python 3, and
PowerShell 7 (`brew install llvm ninja cmake python pwsh`).

Build Release with the documented macOS configuration and drive the suites
through `tools/test/run_tests.ps1`; no MSVC wrapper is needed off Windows:

```bash
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER="$(brew --prefix llvm)/bin/clang" \
  -DCMAKE_CXX_COMPILER="$(brew --prefix llvm)/bin/clang++" \
  -DKOI_BUILD_MODULES=OFF -DKOI_ENABLE_GPU_NNUE=OFF -DKOI_STATIC_RUNTIME=OFF
cmake --build build/release --config Release
pwsh -NoProfile -File ./tools/test/run_tests.ps1 -BuildDirectory build/release -NoBuild
```

The heavy-search group is `-Label heavy -ExcludeLabel process`; the process
tests are registered individually on macOS (`koi_replay_tests`,
`koi_engine_process`, `koi_engine_en_croissant_process`, `koi_uci_match_process`,
`koi_stockfish_strength_option`, `koi_uci_match_clock`,
`koi_engine_perft_responsiveness`) and run serially; the Python group is
`-Label python`.

### Speed gates on macOS

`speed_gate.ps1` joins repository paths with the host separator, so it runs
unchanged under Homebrew `pwsh`. Build the isolated baseline from the same
revision as on Windows (a detached worktree), then launch the gate through
`tools/build/launch_gate_macos.sh`, which wraps `nohup` and `caffeinate -i` so
the run survives the SSH session and the machine does not idle-sleep:

```bash
git worktree add --detach build/<name>-baseline-src <baseline-revision>
# configure and build that tree with the same macOS flags, then:
tools/build/launch_gate_macos.sh \
  -BaselineExecutable build/<name>-baseline-release/koi-bench \
  -CandidateExecutable build/release/koi-bench \
  -OutputDirectory artifacts/verification/speed-gate/<name> \
  -Runs 5 -Threads 1 -DepthSweep 2..5 \
  -FenFile artifacts/verification/speed-gate/<name>/sparse-qsearch-fens.txt
```

The wrapper writes `gate.out.log`, `gate.err.log`, `gate.pid`, and (when the
gate exits) `gate-status.txt` into the gate directory. An arm64 gate measures
the scalar NNUE path; the AVX2 kernels only exist on x86-64, so arm64 and
x86-64 NPS numbers are never compared. Treat the Mac as the fast pre-screen and
keep a Windows x86-64 gate for release claims that touch hot paths. Record the
first `-Runs 2` run as that machine's noise floor.

### Elo A/B matches on macOS

`elo_estimate.py` requires `--stockfish` to match `stockfish.path` in the
anchor manifest, so create a macOS manifest beside the Windows one with the
same anchor ratings (Stockfish 19 `UCI_LimitStrength` at 1400/1600/1800) and a
macOS Stockfish 19 path -- either the official prebuilt binary or a source
build (`make -j profile-build ARCH=apple-silicon`). The underlying
`tools/stability/uci_match.ps1` harness runs under Homebrew `pwsh`:

```bash
python3 tools/measurement/elo_estimate.py \
  --koi build/release/koi-engine \
  --replay build/release/koi-replay \
  --stockfish third_party/stockfish-19-macos/stockfish \
  --anchors artifacts/manifests/elo-anchors-macos.json \
  --openings tests/data/openings/openings-curated-32.txt \
  --movetime-ms 1000 --threads 4 --hash 512 --speed 100 \
  --min-games 192 --max-games 320 --prior-elo 1600 --mode no-book \
  --all-anchors --run-label measurement \
  --output artifacts/elo/<name>/elo.json
```

A/B matches on one machine are valid; absolute Elo at a fixed movetime is
machine-specific and is never compared across platforms.

### Remote operation

Enable Remote Login on the Mac (System Settings > General > Sharing), add the
Windows machine's public key to `~/.ssh/authorized_keys`, and drive the box
over SSH. Long jobs must be detached (`launch_gate_macos.sh` for gates,
`nohup`/`caffeinate` for Elo) and the Mac must not sleep mid-run. Numeric
evidence stays under the ignored `artifacts/` tree and is not published.

### Verified on a Mac mini M4 (macOS 27, AppleClang 21)

The full local flow was exercised on an Apple Silicon Mac mini M4 (10 cores,
16 GiB): Release build (145/145 targets), `ctest -j 8` = 100% of 67,
`run_tests.ps1` heavy (5/5), process (9/9, serial), and Python (24/24) groups
all green with `ctest-junit.xml` and `LastTest.log` written under
`artifacts/verification/test-suite-hardening/`. The first `-Runs 2` gate
measured a same-source noise floor of +0.65% total NPS (about 1.07M NPS on
the sparse corpus; arm64 and x86-64 NPS numbers are never compared), and an
Elo smoke ran 192 anchor games end-to-end and wrote its report.
`elo_estimate.py` accepts `--min-games` only in {128,192,256,320}, and
`--all-anchors` needs at least 192 games.

Two macOS traps are handled in-tree: `CMakeLists.txt` sets
`Python3_FIND_FRAMEWORK=LAST` on APPLE so a PATH Python 3.10+ wins over the
CLT Python 3.9 framework (which lacks `int.bit_count()`), and
`speed_gate.ps1` joins repository paths with the host separator. No
administrator rights are needed: Homebrew cannot install without `sudo` over
SSH, so the verified setup used `pip install --user` (cmake, ninja, numpy),
a standalone Python build, and the PowerShell GitHub tarball under `$HOME`.
