# Koi Search Strength Pass Working Summary

## Current state

- Branch: `koi-engine-v1`; baseline commit: `a13fc27`.
- The source currently matches `HEAD` in search behavior; only explanatory
  qsearch comments differ in `src/koi/detail/search_context.hpp`.
- Release binaries were rebuilt through the Visual Studio x64 developer
  environment after an initial environment-only failure from a bare `cl.exe`.

## Architecture model

- `SearchService` owns iterative deepening, aspiration retries, root-worker
  scheduling, short-search fallbacks, and completed-iteration authority.
- `SearchContext` owns recursive negamax/qsearch, stack frames, TT access,
  node/time cancellation, PVs, and per-worker statistics.
- `SearchMovePicker` and `SearchOrderingTables` are private worker-local
  ordering state; `SearchPolicy` owns scalar LMR/pruning decisions.
- `GameState` remains the legality, make/unmake, repetition, key, and move
  metadata authority. The evaluator is unchanged and classical by default.

## Reproduced baseline evidence

- Release focused CTest after rebuilding: 6/8 passed; `koi_search_tests` fails
  the single-PV forcing fixture (`f6e4` instead of `f6g4`) and
  `koi_strength_tests` fails `check_03` (`f2d4` instead of an accepted check).
- The failures are deterministic at the current source. Ordering, policy,
  runtime, SEE, and perft focused tests pass.
- `check_03` at depth 2 ranks the quiet `f2d4` above the accepted checks; with
  `MultiPV=16`, `f2e3` is the top line, showing the issue is shallow root
  ranking/tie behavior rather than legality or missing check generation.

## Isolated qsearch experiments

Each experiment was rebuilt in Release and run against the forcing regression
plus the strength suite:

| Change retained while testing | Forcing fixture | `check_03` | Decision |
| --- | --- | --- | --- |
| Baseline qsearch | `f6e4` | `f2d4` | Baseline |
| qsearch continuation key `position_key()` | unchanged | unchanged | Reject |
| qsearch capture-history writes | unchanged | unchanged | Reject |
| eager qsearch SEE metadata | unchanged | unchanged | Reject |
| SEE floor `0` with new qsearch prune policy | passes `f6g4` | unchanged | Promising, retain |
| old qsearch frontier/pruning | fails | unchanged | Reject |

The promising qsearch policy still needs to be reapplied before final
verification; the current worktree is intentionally baseline while root causes
are investigated.

## Next hypothesis

`check_03` is not affected by qsearch-only changes. Its depth-two root search
needs a principled check/forcing extension or root candidate confirmation that
preserves normal PV/TT/aspiration contracts and is guarded by reproducible
search evidence rather than a fixture-specific move rule.
