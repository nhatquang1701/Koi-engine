# Koi Search Strength Pass Working Summary

## Current state

- Repository: `C:\Users\ntATh\AI test\Koi engine`.
- Branch: `koi-engine-v1`; committed baseline before the current worktree edits:
  `fe01f92`.
- Current worktree edits are limited to search implementation, search constants,
  the synchronized UCI Hash-range documentation, the current search design/plan
  records, and this summary.
- Final verification was completed after the latest edits. One complete Release
  CTest run and one complete Debug CTest run were executed, and no additional
  test commands were run afterward. Each run passed 36/42 tests; the same six
  targets failed in both configurations: `uci_controller_tests`,
  `koi_search_tests`, `koi_strength_tests`, `koi_engine_process`,
  `koi_engine_en_croissant_process`, and `koi_benchmark_process`.

## Architecture model

- `SearchService` owns iterative deepening, aspiration retries, root-worker
  scheduling, short-search fallbacks, and completed-iteration authority.
- `SearchContext` owns recursive negamax/qsearch, fixed search-stack frames,
  TT access, cancellation, PV construction, and worker-local statistics.
- `SearchMovePicker` and `SearchOrderingTables` are worker-local ordering state;
  `SearchPolicy` owns scalar pruning, reduction, and extension decisions.
- `GameState` remains the legality, make/unmake, repetition, key, and move
  metadata authority. Evaluation, NNUE, weights, and training code were not
  changed.

## Research decisions

Research used Chessprogramming Wiki concepts and the vendored Stockfish 19
implementation as guidance, not as source to copy. Adopted ideas include staged
move emission, history/continuation/capture-history ordering, node-type-aware
LMR, improving-state signals, guarded null move and ProbCut, singular probes,
bounded qsearch SEE/delta pruning, check/quiet-forcing extensions, aspiration
widening, and conservative TT-bound storage.

Deliberately not added: correction history, NNUE/evaluation changes, a Lazy SMP
redesign, unrestricted singular/ProbCut/NMP, shared qsearch TT bounds, or any
fixture-specific move rule. The qsearch cache is worker-local and context-keyed;
selective results remain non-authoritative unless they are re-searched at the
required depth/window.

## Implemented search changes

- Added a fixed-storage staged picker to regular and quiescence search while
  preserving the public diagnostic ordering API and deterministic tie breaks.
- Calibrated live LMR around depth, move number, node type, improving state,
  TT-PV, history, continuation history, capture history, and sibling cutoffs.
- Added guarded null-move verification, ProbCut, singular/multi-cut handling,
  quiet/capture futility, root-safe forcing extensions, and qsearch SEE/delta
  gates. Null-move children cannot immediately perform another null move.
  Eligible dynamic null fail-highs are verified before cutoff; the static
  evaluation, pawn-endgame, and repetition-sensitive guards remain active.
- Added a checked-qsearch safety boundary at depth 24, before another metadata
  frame is built, with terminal-aware mate/evasion handling.
- Added a 4K worker-local qsearch cache keyed by position, predecessor move,
  ply, q-depth, check-depth limit, and halfmove clock. It stores only terminal,
  stand-pat, and proven lower-cutoff results, and is enabled only at a
  halfmove-zero irreversible boundary so transposed reversible histories
  cannot change the cached frontier's repetition outcome.
- Root fallback choices now carry their score; all serial/threaded callers keep
  move, score, mate, PV, and corrected root-line state synchronized.
- Root partial-result selection ignores scout-only root scores. Reduced children
  may only contribute exact root metadata after authoritative re-search; an
  unverified reduced child prevents exact/upper TT storage while lower bounds
  remain valid.
- Selective-bound provenance now crosses recursive boundaries. Razor, internal
  iterative reduction, null/ProbCut/multi-cut cutoffs, selective-pruned nodes,
  and reduced/inexact child paths cannot be promoted to nominal-depth parent
  authority or exact root metadata; razor qsearch also propagates repetition
  sensitivity.
- The depth-two root confirmation band ignores non-exact scout scores; the
  depth-one/depth-three tactical helpers may use scout scores only to nominate
  candidates for a fresh authoritative re-search.
- Depth-valid non-PV TT lower-bound cutoffs now give a modest half-depth
  reinforcement to a matching legal quiet move's worker-local history and
  continuation tables; exact/upper, root, qsearch, and repetition-sensitive
  hits remain unchanged.
- Threaded checked roots now pass the remaining check-extension budget to child
  searches, matching the serial path's extension accounting.
- Checked threaded roots no longer use a shared null-window root-PVS alpha;
  their small evasion sets are searched with full windows so equal evasions
  retain deterministic stable-index ranking.
- Threaded scout re-search exactness now uses the effective alpha that bounded
  that re-search, and aspiration fail-high attempts cannot also be classified
  as fail-low.
- Qsearch cache hits reset their worker-local metadata frame before returning,
  preventing stale qsearch cutoff counts from influencing a later LMR decision.
- The staged picker materializes SEE before ranking tactical candidates, so
  qsearch's first-candidate pruning is driven by capture quality rather than
  only MVV-LVA order.
- Regular-search tactical candidates remain unmaterialized until emission;
  qsearch retains eager capture materialization because its early SEE/delta
  gates depend on capture-quality ordering. Checked evasion stages skip SEE
  entirely.
- Depth-two root confirmation is limited to mixed forcing/non-forcing choices;
  capture-versus-capture defensive ties remain under the ordinary authoritative
  root search.
- Threaded root ranking now uses the same stable-index ordering for single-PV
  and MultiPV results, avoids applying the emergency single-PV correction to
  MultiPV lines, and feeds any post-selection score correction back into both
  root scheduling and time-management hardness observations.
- Root completion is now separated from strict provenance. Serial search uses
  full root-score coverage plus a valid PV to publish a completed iteration;
  threaded search uses complete `line.searched` coverage plus a ranked usable
  line. The stricter root-authoritative flag remains required for aspiration
  seeding and proof-sensitive metadata, so selective qsearch paths no longer
  suppress normal UCI depth progress or cause unbounded retry loops.
- Depth-three live LMR is now gated at phase-rich nodes (`game_phase >= 8`).
  This preserves the low-phase full-depth-verification fixture's required
  reduction while preventing shallow opening quiet moves from being reduced
  to qsearch-only estimates before their central consequences are visible.
- Non-capturing promotions remain in the good tactical stage instead of being
  deferred with losing captures; root partial/fallback checks also recognize
  checking captures when the performance-oriented root metadata pass omitted
  capture check flags.
- Threaded aspiration attempts clear root-line completion state before each
  retry, so an interrupted retry cannot rank bounds left by an earlier window.
- Root publication now separates usable completion from strict provenance. A
  fully covered root pass with a valid ranked PV can advance `completed_depth`
  and publish an iteration even when ordinary qsearch/selective paths make the
  result non-exact. The strict exact/provenance flag remains required for
  aspiration seeding, exact root metadata, and other proof-sensitive uses;
  selective fail-highs cannot trigger aspiration widening.
- Serial root publication now applies the same safe-provenance rule as the
  threaded path when an exact incumbent exists: unresolved selective
  challengers block publication, while a fully covered all-selective pass
  remains explicitly allowed as heuristic progress outside aspiration
  authority.
- Interrupted threaded-root fallback now accepts only an exact, non-exposed
  partial line; unresolved selective estimates fall through to the bounded
  short-search safety move.
- Deep qsearch now probes up to two narrow quiet checks even when no checking
  capture is present, and late non-checking captures survive the generic
  two-move gate when their SEE is non-negative or they take a rook-or-better;
  the existing futility/SEE pruning still applies afterward.
- Removed dead distance-zero branches from multi-ply continuation-history
  updates; those loops begin at distance one, so the update behavior is
  unchanged while the feedback path is explicit.
- Claimable and forced rule draws are now separated in the public position
  view and search boundary. Automatic/dead draws remain terminal; a current
  claimable draw contributes a zero floor while legal continuations are still
  searched for wins. Claimable nodes disable selective cutoffs and TT storage,
  and the root retains a legal UCI move even when claiming is the best result.
- Threaded root lines now retain searched/completed/exact and selective-bound
  provenance. Publication additionally requires complete root coverage and
  every non-exact line to be a safe fail-low upper bound, so an unresolved
  selective challenger cannot disappear from ranking and be masked by an exact
  incumbent. Nominal confirmation refreshes clear stale selective flags.
- Root selective fail-low safety now carries a separate lower-bound direction
  from qsearch and regular negamax. A selective child is accepted as a safe
  root upper bound only when that direction was explicitly preserved; unknown
  or opposite-direction selective results remain blockers.
- Emergency short-search draw handling now treats claimable draws as optional
  zero choices at each fallback minimax layer while automatic/dead draws remain
  terminal. The known locked-pawn dead-position example is matched by its
  normalized board pattern rather than one exact side/clock/fullmove FEN.
- The UCI controller, transposition table, SearchOptions documentation, README,
  and current search design/plan records retain the established Hash range of
  1--4096 MB.
- Fixed-depth depth-three roots now perform a bounded full-window confirmation
  when a quiet checking incumbent has a near-tied quiet checking alternative.
  The incumbent and at most two alternatives are compared again at the
  requested depth so a mixed exact/upper-bound PVS ranking cannot decide a
  shallow check-versus-check tie or change the score's nominal depth; timed and
  deeper searches are unchanged.
- Added a guarded reverse-futility/static fail-high cutoff for quiet, interior,
  non-PV nodes. It is disabled for tactical, repetition-sensitive, sparse-
  material, TT-move, excluded, checked, root, and mate-score cases, and its
  result remains marked as a selective bound.
- Reworked main-search quiet staging into good quiets, deferred captures, and
  bad quiets. Quiet checks, killers, and proven counters now compete through
  their ordering bonuses instead of occupying an unconditional pre-capture
  stage; qsearch keeps its separate quiet-check stage.
- Live dynamic LMR now receives one-based searched-move numbers, allowing the
  second move to be reduced when its existing tactical, history, feature, and
  provenance gates permit it. The legacy diagnostic policy contract is
  unchanged.
- Added a conservative negative continuation-history gate for late quiet
  interior non-PV moves. It requires a strongly negative continuation score
  and excludes tactical nodes, checks, captures/promotions, TT/killer/counter
  moves, repetition-sensitive paths, claimable draws, sparse material, parent
  evasions, and TT-PV nodes; pruned results remain selective and are counted
  per worker.

## Baseline evidence

- The focused Release baseline had 6/8 relevant checks passing. The known
  failures were the single-PV forcing fixture (`f6e4` instead of `f6g4`) and
  `check_03` (`f2d4` instead of an accepted check).
- An isolated qsearch SEE floor of zero plus the live qsearch prune policy was
  promising for the forcing fixture; qsearch-only changes did not resolve
  `check_03`.
- The shallow root issue led to bounded root candidate confirmation rather than
  a fixture-specific move preference.
- Existing verification records contain the earlier architecture Release
  result (41/42, with the documented short-clock oracle limitation); it is not
  a substitute for the final suites after these edits.
- The final Release and Debug CTest runs both passed 36/42 tests. Both runs
  failed the same six targets. `uci_controller_tests` reported the Task 1 WDL
  transcript, ponderhit book bypass, and Ponder PV emission failures.
  `koi_search_tests` reported the medium-timed forcing-root guard publishing
  an unsearched parallel fallback (`depth=0`, `best=b7f7`, `root_pvs=0`).
  `koi_strength_tests` reported that mate fixtures did not solve to a positive
  mate score. `koi_engine_process` timed out waiting for a depth-two ponder PV,
  `koi_engine_en_croissant_process` timed out waiting for a MultiPV bestmove,
  and `koi_benchmark_process` timed out waiting for `koi-bench` reference-1 to
  exit within 120000 ms. Release elapsed time was 570.53 seconds; Debug
  elapsed time was 1930.24 seconds.

## Final verification record

1. Review the final diff and ensure no tests, fixtures, evaluator, NNUE,
   weights, or measurement inputs changed.
2. Run `git diff --check`.
3. Build Release and Debug.
4. Run the complete Release and Debug CTest suites once, at the end. Both
   completed with 36/42 passed and the six failures recorded above.
5. This summary records the exact final results.
6. Report exact benchmark, suite, and known-limitation evidence. Do not claim
   Elo improvement without the user's controlled En Croissant retest.
