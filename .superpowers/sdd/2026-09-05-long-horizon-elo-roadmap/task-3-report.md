# Task 3 implementation report

Date: 2026-09-05
Base: `bd01fcd` (`fix: close Task 2 evaluator review findings`)

## Scope delivered

- Extended the private `SearchMoveOrdering` module with depth-aware quiet
  history updates, explicit quiet-fail maluses, and fixed-size continuation
  history keyed by the prior move.
- Kept TT moves, SEE/MVV-LVA captures, promotions, checks, killers, and the
  deterministic earliest-move tie key in their existing priority tiers.
- Replaced the recursive ordering scratch vector with fixed-size storage
  bounded by `kMaximumLegalMoves`, avoiding recursive hot-path allocation.
- Made LMR reduction depend on depth, move number, and quiet-history score;
  reduced fail-high lines continue to receive full-depth verification.
- Added conservative depth-1 razoring and quiet futility pruning only outside
  check/tactical positions and only at the existing safe game-phase threshold.
- Added depth-6 null-move verification, while retaining the existing sparse
  endgame and non-pawn-material guards.
- Added counters for null verification, futility/razoring, quiet-history, and
  continuation-history events, including threaded-stat aggregation.
- Passed the root move into threaded workers as continuation context. No UCI,
  opening-book, evaluator, public search API, or worker-stdout behavior changed.

## TDD evidence

The new ordering and futility tests were added before the production changes.
The first correctly configured RED build was:

```text
Launch-VsDevShell.ps1 -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
cmake --build out\task2-debug-vs --config Debug --parallel --target search_ordering_tests koi_search_tests
```

It reached compilation and failed for the intended missing feature surface:

```text
error C2660: SearchMoveOrdering::record_quiet_cutoff: function does not take 5 arguments
error C2039: record_quiet_fail is not a member of SearchMoveOrdering
error C2661: SearchMoveOrdering::order: no overloaded function takes 5 arguments
error C2039: quiet_futility_prunes is not a member of SearchStats
error C2039: razoring_prunes is not a member of SearchStats
```

The initial attempt outside the Visual Studio developer shell failed earlier
on the environment (`fatal error C1083: Cannot open include file: 'algorithm'`);
no source was changed for that issue.

Focused GREEN verification after implementation:

```text
ctest --test-dir out\task2-debug-vs -C Debug -R "^(search_ordering_tests|koi_search_tests)$" --output-on-failure
100% tests passed, 0 tests failed out of 2

ctest --test-dir out\task2-release-vs -C Release -R "^(search_ordering_tests|koi_search_tests)$" --output-on-failure
100% tests passed, 0 tests failed out of 2
```

The focused search suite includes the inherited mate-distance, legal-PV,
deterministic fixed-depth, LMR verification, sparse-null, and global-node
limit regressions, plus the new futility safety/accounting tests. The ordering
suite includes TT/MVV-LVA/SEE priority, stable ties, killer priority, checks,
history malus, and continuation ordering.

## Required verification

Fresh Visual Studio x64 Debug and Release builds completed successfully.
Current-tree full CTest results:

```text
ctest --test-dir out\task2-debug-vs -C Debug --output-on-failure
100% tests passed, 0 tests failed out of 16

ctest --test-dir out\task2-release-vs -C Release --output-on-failure
100% tests passed, 0 tests failed out of 16
```

This includes the UCI controller, UCI process, UCI match process, benchmark
process, perft, replay, strength, and Python tests.

The fixed-depth tactical benchmark was run from the current Release binary:

```text
threads=1 rows=64 matches=64
threads=2 rows=64 matches=64
threads=4 rows=64 matches=64
threads=1 repeated output: byte-identical
```

The direct UCI process scripts also exited successfully. The UCI-match script
prints its expected rejected-illegal-opening diagnostic while validating the
fixture; the process exit code remains zero. No engine worker emitted protocol
text to stdout.

## Self-review

- Only six source/test files plus this report are changed; the generated
  `tests/__pycache__/tune_eval_test.cpython-314.pyc` remains untracked and was
  not altered or committed.
- `git diff --check` reported no whitespace errors.
- Opening-book sources and UCI controller sources are unchanged.
- No CPU-specific instruction flags were added.
- Recursive `SearchContext` ordering uses fixed arrays; vector use remains in
  root/result and compatibility overload paths outside recursive ordering.
- Threads=1 remains the serial reference path; stable tie-breaking, legal
  moves, mate distance, draw scoring, cancellation, global node accounting,
  and UCI output tests remain green.

## Concerns and follow-up

Continuation history uses a bounded power-of-two hash table to keep each
search context fixed-size and portable; collisions only affect ordering
quality, not correctness. Futility and razoring are deliberately limited to
depth 1 and the existing phase guard because the inherited low-phase LMR
verification fixture is a compatibility reference. Further pruning should be
introduced only with a new tactical-gate RED/GREEN cycle.

Final pre-commit status was limited to the Task 3 files listed above plus the
unrelated untracked Python bytecode artifact.

## Fix round 1 review findings

Review date: 2026-09-05

### Important finding 1: signed history-update overflow

The continuation update path can pass `delta == 8192` at depth 64. The
previous expression multiplied signed `int` values before clamping, so a
near-saturated score could invoke undefined behavior. `update_history` now
promotes score, delta, absolute delta, and the product to `std::int64_t`,
performs the bounded gravity update in the wider type, and clamps before the
result is converted back to `int`.

The new ordering regression performs 500 depth-64 quiet-cutoff updates keyed
by a prior move and asserts the resulting score remains in the documented
bounded range. It passed in both configurations:

```text
search_ordering_tests.exe: PASS history saturation overflow safety
ctest --test-dir out\task2-debug-vs -C Debug -R "^(search_ordering_tests|koi_search_tests)$" --output-on-failure
100% tests passed, 0 tests failed out of 2
ctest --test-dir out\task2-release-vs -C Release -R "^(search_ordering_tests|koi_search_tests)$" --output-on-failure
100% tests passed, 0 tests failed out of 2
```

### Important finding 2: direct null-verification regression

Added `eligible null verification` to `koi_search_tests`. It searches the
standard start position at fixed depth 7 with a 50,000-node cap, preserves a
legal root move, and asserts `result.stats.null_verifications > 0`. The first
compact-material candidate correctly produced a behavioral RED because it
reached the node cap without an eligible null fail-high; the fixture was
refined to startpos so the test exercises the intended path rather than
testing a non-triggering position. The corrected focused test output is:

```text
PASS eligible null verification
```

The existing sparse/pawn-only and low-phase tests remain in the same focused
run and continue to assert zero null cutoffs in unsafe endgames.

### Fix-round build and review evidence

```text
cmake --build out\task2-debug-vs --config Debug --parallel --target search_ordering_tests koi_search_tests
completed successfully
cmake --build out\task2-release-vs --config Release --parallel --target search_ordering_tests koi_search_tests
completed successfully
git diff --check
no whitespace errors
```

The untracked `tests/__pycache__/tune_eval_test.cpython-314.pyc` artifact was
not modified or staged. No other source, UCI, opening-book, or build-portability
behavior changed. The fix commit is the next commit after `fa97d4d`.
