# Task 2 implementation report

Date: 2026-09-05
Base: `755285f` (`docs: record Task 1 round 1 verification`)

## Scope delivered

- Moved the existing classical scalar coefficients into the Koi-owned constexpr
  `ClassicalEvaluationParameters` in `src/koi/evaluation_parameters.hpp`, with
  version `classical-eval-v2` and no runtime parameter dependency.
- Added `ClassicalEvaluator::parameters()` as a read-only accessor.
- Preserved the existing tapered piece-square tables and all inherited scalar
  values; added conservative coefficients for endgame king activity, passed
  pawn king support/proximity, promotion-race value, and tempo.
- Added `king_activity`, `passed_pawn`, and `tempo` to `EvaluationBreakdown`,
  including perspective negation and complete non-dead-material accounting.
- Added `tools/tune_eval.py`, standard-library-only at runtime (with optional
  `python-chess` validation), accepting CSV, TSV, pipe, whitespace, or stdin
  corpus input. It validates rows and emits deterministic C++ metadata with the
  parameter version, SHA-256 corpus hash, and selected coefficients.
- Kept normal UCI output, Task 1 options/timing/diagnostics, book behavior, and
  deterministic Threads=1 search unchanged.

## TDD evidence

### RED

Added evaluator tests before the production implementation and ran:

```text
Launch-VsDevShell.ps1 -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
cmake --build out\task2-tdd --config Debug --parallel
```

The build reached `tests/koi_search_tests.cpp` and failed for the expected
missing feature surface:

```text
error C2039: 'parameters': is not a member of 'koi::ClassicalEvaluator'
error C2039: 'king_activity': is not a member of 'koi::EvaluationBreakdown'
error C2039: 'passed_pawn': is not a member of 'koi::EvaluationBreakdown'
error C2039: 'tempo': is not a member of 'koi::EvaluationBreakdown'
ninja: build stopped: subcommand failed.
```

No production implementation was present when this RED command was run.

### GREEN and focused verification

After the minimal implementation, the focused evaluator/search target passed:

```text
ctest --test-dir out\task2-tdd -C Debug -R '^koi_search_tests$' --output-on-failure
1/1 Test #5: koi_search_tests ... Passed
100% tests passed, 0 tests failed
```

The focused core/search/strength run passed for core and search. The first
strength run exposed that the existing single-service fixture loop reused hash
entries across independent corpus rows; the benchmark itself uses cold
per-position services. The fixture test now clears its hash between rows, and
the complete strength corpus passes without changing engine behavior.

The tuning tool checks also passed:

```text
python -m py_compile tools\tune_eval.py
tune_eval_repeat_identical=True
invalid result: maybe  (exit code 2)
```

## Build and CTest verification

Both builds used the Visual Studio x64 developer shell and CMake 4.4.2 at
`C:\msys64\ucrt64\bin\cmake.exe`; MSVC was 19.44.35228.0 targeting x64.

```text
cmake -S . -B out\task2-debug-vs -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=cl
cmake --build out\task2-debug-vs --config Debug --parallel
[90/90] ... completed successfully

cmake -S . -B out\task2-release-vs -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=cl
cmake --build out\task2-release-vs --config Release --parallel
[90/90] ... completed successfully
```

Full CTest results:

```text
ctest --test-dir out\task2-debug-vs -C Debug --output-on-failure
100% tests passed, 0 tests failed out of 15

ctest --test-dir out\task2-release-vs -C Release --output-on-failure
100% tests passed, 0 tests failed out of 15
```

This includes UCI/process, compatibility, book, search, strength, benchmark,
replay, Windows CI configuration, and Elo-oracle tests.

## Deterministic benchmark comparison

The Task 1 Release benchmark at `out\task1-final-release2\koi-bench.exe` was
the before baseline. The new benchmark was run twice at `Threads=1` and
default speed:

```text
baseline_rows=64 new_rows=64
baseline_median_nodes=119.5 new_median_nodes=119.5 delta_percent=0
new_repeat_byte_identical=True
```

No new row had `match 0`; the inherited 64-position tactical gate remained
fully accepted. The textual rows differ in some transposition-hit/qnode
details, but there is no fixed-depth median node regression.

## Changed files

- `src/koi/evaluation_parameters.hpp`
- `src/koi/classical_evaluator.hpp`
- `src/koi/classical_evaluator.cpp`
- `tests/koi_search_tests.cpp`
- `tests/koi_strength_tests.cpp`
- `tools/tune_eval.py`
- `.superpowers/sdd/2026-09-05-long-horizon-elo-roadmap/task-2-report.md`

## Self-review and concerns

- `rg 'chess\\.hpp' src\\koi\\*.hpp` found no public-header exposure.
- All new terms are integer centipawns and are included exactly once in the
  total and perspective flip.
- To preserve the reviewed tactical-only gate and exact tactical score, the
  new tempo and king-activity contributions activate in pawn-containing
  endgames; their dedicated tests cover the intended long-horizon domain.
  This is deliberate, but future tuning should decide whether a broader
  non-pawn endgame policy is warranted with fresh gate evidence.
- `tests/koi_strength_tests.cpp` clears the shared fixture hash between
  independent rows so the fixture validates each position rather than
  cross-row cached search state. Runtime SearchService hash behavior and all
  Task 1 UCI interfaces remain unchanged.
- No NNUE, runtime parameter file, arbitrary inherited coefficient changes, or
  external Python dependency was added.

Final pre-commit check: `git diff --check` reported no whitespace errors.

## Fix round 1 review findings

Review findings addressed on 2026-09-05:

1. Removed the pawn-presence dependency from endgame king activity and tempo.
   Both terms now use the existing phase boundary (`game_phase <= 2`), so
   pawnless K+R versus K endgames receive the same tapered king activity and
   exactly-one-side-to-move tempo treatment as pawn endgames. This preserves
   the reviewed phase-4 tactical score while covering the intended minor-piece
   endgame phase. Perspective symmetry and insufficient-material behavior are
   unchanged.
2. Reworked `tools/tune_eval.py` to parse the canonical
   `src/koi/evaluation_parameters.hpp` at generation time. It now emits the
   canonical version and every integer parameter in header order, so changing
   the C++ layout/value set cannot silently leave tuning metadata stale.
3. Recorded the minor findings: removed the unused `io` import; the fallback
   validator remains intentionally partial when `python-chess` is unavailable.

### Fix-round TDD RED

Added the pawnless rook regression and `tests/tune_eval_test.py`, then ran:

```text
ctest --test-dir out\task2-debug-vs -C Debug -R "^(koi_search_tests|tune_eval_python)$" --output-on-failure
```

Before the fixes, both tests failed for their target reasons:

```text
FAIL evaluator pawnless endgame terms: pawnless rook endgames must receive tapered king activity credit
AssertionError: 'kTunedEvaluation_pawn_value = 100;' not found
0% tests passed, 2 tests failed out of 2
```

### Fix-round GREEN and verification

After the fixes, the same focused command passed:

```text
2/2 tests passed, 0 tests failed
```

Fresh x64 Release rebuild and complete suites were run with the Visual Studio
x64 developer shell, MSVC 19.44.35228.0, and CMake 4.4.2:

```text
cmake --build out\task2-release-vs --config Release --parallel
completed successfully
ctest --test-dir out\task2-debug-vs -C Debug --output-on-failure
100% tests passed, 0 tests failed out of 16
ctest --test-dir out\task2-release-vs -C Release --output-on-failure
100% tests passed, 0 tests failed out of 16
```

The deterministic post-fix benchmark was run twice at Threads=1:

```text
rows=64 repeat_byte_identical=True row_diff_count=0 gate_mismatches=0
```

The final self-review found no `has_any_pawn` gate, no `import io`, and the
tool's `canonical_parameters()` is the sole source for emitted parameter
names/values. `git diff --check` reported no whitespace errors.
