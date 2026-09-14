# Koi Search Strength Pass Design

## Goal

Raise Koi's measured five-round En Croissant accuracy from 93.841% toward a reproducible 95% or better under the existing target conditions: one second per move, four threads, 4 GB Hash per engine, book disabled, and Stockfish 19 full-game analysis.

The user will perform the accuracy match. This work does not claim the target until that retest is completed.

## Scope and constraints

- Use only the vendored Stockfish 19 source under `third_party/stockfish-19/stockfish-windows-x86-64-universal/stockfish/src` as search guidance.
- Change search, move ordering, search policy, search scheduling, and supporting state only when required by those search changes.
- Do not change `ClassicalEvaluator`, evaluation parameters, NNUE code, NNUE weights, evaluation-training tools, tests, test fixtures, or the User-tests PGN.
- Do not run the accuracy match during implementation.
- Run the complete Release and Debug suites only after the implementation is judged complete.
- Preserve deterministic `Threads=1` fixed-depth behavior, legal PVs, cancellation, completed-iteration authority, repetition safety, and the established 4096 MB Hash boundary.

## Design

### 1. Staged move selection

Add a worker-local staged picker around the existing fixed `MoveMetadataList`. The picker emits one move at a time in Stockfish 19's ordering shape: a valid TT move, good captures/promotions, history-ranked good quiets (with quiet checks, killers, and proven counters receiving their existing priority bonuses), deferred losing captures, and bad quiets. Checked nodes use a complete evasion stage. The picker keeps deterministic tie-breaking and honors excluded moves without allocating or changing public diagnostic ordering APIs.

Move metadata remains the source of legality and move identity. SEE is computed lazily for capture candidates rather than for every generated move before sorting. Existing history tables remain worker-local; their scores are used by the picker and continue to receive updates from searched moves.

### 2. Search integration and selective-search calibration

Use the staged picker in regular negamax and quiescence. Preserve qsearch's complete checked evasions, TT ordering hint, bounded quiet-check continuation, delta pruning, and capture-history ordering. Recalibrate the integer LMR decision to follow Stockfish 19's depth/move-number/node-type/history shape while retaining Koi's full-depth re-search whenever a reduced move challenges alpha. Add conservative reverse-futility/static fail-high and strongly negative continuation-history gates for quiet interior scout nodes, with selective-bound provenance; the continuation gate is limited to late, non-tactical moves and excludes TT, killer, counter, repetition-sensitive, and sparse-material cases. Keep ProbCut, null-move verification, singular extension, and multi-cut guarded against repetition-sensitive, excluded, checked, mate, tactical, and sparse-pawn positions. Add a worker-local qsearch cache keyed by the tactical frontier context; retain only terminal, stand-pat, and proven lower-bound entries, and disable it where reversible history can affect the result. Treat automatic/dead draws as terminal, but model claimable draws as a zero-valued option while continuing to search legal moves for a better score; claimable nodes bypass selective cutoffs and TT storage. Shared qsearch bounds remain outside the regular TT format. A fully covered root pass with a valid PV may advance public completed-depth state and publish an iterative PV/score even when ordinary qsearch/selective provenance is not a strict nominal-depth proof. Keep the stricter exact/provenance flag for aspiration seeding, exact TT/root metadata, and other proof-sensitive decisions; selective fail-highs do not widen the root window.

Selective searches may save lower bounds only when the searched move and bound are trustworthy. A depth-valid non-PV lower-bound TT cutoff may modestly reinforce a matching legal quiet move in the worker-local history tables; exact/upper hits and correction-history or static-evaluation behavior remain unchanged. Selective-bound provenance is propagated through razor, internal iterative reduction, null/ProbCut/multi-cut cutoffs, selective-pruned nodes, and inexact child paths so those results cannot become nominal-depth parent authority or exact root metadata.

### 3. Root and one-second scheduling

Retain the completed-iteration authority and short-search safety fallbacks. Feed the previous principal move and staged root order into each iteration, preserve PV/TT candidates through aspiration retries, and avoid allowing a partial multi-threaded root result to replace a completed iteration. Keep fixed-depth single-thread ordering deterministic and keep the private four-thread root pool cancellation-safe.

## Acceptance evidence

1. The final diff contains no test, fixture, evaluation, or NNUE changes.
2. Release and Debug builds complete successfully.
3. The full Release and Debug CTest suites are run once after implementation and their exact pass/failure results are reported.
4. The user retests the engine under the target conditions; the accuracy result remains the authoritative measure of the 95% goal.
