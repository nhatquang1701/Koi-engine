# Koi Engine Architecture Rework Design

Date: 2026-09-11

## Objective

Rework Koi into an architecture that can support substantially stronger rules,
search, evaluation, NNUE, time-management, and parallel-search development
without another foundational rewrite. The migration must preserve standard
chess correctness, the existing UCI/Lucas compatibility contract,
deterministic `Threads=1` behavior, the current measurement infrastructure,
and the Windows x64 C++26 build contract.

This is an architectural migration, not a Stockfish port. Stockfish 19 is an
external engineering reference for state ownership, per-ply state, search
worker separation, compact TT data, time-management seams, persistent worker
lifecycle, and incremental NNUE state. Koi keeps its own value-oriented rules
boundary, compatibility behavior, deterministic root-parallel policy, and
public API.

## Scope and non-goals

The target covers the complete engine path:

```text
UCI protocol/controller
        |
        v
runtime/search session -- time manager
        |
        +-- root coordinator -- per-worker search context/stack
        |                         +-- move ordering and heuristics
        |                         +-- pruning/reduction/extension policy
        |                         +-- shared transposition table
        |                         +-- evaluation interface
        |
        v
rules state -- legal moves, make/unmake, hashing, repetition
        |
        +-- compatibility adapters: book, tablebase, optional shadow oracle
        +-- evaluation features and NNUE worker state
```

The migration does not immediately:

* copy Stockfish code or make Koi's public API resemble Stockfish's internals;
* add Chess960, variants, or unsupported UCI options;
* remove the vendored chess-library mirror before its consumers and differential
  safeguards have been migrated;
* make NNUE the default evaluator;
* replace deterministic root parallelism with Lazy SMP before the single-thread
  reference path and shared-state contracts are stable; or
* claim Elo improvement from node-count or wall-clock changes alone.

## Current-state audit

The repository already contains a real native rules migration and a working
release gate. The relevant facts at the time of this design are:

* `src/koi/position.cpp` contains the Koi-owned native board, bitboards,
  incremental key, legal move generation, reversible history, null moves, and
  rule-state logic.
* `GameState` currently owns both that native `Position` and a
  `chess::Board` compatibility mirror. The native position is authoritative,
  but generated search moves still update the mirror and selected completion
  and diagnostic paths compare both states.
* `GameState` also owns feature caching, generated move metadata, static
  exchange support, tablebase snapshots, and mirror-consistency diagnostics.
  Those responsibilities are useful, but they are not yet distinct ownership
  boundaries.
* `src/koi/search_service.cpp` is approximately 3,900 lines and currently
  combines iterative deepening, quiescence, pruning and reductions, evaluation
  caching, root selective searches, root-worker coordination, cancellation,
  and result publication.
* `src/koi/uci_controller.cpp` contains the asynchronous UCI lifecycle,
  generation filtering, completion validation, opening-book/tablebase routing,
  debug logging, and protocol formatting. Its single output path is a good
  compatibility boundary to preserve.
* The C++26 module partitions currently export small value contracts. They are
  useful compile-time boundary markers, but they do not yet enforce the
  ownership boundaries of the implementation files.
* The compatibility mirror remains needed by Polyglot-key/book behavior,
  completion validation, and diagnostics. Syzygy already consumes a Koi-owned
  `TablebaseSnapshot`; this is evidence that adapter isolation is viable.
* The current Release build passed all 38 registered CTest tests in 208.78
  seconds before this design work. The only worktree change at audit time was
  the temporary user-provided `Goal.txt`.

These facts mean the correct starting point is a strangler migration around
the existing contracts, not a replacement of the proven rules and UCI paths.

## Architecture decisions

### 1. One authoritative rules state

`Position` remains the production authority for board contents, occupancy and
piece bitboards, side to move, castling and en-passant state, halfmove and
fullmove counters, the position key, repetition history, legal move
generation, and make/unmake transitions.

`GameState` remains the compatibility-facing value type during migration, but
its implementation is reorganized around explicit internal roles:

* `RulesState` owns or directly contains the native `Position` and its
  search-safe state history.
* `CompatibilityMirror` is a private adapter used only when a legacy
  consumer, root completion validator, or explicit differential diagnostic
  needs the vendored chess-library representation.
* `FeatureState` owns feature snapshots and cache lifetime. Feature snapshots
  are derived from the authoritative rules state and are invalidated or
  advanced by make/unmake, never treated as an independent source of truth.

The first migration may continue updating the mirror transactionally so that
behavior remains reversible. The later mirror-removal gate is explicit: each
remaining consumer must be migrated to a Koi-owned snapshot or adapter, the
shadow differential target must remain independently runnable, and the full
rules/process/benchmark gates must be green before production mirror storage
is removed.

### 2. Search is a first-class subsystem

`SearchService` remains the stable façade used by UCI and tests. Its private
implementation is decomposed into the following concepts:

* `SearchSession`: one root, one immutable options/limits snapshot, one
  cancellation/completion state, one time manager, and one result identity.
* `SearchContext`: one worker's mutable search state, including a copied rules
  position, per-ply stack, principal-variation storage, local statistics, and
  evaluation state.
* `SearchStack`: fixed-capacity per-ply frames for transient values such as
  static evaluation, current/previous moves, check state, move count, and
  reduction/extension bookkeeping. Normal recursive search performs no heap
  allocation.
* `MoveOrdering`: TT/PV moves, captures/promotions, killers, quiet/capture
  history, countermoves/continuation history, and future ordering policies.
  Ordering policy owns its tables and scoring; recursive search asks it for a
  ranked move stream rather than embedding every heuristic.
* `SearchPolicy`: a coherent home for pruning, reductions, and extensions.
  Policy decisions receive explicit node context and return a decision or
  depth adjustment; they do not mutate UCI or runtime state.
* `RootCoordinator`: root move filtering, iterative-depth coordination,
  MultiPV ranking, deterministic tie-breaking, root-worker scheduling, and
  final line selection.
* `SearchPublisher`: an internal event boundary that turns completed
  iterations into `SearchInfo` and one final `SearchResult`; the controller
  remains the only protocol writer.

The first implementation can move existing logic without changing formulas.
The architectural acceptance criterion is that a future heuristic can be
added to `MoveOrdering` or `SearchPolicy` without editing UCI, rules, TT
storage, or result validation.

### 3. Evaluation and NNUE are independent of search

The evaluator interface remains the search-facing contract. Evaluation is
split into:

```text
RulesState / PositionView
        -> FeatureExtractor
        -> EvaluationState (classical cache or NNUE accumulator stack)
        -> Evaluator implementation
        -> score from requested perspective
```

Classical evaluation remains the safe default. A future NNUE evaluator owns
immutable network data separately from per-worker mutable accumulators and
caches. Network format/version validation stays in the NNUE adapter, not in
`SearchContext` or `Position`. Feature-set identity, network lifetime,
refresh conditions, king-bucket changes, and scalar/SIMD equivalence are
explicit contracts.

This follows the useful Stockfish separation between `Position`, the worker's
`AccumulatorStack`, per-thread accumulator caches, and immutable `Network`,
while retaining Koi's existing versioned opt-in container.

### 4. TT is shared storage, not shared search state

The transposition table remains an engine-wide shared service with an isolated
physical representation. Search sees only probe/store/clear/resize operations
and score/bound/mate-distance contracts. It does not depend on cluster layout,
allocation strategy, or memory policy.

Entries must define key validity, depth, bound, normalized score, best move,
generation/age, and replacement policy. Concurrent probe/store assumptions are
documented and tested. Resize and clear are runtime lifecycle operations that
cannot race an active search. A TT collision is never allowed to become a
second source of rules truth.

Stockfish's compact clustered entries, generation aging, and lock-free hot
probe pattern are references for these concerns; Koi's existing Windows memory
policy and bounded hash contract remain authoritative.

### 5. Runtime, time, and UCI remain outside search algorithms

The runtime owns configuration snapshots, active-search lifecycle, cancellation
and join semantics, and worker construction. The time manager converts UCI
limits and clock state into a search budget and observes iteration behavior; it
does not recurse or choose moves. The UCI controller parses commands,
maintains protocol state, serializes output, and rejects stale generations.

Books, Syzygy, and future external resources remain adapters. A missing or
malformed asset must result in a safe fallback to core search, never a rules or
search ownership dependency.

### 6. Module partitions expose contracts, not implementation promises

The existing `koi`, `koi:types`, `koi:position`, `koi:eval`, `koi:tablebase`,
`koi:search`, and `koi:runtime` partitions remain, but their contracts are
updated only when a real ownership boundary is stable. Internal headers and
translation units remain private. No public module or header may expose
`chess.hpp`, a worker implementation, a TT storage entry, or a particular NNUE
network representation.

## Migration stages

The overall goal is decomposed into independently verifiable stages. Each
stage is allowed to stop at a clean boundary and must leave the previous
release gates usable.

### Stage 1 — State and adapter ownership seam

Create the internal rules/mirror/feature ownership seam around the current
`Position` and `GameState` implementation. Keep public `GameState` behavior and
transactional semantics unchanged. Add focused tests for native authority,
mirror synchronization, make/unmake/key restoration, null moves, generated
metadata provenance, and the fact that mirror checks are only performed at
explicit validation or diagnostic boundaries.

The concrete Stage 1 seam is now represented by
`src/koi/detail/compatibility_mirror.{hpp,cpp}`,
`src/koi/detail/feature_state.{hpp,cpp}`, and the private `GameState::Impl`
composition. `Position` remains the authoritative rules state. The
`CompatibilityMirror` owns the vendored board, shadow history, conversions,
comparison normalization, and compatibility queries; `FeatureState` owns
feature-cache storage, publication, invalidation, and diagnostics derived from
`Position`. `make_search_move` continues to update the mirror transactionally
but skips interior mirror comparison, while explicit validation and diagnostic
boundaries compare the two states.

The mirror is intentionally retained because Polyglot-key/book behavior,
completion validation, and differential diagnostics still consume it. Mirror
removal is gated on migrating or deliberately isolating each remaining
consumer, preserving an independently runnable shadow-differential target, and
passing the full rules, process, and benchmark evidence described below.

Inventory and migrate consumers of the compatibility mirror one at a time:
Polyglot key/book selection, completion validation, debug snapshots, and any
remaining external adapter. Do not remove the mirror until the consumer list
is empty or each consumer is behind a deliberately retained compatibility
adapter.

### Stage 2 — Search session and stack decomposition

Extract `SearchSession`, `SearchStack`, `SearchContext`, and
`RootCoordinator` from `search_service.cpp` behind the existing
`SearchService`/`SearchHandle` API. Preserve search formulas and the
`Threads=1` deterministic reference path first. Add structural tests for one
owner per piece of state, fixed-capacity PV/stack behavior, cancellation, and
exactly one completion.

The Stage 2 boundary is now represented by the private files
`src/koi/detail/search_session.{hpp,cpp}`,
`src/koi/detail/search_stack.hpp`,
`src/koi/detail/search_context.{hpp,cpp}`,
`src/koi/detail/search_context_support.{hpp,cpp}`,
`src/koi/detail/search_constants.hpp`, and
`src/koi/detail/root_coordinator.{hpp,cpp}`. `SearchService` constructs a
`SearchSession`, whose worker creates a `SearchContext`; the context owns the
fixed-capacity recursive stack and search-local state, while
`RootCoordinator` owns root-line ranking and stable tie-breaking. The
`RootWorkerPool` scheduling loop and some root policy remain in
`search_service.cpp` as an explicit migration boundary for the next stages;
this stage does not claim that search scheduling, ordering, or pruning policy
is fully extracted.

### Stage 3 — Ordering and search-policy seams

Move ordering tables and move ranking behind a focused internal interface, then
group pruning/reduction/extension decisions into a policy layer. Keep current
heuristics and diagnostics intact while adding tests that isolate each policy
decision. Only after this stage may new pruning, LMR, singular, or history
experiments be introduced.

The Stage 3 boundary is represented by the private
`src/koi/detail/search_ordering_tables.{hpp,cpp}` and
`src/koi/detail/search_policy.hpp` seams. `SearchMoveOrdering` remains the
stable internal ranking façade but delegates adaptive killer/history/counter /
continuation state to `SearchOrderingTables`. `SearchPolicy` returns scalar
decision records for null moves, check extensions, LMR, quiet futility, and
quiescence capture pruning; `SearchContext` applies those records and retains
position-derived forcing checks, recursion, and statistics. No new heuristic is
introduced in this stage, and the next architectural work remains evaluation,
TT/time/parallel runtime, and final tooling/module/strength validation.

### Stage 4 — Evaluation state and NNUE evolution seam

Separate feature extraction and per-worker evaluation state from rules and
search recursion. Keep classical evaluation behavior stable. Validate NNUE
feature sets, accumulator updates, refresh paths, scalar/SIMD parity, network
lifetime, and fallback behavior independently.

The Stage 4 execution boundary is represented by the optional public
`EvaluatorWorker` capability, `NnueEvaluator::create_worker()`, and the private
`src/koi/detail/evaluation_context.{hpp,cpp}` owner. `SearchContext` delegates
evaluation through `EvaluationContext`; stateful NNUE workers and their
accumulators are local to that context, while immutable network data remains
owned by the shared `NnueEvaluator`. Evaluators without a worker retain the
existing mutex-guarded fallback. Feature extraction and network format remain
value-oriented public evaluation contracts, and no NNUE representation leaks
through search or rules modules.

### Stage 5 — TT, time, and parallel-runtime hardening

Finalize TT score/age/replacement contracts, time-manager observability, and
thread-local/shared-state rules. Measure cold/warm hash behavior, resize/clear
latency, cancellation, thread scaling, and deterministic result parity before
considering Lazy-SMP-style work.

### Stage 6 — Tooling, modules, documentation, and strength validation

Align module contracts, README/architecture documentation, perft/replay/UCI
process tests, benchmark profiles, Stockfish oracle tooling, release gates,
and match reports with the resulting implementation. Strength claims require
appropriate comparative evidence; NPS and deterministic node signatures remain
separate measurements.

## Verification contract

Every behavioral change follows RED -> GREEN -> refactor and records the
focused command before the broader gate. Architectural moves that claim no
behavior change still require:

* public-header scans proving no chess-library type leaks;
* rules differential/perft and make/unmake/key/repetition tests;
* deterministic fixed-depth `Threads=1` node/score/PV checks;
* legal best-move/PV and completion-gate process tests;
* Debug and Release CTest/process suites;
* cold/warm benchmark profiles with explicit limits and options;
* performance comparisons for perft, benchmark nodes, search speed,
  evaluation/feature cost, memory, and thread scaling where applicable; and
* documentation and module-contract checks matching the implementation.

The current 38-test Release run is the starting regression gate, not proof that
the architecture is complete. A retained change must explain any node-count,
wall-clock, memory, or strength movement. A tactical, legality, protocol,
determinism, or unexplained major performance regression stops that stage.

## First implementation slice

The first plan will implement Stage 1 only. It will be deliberately small in
behavioral surface but meaningful in ownership:

1. Introduce the private compatibility-mirror seam without changing the
   `GameState`, `Position`, or UCI public contracts.
2. Make the authoritative native state, mirror state, feature cache, and
   diagnostic comparison paths explicit in code ownership.
3. Add or strengthen tests that prove transactional make/unmake, native-key
   authority, mirror parity, and the search fast-path's validation boundary.
4. Record a benchmark/profile baseline and rerun the full Release gate before
   and after the seam extraction.
5. Update the architecture documentation to describe the actual first-stage
   ownership, including why the mirror remains and what evidence is required
   before its removal.

No search heuristic or UCI behavior is changed in this first slice. That keeps
the migration reversible while creating the boundary required for the later
search decomposition.

## Acceptance criteria

The rework is complete only when the target ownership model is reflected in
the code and documentation, all current compatibility/tooling gates remain
usable, and the ultimate future-change test passes: a stronger NNUE/feature
set, new ordering/history/pruning/reduction/search experiments, improved time
management, future parallel search, better tablebases, and SIMD work can be
implemented primarily inside their respective subsystems without another
foundational rewrite.

For Stage 1 specifically, completion requires the focused ownership and
differential tests, unchanged public behavior, a recorded cold/warm benchmark
baseline, full Debug/Release verification where available, clean diff checks,
and an updated repository architecture description. `Goal.txt` is a temporary
task input and must not be deleted until the complete multi-stage objective is
actually satisfied.
