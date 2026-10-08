# Task 5: deterministic root-parallel search report

Base commit: `2537cc5` (`test: expand Task 4 search safety coverage`)

## Scope and ruling

This pass changes only `src/koi/search_service.cpp` and
`tests/koi_search_tests.cpp`. It does not change UCI controller or opening-book
APIs. `Threads=1` remains the reference search path. The existing worker pool
continues to be reused across iterative-deepening depths; no pool is created
when the root has fewer than two legal moves. Single-PV node-limited searches
use the reference path so the global node boundary remains exact.

Ruling: the legal-but-unlisted `e4d5` result for `evasion_06` at Threads 2/4
was treated as a hard regression. The allowlist was not weakened.

## Red phase and pre-change baseline

The pre-change Debug focused suites passed, but the 64-position timed
benchmark reproduced the carried Task 4 defect:

| Threads | `evasion_06` result | Allowlist result |
| ---: | --- | --- |
| 1 | `e4f4` | accepted |
| 2 | `e4d5` | legal but unlisted |
| 4 | `e4d5` | legal but unlisted |

The new red regression was added first in `koi_search_tests`:
`threaded evasion_06 parity`. It runs the exact hard-suite FEN at depth 2,
requires the serial move to be one of `e4f3`, `e4d4`, `e4f4`, and requires
Threads 2 and 4 to match the serial completed depth, move, and score.

Red command and failure:

```powershell
out/task5-msvc-debug/koi_search_tests.exe
```

```text
FAIL threaded evasion_06 parity: evasion_06 must retain the serial fixed-depth
move and score at Threads=2 and Threads=4
```

The Task 4 recorded timed profile was also retained as the pre-change reference:
Threads 1/2/4 aggregate NPS were 41,508 / 68,658 / 97,589 respectively. Its
Threads 2/4 profiles each had the one `evasion_06` allowlist miss.

## Design and implementation

The existing `RootWorkerPool` preserves the requested scheduling model: it
searches the stable first root before releasing the remaining root indices via
an atomic work cursor, stores each root result by original index, and joins all
workers at the end of every submitted iteration. Only the search-handle worker
emits `on_info` or `on_complete`.

For single-PV threaded search, root workers are explicitly speculative:

- Their `SearchContext` does not probe or write the shared transposition table.
  This stops concurrently searched null-window roots from changing the state
  used to select the published line.
- After every fully joined, non-aborted speculative iteration, the handle
  thread confirms the same depth with its own serial `SearchContext`, the same
  stable root ordering, shared TT, aspiration window, time origin, and stop
  flag as `Threads=1`.
- Only that confirmed move, score, PV, and completed depth are published.
  Partial/aborted iterations never replace the previous completed result.
- Worker statistics and confirmed serial statistics are aggregated for info and
  final accounting. Node-limited single-PV searches take the existing serial
  path, retaining one exact global node counter and boundary.

MultiPV preserves the prior shared-TT worker behavior so warmed-hash MultiPV
and stable ordered tie behavior remain intact. The worker pool is still one
pool per search-handle and is reused for each depth/research; its destructor
joins every worker before the handle worker reports completion.

## Verification

MSVC 19.44 / CMake 3.31.6 / Ninja were used because the PATH CMake (3.27.1)
cannot configure this CMake 3.31 project.

```powershell
# Debug/Release configure and relevant targets
call VsDevCmd.bat -arch=x64 -host_arch=x64
cmake -S . -B out/task5-msvc-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build out/task5-msvc-debug --target koi_search_tests uci_controller_tests koi_strength_tests koi_bench koi_engine -j 8
cmake -S . -B out/task5-msvc-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build out/task5-msvc-release --target koi_search_tests uci_controller_tests koi_strength_tests koi_bench koi_engine -j 8

# Registered focused and process suites
ctest --test-dir out/task5-msvc-debug --output-on-failure -R '^(koi_search_tests|uci_controller_tests|koi_strength_tests|koi_engine_process|koi_benchmark_process)$'
ctest --test-dir out/task5-msvc-release --output-on-failure -R '^(koi_search_tests|uci_controller_tests|koi_strength_tests|koi_engine_process|koi_benchmark_process)$'
```

Results:

- Debug direct `koi_search_tests`, `uci_controller_tests`, and
  `koi_strength_tests`: all pass.
- Release direct `koi_search_tests`, `uci_controller_tests`, and
  `koi_strength_tests`: all pass.
- Debug registered CTest selection: 5/5 pass. `koi_engine_process` passed in
  5.38 s and `koi_benchmark_process` passed in 48.19 s.
- Release registered CTest selection: 5/5 pass in 26.52 s.
- Final rebuilt Release CTest suite: 14/14 pass, including replay, UCI process,
  UCI match process, and Windows CI configuration tests.
- `git diff --check`: clean.

### UCI process regression correction

The final full Release run exposed a timing-sensitive test assertion in
`tests/uci_process_test.ps1`, not a UCI implementation defect. Its exact
failure was:

```text
Expected a legal initial bestmove, received: bestmove b1c3 ponder b8c6
```

`UciController::write_search_completion` already supports emitting an optional
ponder move, and this same process test accepts `bestmove ... ponder ...` in
its other completion checks. The root-parallel confirmation made that valid
form observable at this point consistently. The narrow test-only correction
now accepts an optional syntactically legal ponder suffix while still checking
the first captured move against the start-position legal-move list. No UCI or
book API/behavior changed. It passes in both Release full CTest and the Debug
`koi_engine_process` run.

Existing focused coverage exercised the additional Task 5 safety contracts:

- `threaded root search`, `classical threaded parity`, and the new
  `threaded evasion_06 parity` cover fixed-depth parity.
- `stable root ties` and `threaded multipv ordered root ties` cover stable
  equal-score ordering.
- `threaded global nodes` and `threaded node parity` cover global node limits.
- `threaded cancellation`, infinite/ponder lifecycle tests, and controller
  generation tests cover prompt cancellation and exactly-one completion.
- `threaded multipv warmed hash` and repeated iterative depths cover worker
  reuse and warmed-state behavior.
- Controller/process tests confirm that only controller-facing callbacks emit
  Lucas-compatible UCI info/completion output.

## 64-position hard gate and benchmarks

Commands used for both configurations:

```powershell
koi-bench.exe --threads 1 --speed 100 --timed --profile-json task5-profile-t1.json
koi-bench.exe --threads 2 --speed 100 --timed --profile-json task5-profile-t2.json
koi-bench.exe --threads 4 --speed 100 --timed --profile-json task5-profile-t4.json
```

All Debug and Release profiles were 64/64. In every profile,
`evasion_06` selected accepted `e4f4`; no legal/tactical allowlist was changed.

Release timed aggregate observations:

| Threads | Nodes plus qnodes | Wall ms | NPS | Relative to T1 |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 47,486 | 77 | 616,701 | 100.0% |
| 2 | 181,457 | 304 | 596,898 | 96.8% |
| 4 | 185,578 | 245 | 757,461 | 122.8% |

The required 80% timed NPS floor is met at Threads 2 and 4. Debug measurements
were 31,784 / 37,820 / 50,062 NPS at Threads 1/2/4 respectively (119.0% and
157.5% of T1 for threaded runs).

## Concerns

1. The confirmation search prioritizes deterministic tactical correctness but
   increases total work and wall time: on this short fixed-depth suite, Release
   Threads=2 took 304 ms versus Threads=1 at 77 ms despite meeting the required
   NPS floor. The profile's NPS denominator includes speculative and confirmed
   nodes, so it should not be interpreted as a latency speedup.
2. The isolated-TT confirmation rule currently applies only to single-PV.
   MultiPV deliberately retains the prior shared-TT behavior to preserve its
   warmed-hash contract; it should receive a separate deterministic parity
   investigation if a future tactical discrepancy is observed.
3. Timed values are machine and background-load sensitive. Raw profile JSON
   files are in the ignored task build directories for local reproduction.

## Fix round 1: Threads=2 speculative-to-serial cancellation coverage

The reviewer-identified gap was covered in `tests/koi_search_tests.cpp` with
`threaded timed cancellation`. The test starts an unbounded-depth, 5-second
`movetime` search at `Threads=2`, waits for the first depth-1 info callback
(which is emitted only after the threaded speculative iteration and serial
confirmation), then stops promptly. It verifies that the handle is joined,
stop returns within two seconds, info depths are not duplicated, exactly one
completion is reported, and the final fallback move is legal. No production,
UCI controller, or opening-book code changed; the prior ponder-suffix test
correction remains unchanged.

Red command/result:

```powershell
cmd /d /c "call ""C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat"" -arch=x64 -host_arch=x64 && set "KOI_TEST_FILTER=threaded timed cancellation" && out\task5-msvc-debug\koi_search_tests.exe"
```

The new test reached the intended path and failed on the deliberate TDD
sentinel:

```text
FAIL threaded timed cancellation: RED: replace with handoff cancellation assertions
```

Green verification:

```powershell
# Debug focused regression, repeated ten times
cmd /d /c "call ""C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat"" -arch=x64 -host_arch=x64 && set "KOI_TEST_FILTER=threaded timed cancellation" && for /L %N in (1,1,10) do @out\task5-msvc-debug\koi_search_tests.exe || exit /b 1

# Complete focused search suite in both configurations
out\task5-msvc-debug\koi_search_tests.exe
out\task5-msvc-release\koi_search_tests.exe
```

Results: the focused regression passed 10/10 Debug runs. The complete Debug
and Release search suites each passed all 49 registered tests. The test uses
the existing short evaluator delay and a two-second cancellation bound, so
its wall-clock result remains subject to machine scheduling; repeated runs
showed no hangs, duplicate callbacks, or flaky failures.
