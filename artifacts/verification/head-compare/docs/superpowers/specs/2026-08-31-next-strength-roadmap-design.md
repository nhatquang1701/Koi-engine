# Koi Engine Next Strength and Compatibility Design

## Goal

Improve Koi's playing strength and response time while preserving its Windows
x64 C++26 build, standard UCI behavior, Lucas Chess play/analysis/tutor
workflows, deterministic `Threads=1` searches, and the existing `Hash`,
`Threads`, `Speed`, `MultiPV`, and `Ponder` controls.

## Approved behavior

- A bare `go` with no depth, node, clock, movetime, or `infinite` limit uses an
  internal 250 ms move-time budget. `Speed` scales time-based budgets but never
  lets Koi exceed an explicit limit. Explicit depth, node, movetime, clock,
  infinite, and ponder commands retain their authority.
- `infinite nodes N` honors the node limit. `wtime` and `btime` are accepted
  independently; the side-to-move clock is used for allocation when available.
- `Ponder=true` allows `bestmove <move> ponder <reply>` when the second PV move
  is legal. `ponderhit` applies the expected reply to the saved root and may
  safely restart ordinary search; speculative tree reuse is not required.
- `UCI_AnalyseMode` remains a standard mode hint. Lucas analysis uses
  `go infinite` or explicit limits. `MultiPV` lines are legal, distinct, and
  rank-stable; `searchmoves` restricts only root moves.
- Search has one controller-owned output path, one completion callback, and no
  stale output after a position, option, or quit command replaces a search.

## Measurement contract

`koi-bench` keeps its current deterministic text output. It adds
`--profile-json <path>` and `--warm-hash`; timed fields are emitted only with
`--timed`. Profile records include the build/engine identity, suite and
position IDs, FEN, limits, options, score/PV, nodes, qnodes, TT hits, pruning
counters, elapsed time when requested, and NPS. Cold and warm TT runs are
identified explicitly.

The hard tactical gate contains 64 curated positions covering mates, checks,
evasions, forks, pins, poisoned captures, promotions, defensive moves, and
pawn races. A 128-position strength corpus adds positional, king-safety, and
endgame cases. Legality and existing tactical solves are hard gates; broader
strength, Elo, and NPS results are reports rather than machine-dependent CI
thresholds.

## Strength and performance sequence

1. Establish the timing, profiling, replay, and UCI compatibility contracts.
2. Optimize measured single-thread hot paths: SEE, qsearch move generation,
   feature extraction, evaluation caching, and TT contention.
3. Tune tactical search and the classical evaluator one gated change at a time.
4. Improve deterministic root parallelism only after the single-thread path is
   complete and verified. `Threads=1` remains the reference behavior.
5. Keep NNUE, books, tablebases, and variants deferred behind the existing
   `Evaluator` and rules boundaries.

## Match report

The optional harness writes `koi-uci-match-v2` JSON plus PGN, preserving v1
readability. Each ply contains the actual root FEN, exact UCI commands, engine
identity, returned move, replay legality, timing, final info/PV, all info
lines, and raw `bestmove`. Each game contains result, winner, termination, and
process status. Koi never labels an opponent's reported score as an external
reference evaluation.

## Acceptance

- Existing tests and all new tactical, timing, UCI, replay, and concurrency
  tests pass in Debug and Release.
- Fixed-depth `Threads=1` output is stable and `Threads=2` agrees on best move
  and score.
- All PV and returned moves are legal, cancellation is prompt, and exactly one
  completion result is emitted.
- Lucas completes normal play, repeated analysis, MultiPV tutor sessions,
  search-move restrictions, clock searches, and ponder hit/miss workflows.
- Local unconstrained moves target approximately 250 ms p95 after startup;
  timing and Elo remain local measurements, not flaky CI gates.
