# Test-suite hardening verification (2026-09-17)

Scope: the harness/parallelization/determinism work described in
`docs/superpowers/plans/2026-09-17-test-suite-hardening.md`.

## Environment

- Host: Intel i3-10100F (4 cores / 8 threads), Windows x64.
- Compiler: MSVC 14.44.35207 via `vcvars64.bat`; Ninja generator driven by
  MSYS2 CMake 4.4.3 (the `cmake`/`ctest` on `PATH` are MinGW 3.27.1 and run
  the existing trees fine).
- Release tree: `build/release`, `KOI_BUILD_SHADOW_DIFF=ON`, python-chess
  1.11.2 (Python 3.14), cutechess-cli 1.5.1 present.
- PowerShell 7.6.6; Python tests run with `PYTHONDONTWRITEBYTECODE=1`.

## Commits

| Commit | Phase | Content |
| --- | --- | --- |
| `f126fcf` | 0-3 | Unified harness, determinism, parallel CTest, shards, labels, tooling. |
| `820f56d` | 4 | Four dedicated suites, single-source handshake fixture, runner retry. |
| `7b175c2` | 5 | Cutechess diagnostics/smoke split, host-agnostic pwsh, installer seams. |
| `72cf999` | 6 | CI split into three independent jobs plus the updated contract test. |

The phase 7 documentation change is the commit containing this record.

## Results

| Run | Tests | Wall time | Notes |
| --- | --- | --- | --- |
| Phase 1 (sequential) | 45/45 | 436.35 s | after the harness migration |
| Phase 3 (parallel `-j 8`) | 48/48 | 144.50 s | first sharded `koi_search_tests` |
| Phase 4 (parallel `-j 8`) | 52/52 | 173.73 s | four new suites registered |
| Phase 5/6 (parallel `-j 8`) | 53/53 | 151.25 s | final; results in `artifacts/verification/2026-09-17-test-suite-hardening/phase5-full` |
| Final (parallel `-j 8`) | 53/53 | 162.99 s | after the intermittent classification; results in `.../final-release2` |

Label totals for the final run (sec*proc): unit 541.49, search 537.18, heavy
719.95, process 361.94, matches 227.15, tools 105.69, python 16.39, uci 24.77,
packaging 3.60, evaluation 0.23, runtime 3.30.

`koi_search_tests` reports `koi-test-summary run=150 pass=134 fail=0 xfail=16
xpass=0 skip=0 unseen=0` when run as a single executable; under CTest it is
split into four shards that all pass.

One final parallel run exposed the XPASS policy working as intended:
`XPASS incomplete root forcing fallback` in `koi_search_tests_4of4` failed that
run even though the case is `XFAIL` in six consecutive isolated runs. Its
outcome therefore flips with host scheduling, so it was classified
`intermittent`: both outcomes are reported in the summary
(`... unseen=<n> intermittent=<n>`) but neither is fatal, keeping the suite
deterministic without hiding the gap (see `tests/README.md`, "Known-failing
behavior tests"). All other known-failure entries remain strict

Per-suite case evidence for the new suites: `transposition_table_tests`
run=7 pass=7, `search_service_tests` run=7 pass=7 (includes the in-place
ponderhit conversion), `classical_evaluator_tests` run=7 pass=7,
`evaluation_features_tests` run=6 pass=6.

Debug smoke (unit, heavy excluded, `KOI_TIMEOUT_SCALE` auto = 3):
**23/23 passed, total 9.49 s** (results in
`artifacts/verification/2026-09-17-test-suite-hardening/debug-smoke`),
including the four new suites and `uci_controller_tests` (9.20 s). This is the
same subset the CI `debug-smoke` job runs.

## Flake probes

Both historically failing cases were probed five times each with
`tools/test/flake_probe.ps1` (case filter across the four shards):
`short oracle b2b4 rook lift` 5/5 pass
(`artifacts/verification/2026-09-17-test-suite-hardening/flake-probe-b2b4`) and
`short timed threaded authoritative root` 5/5 pass
(`.../flake-probe-threaded-root`). The b2b4 case is now deterministic at
depth 2; the threaded case keeps its 200 ms movetime and timing-sensitive
declaration.

## What changed, by area

### Harness (`tests/support/koi_test_support.hpp`)

Single `run_tests` entry point with `--filter`/`--shard`/`--list`/`--quiet`,
`KOI_TEST_FILTER`/`KOI_TEST_SHARD`/`KOI_TEST_RETRIES`/`KOI_ALLOW_XPASS`,
`XFAIL`/`XPASS`/`SKIP`/`XFAIL-UNSEEN` result lines, a machine-readable
`koi-test-summary` line, and process-unique `TempDirectory`. `XPASS` is fatal,
so known-failure entries cannot silently go stale; the deliberately tiny
`intermittent` list is exempt in both directions (see above), and skips are
explicit instead of silent early returns.

### Determinism

- The short-oracle rook-lift case asserts its reviewed rejection with a
  deterministic depth-2 search (threads 1, hash 64) instead of a 100 ms clock.
- The threaded short-clock case uses 200 ms and remains declared
  timing-sensitive.
- 35 timing-sensitive cases are declared; `KOI_TEST_RETRIES` (or
  `run_tests.ps1 -RepeatUntilPass`) retries them.

### Parallel CTest

- Four-way shard split of `koi_search_tests`, 0-based shard indices passed as
  `--shard=<i>/4`.
- Unique scratch directories for `koi_bench_process_test.ps1` (GUID temp dir)
  and `hash_memory_stability_test.ps1` (GUID directory under `artifacts/`,
  required by the soak script's repository-artifact policy).
- Labels on every test plus `PROCESSORS 2` for heavy cases and
  `RUN_SERIAL TRUE` for `koi_engine_process` (it installs a temporary
  `book.bin` beside the binary).
- `KOI_TEST_TIMEOUT_SECONDS` (default 600) and `KOI_TIMEOUT_SCALE` (auto: 3x
  for Debug) so Debug does not inherit Release deadlines.

### Single-source handshake

`tests/data/uci/handshake.txt` holds the 28-line transcript with
`{max_threads}` and `{empty}` placeholders; both `uci_controller_tests` and
`uci_process_test.ps1` consume it, so the two contracts cannot drift.

### Optional-dependency visibility

`cutechess_stability_diagnostics` always runs (fabricated engines).
`cutechess_stability_smoke` is registered only when `cutechess-cli.exe` is
found, and CMake reports when it is omitted. `install_book_script` now covers
the successful install, the already-installed no-op, and `-Force` replacement
through the offline `-SourceFile`/`-ExpectedSha256` seams, while the pinned
release URL and hash remain the defaults.

### CI

Three independent jobs (`release-full`, `debug-smoke`, `shadow-diff`) with
`timeout-minutes`, python-chess installation, `ctest -j 4`, JUnit and
`LastTest.log` artifacts, and no fail-fast cancellation.

## Known limitations

- **No virtual clock seam.** The search runner reads
  `std::chrono::steady_clock` directly at many call sites, so a test-only clock
  would not control the assertions that motivated it without production
  changes. The two structural timing offenders were made deterministic instead,
  and the remaining timing cases rely on the timing-sensitive declaration plus
  retries. This is a deliberate, documented deviation from the original plan.
- **Debug heavy suites are not part of the CI smoke.** The Debug job excludes
  the `heavy` label because `koi_search_tests` exceeds the Debug budget; the
  full Debug suite must be run manually with the scaled timeouts.
- **The local 53-test count is configuration-dependent.** Without python-chess,
  shadow-diff, or cutechess-cli the count is lower; `tests/README.md` states
  the conditions.
- **Retries can mask host crashes only transiently.** `run_tests.ps1` adds
  `--repeat until-pass:2` after observing one `pwsh` 7.6.6 CLR crash
  (0x80131506 in the ConsoleHost jump-list thread). Real assertion failures
  fail every attempt, so the retry cannot turn a broken test green.
- **Timing-sensitive retries default to one attempt.** CI does not set
  `KOI_TEST_RETRIES`; increasing it is an operator decision.
