# Task 4 report: practical Elo search and evaluator tuning

## Status

`DONE_WITH_CONCERNS`. Committed as
`7ba3fa5898172d92589ebec63177ab442e73625a` with the required message
`strength: tune tactical search and evaluation`. The implementation keeps the
Koi-owned public boundary and does not alter UCI controller or opening-book
sources. Direct `SearchService` tactical tests are book-independent, so the
hard gate ran without book participation.

## Changed files

- `src/koi/classical_evaluator.cpp`
  - Added the private `EvaluationParameters` block for scalar material,
    phase, pawn-structure, activity, and king-safety coefficients.
  - Penalized a passed pawn whose immediate forward square is occupied by an
    opposing blocker. `evaluate()` and `breakdown()` signatures are unchanged.
- `src/koi/search_ordering.cpp`
  - Added a bounded SEE component to capture priority. It only reorders legal
    captures within the existing capture tier; TT, promotion, checking,
    killer/history, and stable tie-break tiers remain intact.
- `tests/koi_search_tests.cpp`
  - Added a file-backed evaluator regression for directly blockaded passers.
- `tests/search_ordering_tests.cpp`
  - Added a regression requiring a free capture to precede a poisoned queen
    capture.
- `tests/data/evaluation-positions.txt`
  - Added independent FEN fixtures for the passed-pawn comparison.

No changes were made to `src/koi/uci_controller.*`, `src/koi/opening_book.*`,
`src/koi/classical_evaluator.hpp`, `src/koi/static_exchange.cpp`, or
transposition-table code. Existing SEE implementation and tests already cover
legality-sensitive pinned/king/promotion/en-passant cases, so this pass reuses
that source of truth rather than duplicating it.

## Fix round 1: regression-safety expansion

This fix round changes tests and evaluator fixtures only. It intentionally
retains the conservative Task 4 production changes from `7ba3fa5`; investigation
found no new production fault.

- `tests/koi_search_tests.cpp`
  - Added a forced checked position where every legal response is a quiet
    interposition, proving search keeps legal checks/evasions and quiet
    defenses.
  - Added a mate-in-one distance assertion, a pawn-only zugzwang/null-move
    safety assertion, and a late-quiet-move LMR statistics assertion.
  - Added color-and-rank mirrored evaluator assertions for material, PST,
    mobility, pawn structure, activity, king safety, and total; the fixture
    has non-zero mobility, pawn, activity, and king-safety terms.
  - Added Black passed-pawn blockade and color-symmetric endgame passer-scaling
    assertions.
- `tests/data/evaluation-positions.txt`
  - Added the mirrored evaluator, Black blockade, and both-color endgame
    passer fixtures used by the new position-based tests.

The existing Task 4 production behavior already passed these regressions, so
there is no invented production red-green cycle. The initial endgame mirror
fixture was caught as incorrectly constructed by its own symmetry assertion;
it was corrected to the actual rank-and-color mirror before recording the
passing evidence below.

Focused fix-round commands and results, from the Visual Studio x64 developer
environment:

```text
cmake.exe --build out\elo-debug --target koi_search_tests -j 4
set "KOI_TEST_FILTER=evaluator mirrored terms"
out\elo-debug\koi_search_tests.exe
# PASS evaluator mirrored terms and black blockade

set "KOI_TEST_FILTER=evaluator color symmetric endgame passer"
out\elo-debug\koi_search_tests.exe
# PASS evaluator color symmetric endgame passer

set "KOI_TEST_FILTER=forced quiet evasion"
out\elo-debug\koi_search_tests.exe
# PASS forced quiet evasion

set "KOI_TEST_FILTER=mate in one distance"
out\elo-debug\koi_search_tests.exe
# PASS mate in one distance

set "KOI_TEST_FILTER=pawn-only zugzwang null safety"
out\elo-debug\koi_search_tests.exe
# PASS pawn-only zugzwang null safety

set "KOI_TEST_FILTER=late quiet move reductions"
out\elo-debug\koi_search_tests.exe
# PASS late quiet move reductions

cmake.exe --build out\elo-debug -j 4
ctest.exe --test-dir out\elo-debug --output-on-failure
# 14/14 passed; 0 failed

git diff --check
# no whitespace errors
```

## TDD evidence

The production changes followed two red-green cycles.

1. Evaluator blockade test
   - Initial `>` assertion was rejected because the existing phase calculation
     incidentally created a two-centipawn difference; it was strengthened to
     require at least ten centipawns of structure separation.
   - Red command: `cmake --build out/elo-debug --target koi_search_tests -j 4`
     followed by `out/elo-debug/koi_search_tests.exe` (from the MSVC x64
     developer environment).
   - Red result: `FAIL evaluator passed pawn blockade: a directly blockaded
     passed pawn must lose meaningful structure credit against an unobstructed
     passer`.
   - Green result after the private parameter/blockade change: all 42
     `koi_search_tests` checks passed.

2. SEE capture-ordering test
   - Red command: `cmake --build out/elo-debug --target
     search_ordering_tests -j 4` followed by
     `out/elo-debug/search_ordering_tests.exe`.
   - Red result: `FAIL SEE capture ordering: a free capture must be ordered
     ahead of a materially losing capture`.
   - Green result after bounded SEE priority: ordering tests passed, followed
     by passing static-exchange, search, and strength suites.

The Visual Studio x64 developer environment and its bundled CMake 3.31 were
used because the PATH CMake is 3.27.1 (below the project's required version).

## Tactical gate

The 64-position hard gate is direct-search/book-independent and therefore has
no opening-book contribution.

| Run | Threads | Result |
| --- | ---: | --- |
| Before implementation | 1 | 64/64 accepted moves, `PASS strength suite` |
| After evaluator group | 1 | 64/64 accepted moves, `PASS strength suite` |
| After ordering group / final Debug | 1 | 64/64 accepted moves, `PASS strength suite` |
| Final Release CTest | 1 default | `koi_strength_tests` passed |

Check evasions, mate scoring/distance, qsearch checked evasions, SEE pinned
and king-recapture legality, null move restoration, LMR statistics,
TT mate normalization, deterministic Threads=1 ordering, and killer/history
ordering are covered by the passing existing focused suites.

## Verification commands and results

The build commands were invoked through:

```text
cmd.exe /c call VsDevCmd.bat -arch=x64 -host_arch=x64 && cmake.exe --build <directory> -j 4
```

Fresh final verification:

```text
cmake --build out/elo-debug -j 4
ctest --test-dir out/elo-debug --output-on-failure
# 14/14 passed; 0 failed

cmake --build out/elo-release -j 4
ctest --test-dir out/elo-release --output-on-failure
# 14/14 passed; 0 failed; 33.65 s

git diff --check
# no whitespace errors
```

Release CTest includes the UCI process, benchmark process, replay, and UCI
match process suites; these were verified but not modified by this task.

## Timed benchmark observations

All values below are Debug `koi-bench --timed` observations over the 64
position hard suite at Speed=100 and cold hash. They are diagnostic data, not
machine-dependent CI thresholds. Per-position raw profiles are saved alongside
this report as `task-4-bench-*.json`.

| Profile | Threads | Nodes | QNodes | TT hits | SEE prunes | Delta prunes | Null cutoffs | LMR reductions | Elapsed ms | Aggregate NPS |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Before | 1 | 11,205 | 36,274 | 445 | 205 | 2,354 | 0 | 0 | 1,769 | 26,839 |
| After | 1 | 11,209 | 36,277 | 445 | 203 | 2,355 | 0 | 0 | 1,144 | 41,508 |
| After | 2 | 20,370 | 107,128 | 394 | 1,182 | 12,436 | 0 | 0 | 1,857 | 68,658 |
| After | 4 | 20,615 | 108,789 | 390 | 1,214 | 12,301 | 0 | 0 | 1,326 | 97,589 |

The Threads=1 run remained 64/64. Threads=2 and Threads=4 each produced one
non-reference allowlist miss (`evasion_06`), while still returning legal moves.
This does not affect the deterministic Threads=1 reference behavior, but it
is a regression-risk observation for multi-thread tactical consistency.

## External-opponent evidence

No Stockfish executable, supplied opponent executable, or `stockfish` PATH
command was available in the repository/workspace/PATH inspection. Jack is
unavailable and was neither added nor claimed as a match opponent. Therefore
the required paired 1+0 and 5+3 `OwnBook=false` opponent matrix was not run;
this is an external-evidence limitation, not a local implementation blocker.

## Design decisions and deferred work

- The evaluator change is intentionally narrow: it corrects a tactical/endgame
  blind spot while preserving the evaluated API and all prior scalar behavior
  except the explicit blockade penalty.
- The SEE term is clamped to queen magnitude before applying its small capture
  priority weight. It improves move ordering only; no capture is pruned or
  made illegal.
- PVS/aspiration widths, qsearch bounds, delta thresholds, null-move and LMR
  conditions, TT replacement, killer/history formulas, and raw PST/material
  values were intentionally not retuned. Existing safeguards and the lack of
  paired external match evidence make speculative changes riskier than the
  two targeted improvements retained here.

## Concerns

1. External Elo gain is not established without the unavailable paired
   opponent matrix.
2. Threads=2/4 have the `evasion_06` hard-suite allowlist discrepancy noted
   above. Threads=1 remains deterministic and passes the full gate.
3. Benchmark wall time/NPS varies by machine and background load; only the
   recorded node/pruning counters should be compared structurally.
