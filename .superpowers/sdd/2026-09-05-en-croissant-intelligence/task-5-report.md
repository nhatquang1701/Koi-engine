# Task 5 Report - Authoritative Threaded Root Search

Date: 2026-09-05
Base: `df83132`
Workspace: `C:\\Users\\ntATh\\AI test\\Koi engine`

## Scope

This recovery implementation completes the safe part of Task 5: threaded
single-PV root jobs are authoritative and no longer followed by a redundant
serial confirmation search. The existing stable root ordering, tie-breaking,
shared cancellation, global node accounting, and controller-owned reporting
paths were retained.

The attempted change is intentionally small. `src/koi/transposition_table.*`,
`src/koi/game_state.*`, and `CMakeLists.txt` were not changed because the
current branch already provides the required shared-table and benchmark
interfaces, and a new synchronization or move-conversion rewrite could not be
validated safely during this recovery. Existing concurrent TT, node-limit,
cancellation, and benchmark tests remain the coverage for those interfaces.

## Implementation

- `RootWorkerPool::run` now receives a root check-extension flag and each root
  job searches with a full `[-infinity, +infinity]` window.
- The single-PV path no longer creates a serial `reference_context`, runs a
  serial root search, or repeats a parallel aspiration re-search.
- Root check extensions are computed once per completed iteration and passed to
  every authoritative root job, matching the prior root-level extension
  boundary without using a stale per-child position check.
- Parallel workers continue to aggregate statistics through the controller;
  workers never emit UCI output.
- Added a counting-evaluator regression proving a depth-one threaded search
  evaluates each legal root exactly once and returns a legal move.
- Updated threaded timed-cancellation coverage to require the threaded root
  callback rather than the removed serial confirmation callback.

## Verification

Fresh verification was run after the system-restart recovery with CMake 4.4.2,
MSVC 19.44.35228.0, Visual Studio 17 2022, x64, and the current working tree:

```powershell
& 'C:\\msys64\\ucrt64\\bin\\cmake.exe' -S . -B out\\resume-task5-vs -G 'Visual Studio 17 2022' -A x64
& 'C:\\msys64\\ucrt64\\bin\\cmake.exe' --build out\\resume-task5-vs --config Debug --target koi_search_tests --parallel 4
ctest --test-dir out\\resume-task5-vs -C Debug -R '^koi_search_tests$' --output-on-failure
```

Result: configure/build succeeded; `koi_search_tests` passed, `1/1`, in
`22.68 s`.

```powershell
& 'C:\\msys64\\ucrt64\\bin\\cmake.exe' --build out\\resume-task5-vs --config Debug --target koi_engine koi_bench uci_controller_tests --parallel 4
ctest --test-dir out\\resume-task5-vs -C Debug -R '^uci_controller_tests$|^koi_engine_process$|^koi_engine_en_croissant_process$|^koi_benchmark_process$' --output-on-failure
```

Result: `4/4` tests passed in `53.79 s`:

- `uci_controller_tests`
- `koi_engine_process`
- `koi_engine_en_croissant_process`
- `koi_benchmark_process`

The benchmark process test exercised deterministic benchmark output, the
configured threaded profile, cold/warm profiles, timed mode, and the optional
128-position profile. The search regression suite exercised threaded root
execution, fixed-depth parity, stable ties, global node-limit accounting,
infinite/timed cancellation, callback uniqueness, and concurrent evaluator
safety.

No Stockfish oracle or Elo claim was made. The local CTest suite and Release
build remain part of the final Task 8 validation gate.

## Limitations and concerns

- This recovery does not add a new striped TT implementation or a new direct
  move-conversion layer; both are deferred until a measured, independently
  reviewable change can be validated without altering the `Threads=1`
  reference.
- No standalone historical RED transcript survived the interrupted worker
  sessions. The changed tests and focused GREEN results above are current and
  reproducible.
- A full Release build and full CTest run are intentionally deferred to Task 8
  after the Task 5 commit is reviewed.

## Review fix round

The scoped review identified that the first standalone gate could accept a
faster candidate that stopped before the requested depth, that fixed execution
order could bias timings, and that the new root-in-check test did not prove
effective worker concurrency. Commit `ea584fa` addresses those findings:

- `tools/task5_perf_gate.ps1` now validates the 64 timed stdout rows against
  the JSON profile, checks completed depth against each fixture's requested
  depth, requires accepted rows and baseline/candidate move-score parity, and
  alternates baseline-first and candidate-first order across runs.
- The output directory is canonicalized through the Windows final-path API
  before the repository containment check, including an existing junction.
- The root-in-check regression asserts concurrent evaluation when the host has
  more than one effective worker and otherwise accepts the safe serial
  fallback.
- The README's architecture and release-gate wording now describes En
  Croissant as primary and the authoritative threaded root path.

RED evidence (external temporary output, not committed): the fake benchmark
reported `Run 1 benchmark parity depth mismatch for position fixture-01` and
exited `1`. GREEN evidence from the real Release benchmark executable:

```text
runs=3 threads=2 speed=100 suite=strength limits=fixed-depth hash=cold
run=1 baseline_total_ms=288 candidate_total_ms=273 order=baseline,candidate
run=2 baseline_total_ms=280 candidate_total_ms=285 order=candidate,baseline
run=3 baseline_total_ms=274 candidate_total_ms=283 order=baseline,candidate
baseline_median_ms=288.0
candidate_median_ms=285.0
candidate_vs_baseline=-1.04% regression_limit=5.00%
result=PASS
```

After the fix, the rebuilt `koi_search_tests` passed `1/1`; PowerShell
parsing passed; and the focused performance gate passed. Full Debug/Release
validation remains the Task 8 gate.
