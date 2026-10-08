# Task 4 Report: Tactical Search Reliability Audit

## Scope

Task 4 was audited from `23132a5` against the current shared branch. The
requested tactical behavior was already present in the existing search code
and tests, so no speculative production change was retained:

- regular search generates `MoveMetadata` with `gives_check` once per node;
- captures, promotions, checks, TT moves, and killers are excluded from LMR;
- every reduced late move that scores above alpha is re-searched at full child
  depth, including fail-high results;
- null move pruning is gated by `position_features().game_phase >= 8` and
  current-side non-pawn material, with existing verification retained;
- qsearch checked evasions, checking continuations, SEE, delta pruning, TT
  rules, cancellation, quiet-history diagnostics, and stable single-thread
  tie-breaking remain unchanged.

The existing focused regressions already cover checking order, poisoned
captures, check evasions, mate distance, low-phase and pawn-only null safety,
eligible null verification, LMR activity, and reduced-move full-depth
verification.

## TDD evidence

A candidate additional sparse pawnless fixture was written first and run
without a production change. It passed immediately (`null_cutoffs == 0`), so it
did not distinguish a missing behavior and was removed rather than used to
justify a broader null-move rule. This prevented an unproven search change.

The existing Task 4 regressions were then run as focused GREEN checks:

```text
PASS shorter mate preference
PASS pawn-only zugzwang null safety
PASS low-phase null safety
PASS eligible null verification
PASS late quiet move reductions
PASS late move full-depth verification
PASS forced quiet evasion
PASS mate in one distance
PASS checked quiescence cap
PASS bounded quiescence checks
PASS tactical search statistics
```

`search_ordering_tests.exe` exited `0`, and the existing threaded targeted
checks passed for root-worker execution, `evasion_06` parity, and threaded
MultiPV final-depth parity.

## Tactical gate verification

The existing 64-position strength suite passed at the single-thread reference
path:

```text
PASS strength suite
```

The direct MinGW benchmark invocation completed all 64 rows at `Threads=1`
with `matches=64`. The `Threads=2` benchmark did not complete within the
bounded local run and was terminated; the `Threads=4` run was not started after
that hang. A targeted classical threaded parity check also exceeded its
20-second diagnostic bound, while the other targeted threaded checks passed.
This is recorded as an environment/build validation limitation, not treated
as evidence for a Task 4 search rewrite. The repository CMake configuration
could not be regenerated locally because the installed CMake is 3.27.1 and
the project requires 3.31 or newer.

## Changed files

- `.superpowers/sdd/2026-09-05-en-croissant-intelligence/task-4-report.md`

No opening-book, UCI, evaluator, tablebase, or untracked user/Stockfish paths
were changed by Task 4.
