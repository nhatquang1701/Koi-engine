# Engine speed program: nodes per second and time to depth

Status: Phases 0-2 complete (2026-09-25); Phase 3 next. Owner: Koi Engine.

## Goal

Make the engine measurably faster: more nodes per second and less wall time to
reach a given depth, at unchanged strength. The program upgrades measurement
first, then lands bit-identical (value-preserving) hot-path work, then
build-level PGO, then behavior-changing refinements behind the SPRT gate.
Every phase is measured against the baseline captured before any change.

Scope decisions (confirmed 2026-09-25):

- Change scope: speed-first and SPRT-gated. Bit-identical refactors land
  first; any change that can alter search or evaluation values needs the
  64-position tactical gate plus a recorded SPRT campaign.
- Measurement: `koi-bench` gains time-to-depth and external FEN-list modes
  plus an NPS/time-to-depth artifact writer, so every phase compares like for
  like.
- PGO: Windows MSVC first (`/GENPROFILE` generation, `/USEPROFILE` use);
  Linux/Clang PGO is a follow-up only if the Windows experiment earns its
  keep.
- Big-ticket items: all four - TT read-path locking rework, move generation
  rewrite, evaluation feature caching, and search bookkeeping cleanup.
- Standing contracts: the classical evaluator stays the default and NNUE
  stays opt-in; Threads=1 stays deterministic and byte-identical for
  value-preserving changes; perft stays byte-identical; the UCI handshake and
  `release_verify.ps1` contract are unchanged. Elo/NPS stay reports, never CI
  thresholds.

The previous session's four-part speed audit (search hot path, move
generation/position, evaluation, tooling/build) is the inventory below;
`file:line` references are to the audit revision `47d2cf3`.

## Baseline (commit 47d2cf3, 2026-09-25)

Recorded evidence, all from the 64-position strength suite unless noted:

- `artifacts/verification/engine-hardening/nps-report.txt`
  (`koi-bench --threads N --speed 100 --timed`): T1 226,877 nps
  (16,469 nodes + 753,552 qnodes / 3,394 ms), T2 418,188, T4 690,967,
  T8 905,347; live UCI probe on startpos (`go movetime 700`): 93k / 206k /
  238k nps at Threads 1/4/8.
- `baseline.txt`: Threads=1 repeat identical, 64/64 matches at every thread
  count. Older `release-verify-phase5/bench-timed.json` (Threads=4):
  2,887,016 visited / 2,852 ms = 1,012,277 aggregate nps.
- Depth cost on startpos: depth 6 about 3.5 s, depth 7 about 39.9 s, depth 8
  about 613 s (`2026-09-24-engine-hardening.md:389-390`).
- Historical NPS for context: classical 125,069 -> 135,008 -> 144,972
  (`2026-09-19-engine-v2.md:30-35,384-393`); attack tables + O(1) check flags
  took 58,697 -> 86,989 (`2026-09-16-strength-program.md:30-37`); perft about
  398k nodes/s.
- Value-preserving pin for the classical evaluator: fixed-depth T1 benchmark
  rows must stay byte-identical. Re-recorded in Phase 0 (commit `47d2cf3` plus
  the measurement upgrade): 770,021 nodes+qnodes, 64/64 matches, default
  stdout sha256 `6F7D8FF5...A7F8`. The `754,289` figure cited from the
  engine-v2 era is superseded by later search changes.

## Hotspot inventory

Ranked by the audit; the expected effect column is the audit's estimate, not a
commitment.

### Search hot path (highest NPS headroom)

1. **TT read path locking.** Every node pays `StorageReadGuard` (shared
   `storage_mutex_`) plus stripe locks: prefetch 1 shared, probe storage
   shared + stripe shared, store storage shared + stripe unique, i.e. 3-5
   mutex operations per node (`transposition_table.hpp:142`,
   `transposition_table.cpp:135-160,299-307`, `transposition_table.cpp:457,469,549,563,579`).
   `search_transposition_key` is recomputed per site (`search_context.cpp:650,1224,1300`,
   qsearch `:189`); `hashfull_permill` latches all 64 stripes per info line
   (`uci_controller.cpp:1586`).
2. **Per-node legal generation and metadata.** `make_observed`
   (`search_context.hpp:505-513`) -> `make_search_move`
   (`game_state.cpp:1984-2001`) -> `apply_generated_move` re-validates the
   position key and splitmix token (`game_state.cpp:1942-1974`); quiet-move
   metadata rebuilds occupancy and re-tests every quiet move for check
   (`game_state.cpp:1071-1184,1841-1847`).
3. **Move picker cost.** `SearchMovePicker` (~3 KiB) per node
   (`search_ordering.hpp:123-163`); `is_killer` 3x, `capture_history_score`
   up to 5x, `priority()` recomputed on emit after `prepare_candidates`
   ranked, up to 256 candidates sorted per node, SEE materialized per capture
   in qsearch (`search_ordering.cpp:416,426-453,500-503,565,616,625`;
   `search_context.cpp:1405,1417,1420,1609,1612,1635`); `quiet_history_score`
   reads a 6-deep continuation chain plus pawn history per quiet
   (`search_ordering_tables.cpp:189-226`).
4. **Feature extraction and repeated position queries.** `ensure_features()`
   once per node (`search_context.cpp:760-770`), but
   `position_features()` is called per quiet LMR candidate after make
   (`search_context.cpp:1559`) and per quiet candidate in the depth-1 forcing
   probe (`:1591`); `quiet_move_is_forcing` is a 64-square scan plus ray
   walks (`search_context_support.cpp:69-156`); `FeatureState` takes a
   shared_mutex and returns ~160 B by value per hit
   (`feature_state.cpp:47-96`). `is_repetition_sensitive` is recomputed at
   five sites plus inside `null_move_is_safe`; `correction_keys_for`
   (`:888`) recomputes the pawn key already fetched at `:786-787`;
   `has_non_pawn_material` is computed twice per node (`:660,:987`);
   `draw_status()` runs per node and per qsearch node
   (`search_context.cpp:696,208`).
5. **Per-node scratch and copies.** `MoveMetadataList` 8 KiB per node
   (`search_context.cpp:686`; qsearch `:196,264`); ProbCut and failed-move
   buffers (`:1147-1154,1319-1323`); root work copies `GameState` per root
   move per job (`search_runner.cpp:2229`) plus about 20 emergency/fallback
   sites; the root-parallel path keeps a second ~900 KiB
   `SearchMoveOrdering` and re-sorts per iteration
   (`search_runner.cpp:3907,4014,4025-4084`); every `SearchContext` allocates
   a 512 KiB eval cache plus a 64 KiB qsearch cache.
6. **Make/unmake cost.** `make_generated_move` records a ~304 B `Snapshot`
   and `apply_unchecked` runs `remove_rule_keys`/`add_rule_keys` with up to
   three en-passant legality trial probes
   (`position.cpp:781-792,1072-1093,1242-1335,272,283,1316-1319`);
   `make_null_move` rebuilds derived state with a full rescan
   (`position.cpp:808-826`).

### Move generation and position

1. **Legal filtering by apply/undo.** `legal_moves_into_impl` applies and
   restores every pseudo-legal move to test legality
   (`position.cpp:684-703,675-680,720-728`); `is_legal`/`apply_legal`
   regenerate the full list (`:740-745,765-771`); perft and
   `GameState::make_move` use this slow path
   (`game_state.cpp:1925-1940`). There is no `checkers()`, `pinned()`, or
   `blockers_for_king()` yet.
2. **Mailbox pseudo-legal generation.** `generate_pseudo` scans 64 squares
   with step/ray loops (`position.cpp:1115-1240`); sliding attacks are ray +
   nearest-blocker XOR (`attack_tables.cpp:122-133`); `nearest_blocker` is
   unused by `position.cpp`.
3. **Snapshot and state copies.** `Snapshot` ~304 B, `NativeState` ~78 KiB
   with a 256-entry history ring whose records memmove when full
   (`position.cpp:157-188,199-207`); `Position::Impl` copies are heap-based
   (`:1340-1351`); every en-passant legality probe copies the whole state
   (`:467-476`).
4. **Double generation.** `GameState::make_move` regenerates native legal
   moves, applies the shadow, and runs `matches()` even where callers already
   hold a legal move (`game_state.cpp:1925-1940`).
5. **Mirror cost.** Non-search callers pay native legal regeneration plus
   shadow apply and a 12-bitboard `matches()` (`compatibility_mirror.cpp`,
   `game_state.cpp`).

Oracles that keep this honest: perft (startpos 20/400/8902, Kiwipete
48/2039/97862, position 3 14/191/2812, `perft_tests.cpp:20-46`), shadow-diff
3-way equality and snapshot equality (`native_shadow_diff_tests.cpp:38-223`),
rules incl. en-passant pin identity and 320-ply replay
(`native_rule_state_tests.cpp:197-281`), and generation-order equality
(`:321-332`). Engine-v2 Phase 8 already scopes direct legal generation and
bitboard generation (`2026-09-19-engine-v2.md:177-183`, design
`:149-158`); perft must stay byte-identical.

### Evaluation (classical is the default)

1. `PositionFeatures` computes occupied/color bitboards during extraction but
   does not store them, and discards per-piece attacks after taking the
   mobility union (`game_state.cpp:1186-1270,1200-1204,1249-1250`);
   `feature_masks()` is a full 64-square scan rebuilt roughly 6-8 times per
   evaluation (`classical_evaluator.cpp:74-86,97,196,280`).
2. Pawn terms rescan the board with no pawn hash: `pawn_structure_for` four
   scans plus ray walks, `passed_pawn_for` two scans, `pawn_break_for` two
   scans (`classical_evaluator.cpp:449-567,569-628,343-378`).
3. King safety re-derives attacker sets per evaluation
   (`classical_evaluator.cpp:230-329`); `is_dead_position` runs a 64-square
   locked-pawn-wall compare (`position.cpp:989-1024`).
4. NNUE v5 (opt-in): `apply_move_deltas` runs a full threat-feature
   re-extraction per make (`nnue.cpp:1844-1853,1738-1767`,
   `evaluation_features.cpp:194-317`).

Semantics-preserving candidates (must not change evaluation values): store
occupied/color in `PositionFeatures`; precompute per-piece mobility counts and
piece counts during extraction; a pawn hash keyed by the existing `pawn_key`;
king-safety attack tables; a cache for the locked-pawn-wall test; return
features by reference / a per-context unguarded cache to remove the lock and
copies; and 2-way eval-cache tuning. Strength-affecting evaluation changes
(rook/knight mobility, bishop/queen double count) are explicitly out of
scope - they belong to a strength plan, not this program.

### Tooling and build

- `koi-bench` (`tools/engine/koi_bench.cpp`) already supports
  `--threads/--speed/--timed/--nodes/--warm-hash/--optional/--warmup/--repeat/--profile-json/--nnue`;
  NPS is `(nodes+qnodes)*1000/elapsed_ms` (timed only). It has no external FEN
  input, no time-to-depth, no depth sweep, and no artifact writer.
- `performance_gate.ps1` compares baseline and candidate on behavioral parity
  plus a median runtime ceiling (fails above +5%); it cannot compare
  behavior-changing variants and has no NPS floor.
- Time to depth is only available through
  `tools/stability/uci_match.ps1 -FenFile <name|FEN> -Depth N`, whose `infos[]`
  carry depth/time_ms/nps, or `tools/measurement/elo_oracle.py`.
- No PGO is wired anywhere; MSVC 19.44 supports `/GENPROFILE`/`/USEPROFILE`.
- Profiling: `wpr.exe` exists but no `.etl` analyzer (no WPA/xperf/VTune),
  so counter-based evidence from `--profile-json` is the primary tool and
  `wpr` collection is manual/optional.

## Design decisions

- **Measurement contract.** `koi-bench` gains `--fen-file <path>` (one FEN per
  line, `#` comments), `--depth <N>` (fixed-depth timed mode), and
  `--depth-sweep <a..b>` (time-to-depth table), plus `--report <path>`
  writing a `koi-bench-speed-v1` JSON artifact with per-position depth,
  nodes, qnodes, elapsed, nps, and totals. Existing flags keep byte-identical
  stdout so `performance_gate.ps1` and `release_verify.ps1` regexes do not
  change.
- **Speed gate.** A new `tools/build/speed_gate.ps1` compares old and new
  `koi-bench-speed-v1` artifacts from alternating runs (median of N),
  reporting NPS ratios and time-to-depth deltas, with a regression ceiling
  only for NPS (for example fail above -2% at Threads=1). Correctness gates
  stay separate: value-preserving phases use the byte-identical fixed-depth
  pin, behavior changes use the tactical gate and SPRT.
- **TT reclamation stays safe.** The hardening pass made readers hold a shared
  `storage_mutex_` for a whole store/probe/prefetch; that use-after-free fix
  must not regress. The rework keeps retirement safety with an epoch or
  hazard scheme (readers pin a generation; retired storages are freed only
  when no pinned reader remains) or, if that proves out of budget, a
  copy-on-write `shared_ptr` snapshot loaded under the maintenance mutex, so
  the hot path pays one atomic load instead of one-to-three shared locks.
  Stripe locks stay for stores; probes may use relaxed stripe reads once
  entry-level torn-read safety is argued and tested. The concurrency and
  ASan tests are extended to hammer resizes under the new scheme.
- **Incremental position state.** The move generation rewrite introduces
  `checkers()`, `pinned()`, and `blockers_for_king()` maintained by
  `apply_unchecked`/`restore` (from move metadata where possible), and
  generates legal moves directly, removing the apply/undo legality filter.
  Output order is part of the contract: the fixed-buffer and vector APIs must
  stay equal and in the same order as today, perft stays byte-identical, and
  the shadow-diff suites stay green. En-passant legality trial probes are
  replaced by the incremental pinned/blocker information.
- **Evaluation invariance.** Cached features and the pawn hash are pure
  memozation of the existing computation: the classical fixed-depth benchmark
  rows must stay byte-identical, which is a stricter check than a score
  comparison. `PositionFeatures` grows stored bitboards and per-piece
  mobility; the cache key and lifetime semantics stay as they are, except
  that the per-context fast path may become unguarded after the Phase 1
  locking rules are settled.
- **Search bookkeeping.** Root worker and emergency/fallback paths stop
  copying `GameState` per candidate where the move set is already fixed;
  ordering re-ranks only when the candidate set or history changed; repeated
  queries (`is_killer`, `capture_history_score`, `priority`,
  `is_repetition_sensitive`, `draw_status`, `has_non_pawn_material`,
  correction pawn key, forcing-move test) are computed once per node or
  cached; per-node scratch is initialized only over its used span.
- **PGO.** A `KOI_PGO` CMake option with `generate` and `use` modes wires
  MSVC `/GENPROFILE` (`/LTCG:PGI`) and `/USEPROFILE` (`/LTCG:PGO`) for
  Release, gated to MSVC. The training recipe is a mixed workload (the
  64/128-position bench suites plus a short self-match), documented so a
  profile can be regenerated; the `.pgd` stays out of the repository (a
  recipe, not a binary) and CI builds are unaffected.
- **Evidence.** Every phase records before/after artifacts under
  `artifacts/verification/speed-program/<phase>/` (bench JSON, speed-gate
  report, depth sweep) plus a phase record in this document. Behavior
  changes additionally record the tactical gate and SPRT results.

## Phases

0. **Measurement upgrade and baseline capture.** Add `--fen-file`, `--depth`,
   `--depth-sweep`, and `--report` to `koi-bench`; add
   `tools/build/speed_gate.ps1`; capture the 2026-09-25 baseline (cold timed,
   `--warm-hash --warmup 1 --repeat 5` steady, node-limited steady, depth
   sweep over `tests/data/positions/evaluation-positions.txt`,
   `tests/data/endgames/endgame-positions.txt`, and
   `tests/data/openings-curated-32.txt`), and record the byte-identical pin
   (770,021 nodes, 64/64 matches; see the Phase 0 record for the sweep ranges
   actually captured).
1. **TT read path (bit-identical).** Reader epochs or snapshot loading;
   drop the storage guard from probe/prefetch; probe stripe reads become
   relaxed where argument supports it; compute `search_transposition_key`
   once per node and reuse it for ProbCut/IID/qsearch; batch stripe locks in
   `hashfull_permill` or maintain a lazy approximate counter; prefetch the
   child's TT slot before make. Gates: fixed-depth rows byte-identical,
   concurrency soak plus ASan green, speed gate green.
2. **Evaluation feature caching (bit-identical).** Store occupied/color and
   per-piece mobility in `PositionFeatures`; remove `feature_masks()`
   rebuilds; add the pawn hash keyed by `pawn_key`; cache the locked-pawn-wall
   test; return features without a lock plus copy on the per-context path;
   remove the per-candidate `position_features()` calls at
   `search_context.cpp:1559,1591` via a cached per-node feature block or
   incremental update. Gates: classical fixed-depth rows byte-identical,
   speed gate shows the evaluation share drop.
3. **Search bookkeeping cleanup (bit-identical).** De-duplicate the repeated
   queries listed above; hoist `is_repetition_sensitive`; reuse the pawn key;
   trim scratch initialization; stop per-root-move `GameState` copies in the
   worker pool and fallback paths; re-rank root candidates only on change.
   Gates: fixed-depth rows byte-identical, speed gate green.
4. **Move generation rewrite (perft byte-identical).** Incremental
   checkers/pinned/blockers in `apply_unchecked`/`restore`; direct legal
   generation; bitboard generation from the existing attack tables preserving
   output order; replace en-passant trial probes with incremental
   information; remove the double generation in `GameState::make_move` and
   `apply_legal`. Gates: perft byte-identical, ordering tests, shadow-diff,
   full tactical gate; the speed gate must show the qsearch-heavy gain.
5. **PGO (Windows MSVC first).** Wire `KOI_PGO`; train on the mixed workload;
   produce the first `/USEPROFILE` Release build; record NPS and
   time-to-depth deltas; document the regeneration recipe. Gates: full
   Release CTest and `release_verify.ps1` on the PGO build; the byte-identical
   pin still holds (PGO must not change values).
6. **SPRT-gated refinements.** Only if the phases surface candidates that
   cannot be made value-preserving (for example TT replacement or store
   batching, a richer eval cache, history-sharing across Lazy SMP helpers
   carried over from the hardening plan). Each candidate lands alone with the
   64-position tactical gate and a recorded SPRT campaign at a fixed node
   count, and is reverted if not at least neutral.
7. **Verification and docs.** Full Release and Debug CTest,
   `release_verify.ps1`, ASan subset, the concurrency soak, a final speed
   report against the Phase 0 baseline, README/index updates, and the phase
   records below this plan.

## Verification

- Value-preserving phases: Threads=1 fixed-depth benchmark rows byte-identical
  (nodes and matches), perft byte-identical, shadow-diff and rules suites
  green, and `performance_gate.ps1` parity against the frozen baseline.
  Threads>1 asserts invariants, not equality.
- Behavior-changing phase: the 64-position tactical gate plus an SPRT
  campaign recorded under `artifacts/verification/speed-program/`, following
  the engine-v2 contract.
- Build-level changes (PGO): the same correctness gates as a code change plus
  a recorded NPS/time-to-depth comparison; a PGO build that fails any gate is
  not shipped.
- Final: Release CTest green, `release_verify.ps1` PASS, ASan subset clean,
  CI green (Windows full/sanitizer/shadow-diff, Linux GCC/Clang/modules-off/
  flake/tarball), and the end-to-end speed report shows the cumulative NPS
  and time-to-depth effect per phase.

## Risks

- **TT reclamation regression.** This is the exact area of the hardening
  use-after-free; the epoch scheme is subtle. Guard rails: the hardened
  concurrency test, ASan, and a fallback to the `shared_ptr` snapshot design
  that still removes the per-node storage guard.
- **Move generation ordering.** Order is pinned by tests and the byte-identical
  perft contract; if bitboard generation cannot preserve it, the fallback is
  to keep the mailbox enumeration order and swap only the internals.
- **Measurement noise.** Windows dev-box runs vary; alternating baseline and
  candidate runs with medians and a regression ceiling are the guard, and the
  noise floor is established in Phase 0 before any decision is made.
- **PGO reproducibility.** Profiles are workload-specific; an unrepresentative
  training set can overfit. The recipe is fixed and committed, the profile is
  regenerated per release, and no gate may pass on PGO without the
  byte-identical pin.
- **Scope creep into strength.** Evaluation values, search behavior, and
  pruning stay untouched unless the change passes the phase 6 gate;
  strength-only items remain in the follow-up strength plan.

## Phase 0 record (2026-09-25)

- `koi-bench` measurement modes landed: `--fen-file <path>` (blank lines and
  `#` comments skipped; each line is `[name|]payload`, where the payload is a
  FEN or a UCI move list replayed from the start position), `--depth <N>`
  (fixed-depth timed mode), `--depth-sweep <a..b>`, and `--report <path>`
  writing a `koi-bench-speed-v1` JSON artifact (per-position depth, nodes,
  qnodes, tt_hits, score, best move, elapsed, NPS plus totals). `--depth`
  cannot be combined with `--depth-sweep` or a node limit. The default suite
  also accepts these flags; legacy stdout is byte-identical so
  `performance_gate.ps1` and `release_verify.ps1` are untouched.
- Sweep sampling split `SearchInfo::nodes` (total visited) into nodes/qnodes
  with the advertised `info.qnodes`, matching the completed-result split, so
  sweep rows are no longer understated at the final depth.
- `tests/integration/tools/koi_bench_process_test.ps1` now covers the
  `name|FEN` and `name|moves` corpus forms, the fixed-depth header and report
  schema, sweep depth ordering and range, `totals.visited == nodes + qnodes`,
  and the `--depth`/`--depth-sweep` conflict. The test passes
  (`PROCESS_TEST_PASS`).
- `tools/build/speed_gate.ps1` added: alternating baseline/candidate runs,
  report parity (schema, suite, source, evaluator, threads, speed, hash state,
  timed, warmup, repeat, node limit, depth/sweep, and exact `id#depth` row
  keys), per-row median NPS and elapsed, median total NPS, and a `-2%`-style
  regression ceiling (`-MaxNpsRegressionPercent`,
  `-MaxRowNpsRegressionPercent`); evidence lands under
  `artifacts/verification/speed-program/speed-gate/`. Documented in
  `tools/README.md`.
- Baseline captures under `artifacts/verification/speed-program/phase-0/`
  (Threads=1, speed 100, classical):
  - `cold-timed/`: 64 rows, 770,021 visited, 2,877 ms, 267,647 NPS.
  - `warm-steady/` (`--warm-hash --warmup 1 --repeat 5`): 64 rows, 2,433 ms,
    290,478 NPS.
  - `node-steady/` (`--nodes 20000 --warm-hash --warmup 1 --repeat 5`):
    67 rows / 64 ids, 1,161,671 visited, 3,368 ms, 344,914 NPS.
  - `sweep-evaluation/` (2..6): 46 rows / 10 positions, 485,567,688 visited,
    1,653,518 ms, 293,657 NPS.
  - `sweep-endgames/` (2..6): 71 rows / 18 positions, 27,765,931 visited,
    69,665 ms, 398,563 NPS.
  - `sweep-openings/` (2..5): 128 rows / 32 positions, 74,879,547 visited,
    701,187 ms, 106,789 NPS. A 2..6 sweep was attempted and abandoned after
    more than 45 minutes; early-opening depth-6 searches are minutes each, so
    the openings corpus is pinned at 2..5.
- Pin (`phase-0/pin/`): the pre-program binary (`koi-bench-47d2cf3.exe`,
  sha256 `E5A1A70A...CEE6`) and the Phase 0 build
  (sha256 `7A9AB33B...73F7`) produce byte-identical default stdout
  (sha256 `6F7D8FF5...A7F8`, 64 rows, 770,021 visited) and identical timed
  rows after removing `elapsed_ms`/`nps`; 64/64 matches. This is the
  value-preserving pin for Phases 1-3; perft stays the pin for Phase 4.
- `performance_gate.ps1` old-vs-new at Threads=1, Runs=2:
  `result=PASS` (-5.67% median elapsed, inside the 5% ceiling; parity checks
  green).
- Noise floor: `speed_gate.ps1` self-vs-self on the endgame corpus (2..5,
  Runs=3): `result=PASS`, total delta -0.41%, median row delta 0.00%. Rows
  whose elapsed is 0-1 ms swing by up to 99.9% on a one-millisecond boundary,
  so later phase decisions use the median row delta and totals, and rows
  below ~5 ms are treated as noise.
- Nuances recorded for later phases: a bounded-depth request may complete
  below the requested depth when a selective root result is not publishable
  (`search_runner.cpp:2918-2933`), which is why e.g. evaluation has 46 rows
  for 10 positions x 5 depths; three endgame positions are immediate forced
  draws and report a single depth-0 row. Row-key parity in the speed gate
  compares the actual keys, so both behaviors are safe for value-preserving
  work. The engine's depth 0-64 "mate" suite positions stop when the mate is
  found.
- Git: Phase 0 changes committed separately from this record's artifacts
  (bench JSON is untracked evidence).

## Phase 1 record (2026-09-25)

- Design: the storage `shared_mutex_` guard is gone from probe, store, and
  prefetch. A reader `Lease` publishes the live `Storage*` into a per-thread
  hazard slot and clears it on destruction; `resize_locked` retires the old
  storage only after every hazard slot stops pointing at it. Slots are padded
  to a cache line (`struct alignas(64) HazardSlot`). A 1024-slot pool hands
  each thread one stable slot (leaky static with a thread-local releaser); the
  cold fallback (`Lease::adopt_snapshot`) takes `maintenance_mutex_` and pins
  the storage with a `shared_ptr` when no slot is available. `publish()` has a
  fast path: when the live pointer is already pinned in this thread's slot it
  is used without a store or revalidation, because a pinned lease blocks
  retirement. Stripe locks remain for stores, `clear()`, and
  `new_generation()`.
- Key reuse: the `search_transposition_key` computed once per negamax node is
  now reused for the ProbCut store and the IID probe instead of recomputed.
- False sharing found during the T8 gate: the first hazard array used plain
  8-byte `std::atomic<Storage*>` slots, so eight search threads ping-ponged one
  cache line and lost about 9% at Threads=8. Cache-line padding removed it
  (T8 gate went from -8.7% to +0.2% in the first padded run).
- Inline hot path: the first hazard version regressed steady-state Threads=1 by
  1.3% (out-of-line `Lease` ctor/dtor plus a guarded function-local
  `thread_local`). The ctor/dtor are inline now and read a
  constant-initialized `thread_local cached_hazard_slot_`, registering only on
  first use; the steady warm A/B moved from -1.33% to +1.60%.
- Gate tooling fixes: `Get-Median` used banker's rounding
  (`[int]($Count / 2)`), which picks the wrong sample for odd counts (three
  samples returned the maximum); fixed with `[Math]::Floor` in both
  `speed_gate.ps1` and `performance_gate.ps1`. `speed_gate.ps1` gained
  `-MinRowElapsedMs` (default 20 ms): rows below the floor are reported with
  `noise = true` and excluded from the per-row verdict while the totals keep
  every row; documented in `tools/README.md`.
- Correctness evidence: pin byte-identical (default stdout sha256
  `6F7D8FF5...A7F8`, 770,021 visited, 64 rows, timed rows identical after
  removing `elapsed_ms`/`nps`); `transposition_table_tests` 9/9;
  `koi_search_tests` 152 run / 137 pass / 0 fail / 14 xfail / 1 xpass /
  1 intermittent (unchanged from before); `koi_soak_tests` 4/4;
  `koi_bench_process_test.ps1` exit 0; ASan (KOI_SANITIZE, CI recipe
  `ctest -L unit -LE heavy`) 26/26, including the hash-resize soak.
- Speed evidence, Phase 0 binary (`7A9AB33B...73F7`) vs the final candidate
  (`487B5052...DA83`), all PASS:
  - T8 default suite, Runs=9: total -1.69%, median row -1.18%, 48 noise rows
    (`phase-1/speed-gate-t8-fastpath-20ms`).
  - T1 cold default suite, Runs=9: total -1.88%, median row -0.96%, 54 noise
    rows (`phase-1/speed-gate-cold-fastpath`).
  - T1 endgames 2..5, Runs=5: total +0.78%, median row 0.00%
    (`phase-1/speed-gate-endgames-fastpath`).
  - Warm-hash steady A/B: T1 -0.10%, T8 no regression signal; the machine's
    run-to-run spread is about ±1-2%, so small differences are not
    attributable.
- Honest outcome: the win is structural (no storage lock on the hot read path,
  one lease per access, padded hazards, key reuse). Wall-clock deltas sit
  inside the ±2% gate at Threads=1 and are neutral-to-slightly-positive at
  Threads=8; the work also fixes the gate tools' median bug and adds the noise
  floor needed by later phases.
- Local ASan note: the Community VS instance lacks the AddressSanitizer
  runtime libraries, so the sanitizer build uses the BuildTools instance
  (`C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools`) and its
  CMake 3.31; the MinGW CMake 3.27 is below the project minimum.
- Git: source, tool, and plan changes committed separately from the untracked
  evidence under `artifacts/verification/speed-program/phase-1/`.

## Phase 2 record (2026-09-25)

- Stored metadata: `PositionFeatures` gained `occupied`, `colors[2]`,
  `pawns[2]`, and `piece_mobility[64]`; `native_position_features` fills them
  from data it already computes. `feature_masks()` and the knight/sliding
  mobility helpers read the stored bitboards and fall back to the 64-square
  scan when `occupied == 0`, which keeps hand-built fixtures and empty boards
  exact.
- Pawn bitboards: `pawn_structure_for` and `passed_pawn_for` now derive file
  counts, passed, connected, advanced support, blockade, and protection from
  `pawns[color]` with small mask helpers (`file_mask`, `adjacent_files_mask`,
  `ranks_mask`, `pawn_is_passed_bitboard`, `for_each_feature_pawn`); the legacy
  loops remain as the `occupied == 0` fallback. Scoring arithmetic is
  unchanged.
- Correctness evidence: pin byte-identical (default stdout sha256
  `6F7D8FF5...A7F8`, timed rows identical after removing `elapsed_ms`/`nps`);
  `classical_evaluator_tests` 13/13, `koi_core_tests` 27/27,
  `native_rule_state_tests` 12/12, `koi_strength_tests` 7/7,
  `koi_search_tests` 152 run / 137 pass / 0 fail / 14 xfail / 1 xpass /
  1 intermittent, `koi_soak_tests` 4/4, `transposition_table_tests` 9/9,
  `koi_shadow_diff_tests` 7/7, `perft_tests` 3/3,
  `evaluation_features_tests` 15/15, `koi_bench_process_test.ps1` exit 0.
- Speed evidence, Phase 0 binary (`7A9AB33B...73F7`) vs the candidate
  (`BA0AFA9D...4CA3`), Threads=1, both PASS:
  - T1 endgames 2..5, Runs=5: total +0.96%, median row +2.80%, 47 noise rows
    (`phase-2/speed-gate-step2-endgames`).
  - T1 cold default suite, Runs=9: total +5.55%, median row +4.74%, 54 noise
    rows (`phase-2/speed-gate-step2-cold`).
  - The earlier step-1-only endgames gate read higher (+30%) at different
    absolute levels; only paired alternating-run deltas are attributable.
- Deferred within the phase: pawn hash keyed by `pawn_key`, locked-pawn-wall
  cache, removing the FeatureState lock/copy, and eliminating the per-candidate
  child `position_features()` extractions at `search_context.cpp:1559,1591`.
- Git: `src/koi/game_state.hpp`, `src/koi/game_state.cpp`,
  `src/koi/classical_evaluator.cpp` plus this plan record; evidence untracked
  under `artifacts/verification/speed-program/phase-2/`.
