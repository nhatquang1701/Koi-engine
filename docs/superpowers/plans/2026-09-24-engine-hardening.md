# Engine hardening and feature foundations

Status: in progress. Owner: Koi Engine.

## Goal

Fix the crash, hang, correctness, and infrastructure weaknesses found in the
2026-09-24 four-part audit, then build the new features the fixes unlock:
many-thread and long-game stability, the UCI/GUI protocol surface, and
strength work (trained NNUE v5, evaluation tuning, search modernization).

Scope decisions (confirmed):

- Sequencing: stability first, then robustness/scale, then protocol/GUI, then
  strength.
- Feature targets: (1) many-thread and long-game stability, (2) `go mate`,
  `go perft`, `lowerbound`/`upperbound`, ponder, `UCI_LimitStrength`/`UCI_Elo`,
  (3) trained NNUE v5 and evaluation tuning.
- The uncommitted Lazy SMP helper-depth change in
  `src/koi/detail/search_runner.cpp` is fixed properly (per-iteration depth
  read, deterministic stagger, benchmark evidence) rather than reverted.
- Quality gates: a Windows AddressSanitizer CI job and a Linux no-retry flake
  CI job. The sanitizer job is Windows-only (MSVC ASan); the flake job runs on
  Linux CI.
- The UCI handshake stays byte-identical unless a change is explicitly
  intended and the handshake fixture is updated with it.
- The classical evaluator stays the default; NNUE stays opt-in with a CPU
  fallback. Elo/CPL/NPS remain reports, never CI thresholds.

## Weakness inventory

From the 2026-09-24 audit. `file:line` references are to the audit revision.

### Stability and undefined behavior

1. TT retired storages are erased after a two-slot window while lock-free
   readers may still hold the raw `hot_storage()` pointer
   (`transposition_table.cpp:299-307,415-425,504-515`); the CI-segfaulting
   shard runs exactly this concurrency test.
2. Lazy SMP helper threads have no exception guard, so any throw terminates
   the process (`search_runner.cpp:1940-1965`).
3. Worst-case search-stack usage (~1.2-1.5 MB) exceeds the 1 MB default
   thread stack: per-frame `MoveMetadataList` buffers in negamax and
   quiescence (`search_context.cpp:666,1127,1282,182,244`,
   `search_stack.hpp:16`).
4. `std::quick_exit(74)` on protocol input: an empty or fully illegal
   `searchmoves` list and any native/shadow completion disagreement
   (`completion_gate.cpp:13-20,101-131`,
   `uci_controller.cpp:1258-1262,1531-1534`).
5. `SearchSession::stop()` stores and notifies without holding the wait
   mutex (lost wakeup -> hang) (`search_session.cpp:41-65`).
6. A node-limited ponder publishes `bestmove` early and `ponderhit` can then
   publish a second one (`search_runner.cpp:4587-4592`,
   `uci_controller.cpp:1067-1097`).
7. `TimeManager::reconfigure` mutates non-atomic state while helpers call
   `should_stop` (`time_manager.cpp:109-128`, `search_runner.cpp:1955`).
8. NNUE loading allocates by file size with no cap and `bad_alloc` escapes
   the UCI loop and `main`; thread-creation failure also escapes
   (`nnue.cpp:1440-1455`, `uci_controller.cpp:983-994`,
   `search_session.cpp:23-38`).
9. Latent races: `FeatureState` fast path (`feature_state.cpp:55-61`), the
   process-global GPU threaded flag, GPU batch starvation and device frees
   without a current context (`nnue_gpu_evaluator.cpp:60-152`,
   `cuda_driver.cpp:324-328`).

### Correctness

10. Native history is capped at 256 plies and a long `position ... moves`
    command is dropped wholesale, leaving a stale root
    (`position.cpp:28,743-772`, `uci_controller.cpp:706-712`).
11. Native and shadow repetition identity diverge for an illegal-en-passant
    double push (`position.cpp:1299-1302` vs `chess.hpp:2109-2154`).
12. The evaluation cache key omits `fullmove_number`, which the classical
    evaluator reads (`search_context.hpp:443-456`,
    `classical_evaluator.cpp:151,239`).
13. Quiescence TT entries are horizon-blind (qdepth/check-limit are not in
    the shared key) (`search_context.cpp:207-218,296-299`).
14. Parallel-root abort can publish the unordered generated move
    (`search_runner.cpp:2418-2422` vs `:3753-3761`).
15. True IID cannot trigger (`!tt_move` never holds at depth >= 6) and marks
    the node selective when it does (`search_context.cpp:1242-1257`).
16. Syzygy: `SyzygyProbeLimit` defaults to 5 in code but is advertised as 7;
    `SyzygyPath` resolves against the CWD; the root probe ignores
    `SyzygyProbeDepth` for node/time searches (`uci_controller.hpp:120`,
    `uci_controller.cpp:210-211,935-939`, `search_runner.cpp:2475`).
17. Hygiene: FENs with the side not to move in check are accepted,
    `legal_moves_into` silently truncates for small spans, and
    `modules/types.ixx` drifts from the implementation.

### Advertised but inert, or incomplete

18. `UCI_LimitStrength`, `UCI_Elo`, and `StrengthMode` have no effect (the
    only consumer is a hook nothing installs) (`search_service.cpp:89-92`,
    `search_types.hpp:253-261`).
19. No `go mate`, no `go perft`, no lowerbound/upperbound info; the ponder
    move is suppressed unless the `Ponder` option is set; any valid
    `setoption` aborts an active search; a `position` parse failure latches
    `ShuttingDown`.
20. Search modernization backlog: quiescence TT cutoffs, late-move pruning
    tables (the live path computes LMR twice; the legacy `late_move` is
    dead), MultiPV aspiration disabled by design, shared histories across
    Lazy SMP helpers deferred, NPS under-reported, `root_result_publishable`
    dead code.
21. Evaluation: the tuner's "generated" header is a hand-written stub and its
    adoption path is not wired; no pawn hash; mobility double-counts
    bishops/queens and omits rooks/knights; king safety has no attack tables.
22. NNUE v5 has no representative trained network and no holdout discipline;
    the exporter's validation format does not match the label corpus; the GPU
    service only accepts hidden 1536, so no committed v5 network can load, and
    the kernel is structurally slow.
23. Opening book safety is a shallow material-only probe and a rejection
    discards the book move instead of trying the next candidate.

### Tests, CI, and process

24. No sanitizer job, no no-retry flake job, no coverage, no performance
    gate, and no SPRT gate in CI.
25. `XFAIL-UNSEEN` is only enforced on unfiltered, unsharded runs, and CI
    always runs four shards (`koi_test_support.hpp:450-461`).
26. The `uci_controller_tests` deterministic-search case races `go depth 2`
    against an immediate `stop` (`uci_controller_tests.cpp:1277-1296`).
27. The uncommitted Lazy SMP helper-depth change is ineffective (helpers read
    `main_depth_` once before the loop) and adds scheduling-dependent start
    depths (`search_runner.cpp:1931-1952,2653-2655`).
28. Plan/document drift: the Linux plan is missing from the plans index and
    several plan checkboxes are stale.

## Design decisions

- **TT reclamation**: readers take a snapshot of the current storage under
  the maintenance mutex (or an epoch/hazard scheme); retired storages are
  freed only when no reader can hold them. The concurrency test is
  strengthened to hammer resizes under sanitizers.
- **Search stack**: move the large per-node containers into per-context
  scratch storage indexed by ply where possible; otherwise create search
  threads with an explicit larger stack. The worst case is covered by a
  depth-64 plus maximum-quiescence stress case.
- **Protocol safety**: protocol input must never kill the process. An
  unsatisfiable `searchmoves` restriction is treated as unrestricted (or
  answered with `bestmove 0000`), and a completion-gate disagreement logs
  diagnostics and publishes a native-legal fallback.
- **Ponder lifecycle**: every ponder root waits for `stop`/`ponderhit`; a
  request that already published cannot publish twice.
- **Timing snapshot**: helper threads read an immutable atomic timing
  snapshot; `reconfigure` swaps the snapshot rather than mutating fields.
- **XFAIL policy**: `XFAIL-UNSEEN` becomes shard-aware and fatal, so removing
  a known-failure entry fails CI even through the four shards; the
  `tests/README.md` inventory must match the code lists.
- **Sanitizer build**: a `KOI_SANITIZE` cache option (MSVC `/fsanitize=address`
  with a dynamic CRT) and a Windows CI job; Linux keeps its no-retry flake
  job. TSan is out of scope on MSVC, so thread races are covered by ASan plus
  the hardened concurrency tests.
- **Flake job**: runs the search shards and the controller tests with
  `KOI_TEST_RETRIES=1`, no `--repeat until-pass`, repeated several times, so
  first-attempt failures are visible.
- **Evidence**: search behaviour changes keep the engine-v2 contract - the
  64-position tactical gate, perft, shadow-diff, the full suites, and a
  recorded SPRT/NPS report under `artifacts/verification/engine-hardening/`.

## Phases

0. **Safety net and baselines**: fix the flaky controller test; enforce
   `XFAIL-UNSEEN` on shards; add the Windows ASan job (`KOI_SANITIZE`) and
   the Linux flake job; fix the Lazy SMP helper-depth change; capture the
   pre-change baseline (`release_verify.ps1`, `koi-bench --timed` at
   Threads 1/2/4/8, SPRT self-check).
1. **Stability**: TT reclamation, helper exception guard, stack headroom,
   removal of the `quick_exit` protocol paths, session/ponder lifecycle,
   timing snapshot, load/thread-creation containment, and the latent races.
2. **Correctness**: long-game history, shadow repetition identity, evaluation
   cache key, quiescence TT tagging, root abort ordering, IID provenance,
   Syzygy defaults/paths/depth, and the hygiene items.
3. **Robustness and scale**: a soak suite (long games, Threads 8/16, hash
   resize during search, ponder cycles) under sanitizers; shared histories
   across Lazy SMP helpers; honest NPS accounting and a report-only NPS
   artifact; hash-cap and fixed-depth thread-shape decisions.
4. **Protocol and GUI**: `go mate`, `go perft`, bound flags, ponder-move
   emission, `setoption`/`position` state-machine fixes, and a calibrated
   `UCI_LimitStrength`/`UCI_Elo` profile measured with the Elo harness (or
   de-advertised if it cannot be evidenced).
5. **Strength**: true IID redesign, late-move pruning tables, quiescence TT
   cutoffs, MultiPV per-line aspiration, the evaluation tuning/adoption
   workflow (pawn hash, mobility, king-safety tables), a representative
   NNUE v5 with holdout discipline plus GPU parity, and the root-forcing
   XFAIL burndown.
6. **Verification and docs**: full Release and Debug suites, the release
   gate, the sanitizer and flake jobs green, evidence records, a verification
   record, and the README/index refresh.

## Phase 0 record (2026-09-24)

- Flaky `uci_controller_tests` deterministic-search case: the transcript now
  releases one stage per completed `bestmove`
  (`StagedGatedInputBuffer` / `ReleaseOnBestmoveCountBuffer`), so `stop` can no
  longer race a completed search. 20/20 focused repeats and the full binary
  (52 cases) pass.
- `XFAIL-UNSEEN` is now enforced on filtered and sharded runs: an entry that
  names no case, or a selected case that did not run, fails the run. Verified
  with a temporary sentinel entry (fatal on unfiltered, sharded, and filtered
  invocations) and on all four search shards.
- Windows ASan job: `KOI_SANITIZE` (MSVC only) switches to the dynamic CRT,
  strips `/RTC1`, disables LTO, and adds `/fsanitize=address`. Local
  Debug+ASan tree: 375/375 targets built, `ctest -L unit -LE heavy` 25/25
  passed. The new `sanitizer` CI job runs the same subset.
- Linux no-retry flake job: `linux-flake` rebuilds with GCC 14 and repeats
  `koi_search_tests_[1-4]of4`, `transposition_table_tests`,
  `uci_controller_tests`, `search_service_tests`, and `time_manager_tests`
  three times with `KOI_TEST_RETRIES=1` and no `--repeat`.
- Lazy SMP helper-depth change fixed: helpers re-read the published depth at
  the top of every iteration, keep their own progress with `std::max`, and the
  starting class stays index-based (`helper_index % 3`) so it cannot depend on
  thread scheduling. Threads=1 never starts a pool and is byte-identical.
- Baseline evidence in `artifacts/verification/engine-hardening/`:
  `bench-threads{1,2,4,8}.json` plus the timed runs, `baseline.txt`
  (Threads=1 repeat identical, 64/64 matches at every thread count),
  `release-verify/` (Debug+Release CTest PASS, smoke 31 lines / 1 bestmove /
  0 stderr, replay rule draw, En Croissant-style match clean), and
  `sprt-self-check/` (32 games, 6W/20D/6L, Elo 0, LLR -0.009, inconclusive -
  identical binaries must not show an effect).

## Phase 1 record (2026-09-24)

- Transposition table use-after-free removed: readers hold a shared
  `storage_mutex_` for the whole store/probe/prefetch and a resize takes it
  exclusively before replacing (and dropping) the old storage, replacing the
  two-entry retired-storage list.
- Lazy SMP helpers can no longer terminate the process: the helper body is
  wrapped in a catch-all that records the first failure, aborts the other
  helpers, and marks `SearchResult::failed`.
- `std::quick_exit(74)` is gone. The completion quarantine path and the book
  quarantine path now answer `bestmove 0000` with an explanatory `info string`
  and keep the process alive.
- Ponder searches always wait for `stop`/`ponderhit` before publishing, so a
  node-limited ponder no longer emits an early bestmove (and `ponderhit` can no
  longer publish a second one). The UCI process test pins this.
- `SearchSession::stop()` now stores the flag and notifies under
  `stop_mutex_`, closing the lost-wakeup window.
- `TimeManager` keeps the hot-path timing state in atomics (budget in
  milliseconds, node limit, elapsed baseline) so helper `should_stop` reads
  cannot race `reconfigure` on `ponderhit`.
- Containment: NNUE containers are capped at 256 MiB with an allocation catch,
  `EvalFile` installation and startup evaluator selection are guarded, and a
  failed `SearchService::start` answers a legal fallback instead of
  terminating the process.
- Latent races closed: the `FeatureState` fast path takes a shared lock; the
  GPU threaded gate is a counter held by a per-search RAII scope; CUDA release
  re-establishes the context before freeing device memory.
- Verification: Release CTest 66/66; the four search shards pass;
  `release_verify.ps1` PASS (`artifacts/verification/engine-hardening/release-verify-phase1/`,
  smoke 31 lines / 1 bestmove / 0 stderr, 64/64 rows at Threads 1/2/4 plus the
  timed and optional runs); local Debug+ASan tree passes `ctest -L unit -LE heavy`
  (25/25) and runs `koi_search_tests --shard=2/4` twice with no sanitizer report.

## Phase 2 record (2026-09-24)

- Syzygy: the controller default `SyzygyProbeLimit` is 7 (matching the
  advertised option and the handshake fixture) and the module contract agrees;
  a single relative `SyzygyPath` resolves against the executable directory
  (path lists are left alone); the root probe treats a time/node search as
  unbounded instead of depth 1, so `SyzygyProbeDepth` applies.
- The evaluation cache key mixes in `fullmove_number`, which the classical
  evaluator reads for its early-queen and king-ring thresholds.
- An interrupted parallel root now answers with the best-ordered root move
  (the generated head was previously published when no safe partial line
  existed).
- Long replays are no longer rejected: the native snapshot ring drops its
  oldest entry instead of refusing a move, and the compatibility mirror keeps
  the same 256-ply window. Because the shadow stores the halfmove clock in a
  byte (255 wraps to 0) while the native clamps at 255, the mirror comparison
  and `PositionConsistencySnapshot::consistent()` tolerate a saturated native
  clock; a real game ends at the 75-move rule long before this.
- Repetition identity is shared with the shadow: when a double push leaves an
  en-passant square that no legal capture can use, the mirror hashes the
  position without it (the native key already does), so the two repetition
  counts agree.
- Quiescence table entries are tagged with a negative depth
  (`-1 - qdepth`), so a shallow frontier can no longer reuse a value produced
  under a different horizon; regular entries stay usable anywhere.
- True IID no longer taints provenance: the probe's selective flag and the
  child slot's cutoff count are restored after the (discarded) probe.
- Hygiene: `legal_moves_into` reports the true legal count when the span is
  small, the module `PackedMove` no-move value and `move_overhead_ms` match the
  engine, and non-moving-side-in-check FENs stay accepted (the pinned tactical
  fixtures rely on them; Stockfish accepts them too).
- Verification: focused suites 11/11, full Release CTest 66/66, and the local
  Debug+ASan tree passes `native_rule_state_tests`, `koi_rules_tests`,
  `perft_tests`, `transposition_table_tests`, plus `koi_search_tests
  --shard=2/4`, with no sanitizer report.

## Phase 3 record (2026-09-24)

- Stability soak suite: new `koi_soak_tests` (labels `unit;runtime`, 120 s
  budget, about 4 s actual) with four bounded cases: a 322-ply replay that
  crosses the 256-ply snapshot window and then searches it at Threads=8, a
  12-step hash resize (1 <-> 4 MiB) under a live four-worker search, four
  ponder cycles that must publish exactly one answer after `stop`, and a
  thread-count sweep (1/2/4/8/16) that must keep every result legal and
  unfailed. Repeated runs are clean, and the suite joins the sanitizer and
  no-retry flake jobs.
- Honest NPS accounting: helpers publish the counters of each finished
  iteration into monotonic live totals (`live_nodes`, `live_qnodes`,
  `live_tt_hits`, `live_tbhits`, and a max `seldepth`), and both `info`
  writers (serial and root-parallel) report main plus live helper traffic. The
  report-only NPS artifact is
  `artifacts/verification/engine-hardening/nps-report.txt`: the 64-position
  timed gate matches 64/64 at Threads 1/2/4/8 with aggregate
  (nodes+qnodes)/elapsed = 227k / 418k / 691k / 905k nps, and the live info
  probe on startpos (`go movetime 700`) reports 93k / 206k / 238k nps at
  Threads 1/4/8.
- Decisions recorded (no code change):
  - **Hash cap stays 4096 MB.** The engine already clamps to the physical and
    commit limits from the memory snapshot and to the segmented-allocator
    policy; no measured workload is limited by the cap, and raising it would
    change the memory contract for GUI hosts without evidence.
  - **Fixed-depth thread shape stays as it is.** Depth-limited searches use
    shared root PVS below four workers and root splitting at four or more; the
    documented determinism contract is Threads=1 only (threaded runs assert
    legality and coverage invariants), so the shape difference between
    Threads=2 and Threads=4 is accepted rather than papered over.
  - **Shared Lazy SMP histories deferred to Phase 5.** Moving the ordering
    tables across helpers is a search-behaviour change that this plan's own
    contract requires recorded strength evidence for. The SPRT harness is part
    of the Phase 5 strength work, so the experiment is scheduled there instead
    of landing an unvalidated change now. Helpers keep contributing through
    the shared transposition table, the depth publication, and the honest NPS
    accounting added here.

## Phase 4 record (2026-09-24)

- **`go mate N`** maps to the depth that can prove or refute a mate in `N`
  (`2N - 1` plies) unless an explicit depth was supplied. The engine answers
  with an ordinary search and one `bestmove`.
- **`go perft N`** is a debugging command: it copies the root, walks every legal
  move, prints `info string <uci>: <nodes>` per move plus
  `info string Nodes searched: <total>`, and emits no `bestmove`. `N` is
  accepted in `1..10`.
- **Bound flags**: `SearchInfo` carries `Bound { exact, lower, upper }` and every
  publication made without full-window root authority (non-authoritative serial
  iteration, non-authoritative MultiPV rank, depth-0 emergency report) now sets
  `lowerbound`, which `write_search_info` prints after the score.
- **Ponder reply**: the final `bestmove` prints ` ponder <reply>` whenever the
  completed PV has a reply; the `Ponder` option only announces that the GUI will
  ponder, so GUIs that never set it still receive the reply move.
- **Same-value `setoption`**: `apply_boolean`/`apply_unsigned` compare the parsed
  value with the current one and skip the stop/commit path when nothing changes,
  so a GUI re-applying its settings between games cannot cancel a live search.
- **`position` failures no longer latch the controller**: the pre-parse
  `state_ = ShuttingDown` assignment was removed; suppression now goes through
  `stop_and_suppress_active_search()` alone, which returns the controller to
  `Idle`.
- **Strength limiter**: `UCI_LimitStrength` + `UCI_Elo` now actually limit clock
  searches through a monotone node cap (100 nodes at 500 Elo, doubling every
  250 Elo, 25 000 cap, unlimited at 2600+). Explicit depth/nodes/movetime/
  infinite/ponder limits are honoured exactly as sent, so analysis and pinned
  tests never change. The mapping is uncalibrated and documented as such in the
  README.
- **Tests**: `uci_controller_tests` gained five cases (go perft output, go mate
  mapping, limiter node cap versus explicit depth, same-value option
  reapplication, ponder reply without the option) and all `.substr(9)` move
  extractions were replaced with a tokenizer (`first_bestmove_move`). The shared
  PowerShell validators were updated: `Test-SearchInfo` accepts the optional
  bound token, and the bestmove regexes in `uci_process_test.ps1` and
  `en_croissant_uci_test.ps1` accept the optional ponder suffix while keeping
  the capture group on the played move.

## Phase 5 record (2026-09-25)

- **Late-move pruning tables**: the pre-make gate and the post-make reduction
  were two independent copies of the same rules and the live path evaluated
  `dynamic_late_move` twice per candidate, discarding the first reduction. The
  rules now live in `SearchPolicy::late_move_gate`, which both sites call
  (`dynamic_late_move` delegates), so the gate cannot drift from the reduction.
  The production-dead `SearchPolicy::late_move` was deleted and
  `search_policy_tests` was rewritten against the gate plus the dynamic
  reduction. Behaviour is unchanged and only the discarded work is gone.
- **True internal iterative deepening**: the probe previously required
  `!tt_move`, which no depth-6 PV node can satisfy once shallower iterations
  have seeded the table, so the mechanism never ran and the suite carried it as
  a `known_failures` entry. The trigger is now an untrustworthy ordering source
  - no table move, an upper-bound entry, or an entry searched at less than half
  the current depth - and the probe searches `max(1, depth - 4)` (a depth-2
  re-search would repeat almost the whole subtree). The probe restores the
  frame, the child cutoff count, and the provenance flag, then re-probes so the
  seeded move orders the real search.
- The `"true internal iterative deepening"` known failure was removed
  (13 -> 12 entries). `test_search_records_true_internal_iterative_deepening`
  searches depth seven: depth six never reaches a non-root PV node that
  qualifies, and depth seven is the shallowest configuration that observes the
  counter (40 s). A depth-eight startpos search costs about 613 s in this
  engine, so it is not a test budget.
- **Quiescence TT cutoffs: satisfied without further change.** Phase 2 tagged
  qsearch entries with the negative horizon (`-1 - qdepth`); the acceptance
  path takes an exact entry only when the horizon matches and a lower-bound
  cutoff only on a null window, and no fail-low frontier is ever stored as
  exact (stand-pat fail-highs and proven non-selective fail-highs store the
  lower bound, raised-alpha lines store exact). The pruning-sensitive frontier
  therefore cannot be reused under a different horizon.
- **Evidence.** Focused suites: the four search shards, `search_policy_tests`,
  and `koi_strength_tests` (64-position tactical gate, 3.0 s) all pass; full
  Release CTest 67/67. Benchmarks against a worktree build of the previous
  commit (`koi-bench --threads N --speed 100 --timed`): nodes 770 021/770 021
  (T1), 1 536 845/1 538 730 (T2), 3 039 852/3 061 431 (T4), 6 253 577/6 257 495
  (T8) and NPS within +/-1.1% at every thread count, so the IID change costs
  no measurable search time. The recorded SPRT (128 games at 20 000 nodes,
  candidate versus the previous commit) reports 24W/72D/32L, score 0.4688,
  Elo -24, LLR -0.26, decision **inconclusive** - a point estimate inside one
  standard error, i.e. no measurable strength change. Results and the NPS
  report are under `artifacts/verification/engine-hardening/`.
- **Deferred with reasons** (the plan's Risks section already splits these):
  MultiPV per-line aspiration, the evaluation program (pawn hash, the
  union-versus-per-piece mobility double count, king-safety attack tables,
  tuner adoption workflow), shared Lazy SMP histories, a representative NNUE
  version 5 network (needs a training campaign and corpus decision plus GPU
  parity), and the remaining ten root-selection `known_failures`. Each is a
  search or evaluation behaviour change whose acceptance contract is a long
  recorded SPRT campaign, which does not fit this hardening pass; the deferred
  list is the input to the follow-up strength plan.

## Verification

- Every phase: full Release CTest, focused suites for the touched areas, and
  a commit with an imperative capitalized message.
- Stability and correctness phases: sanitizer job green; hardened
  concurrency tests; no `quick_exit` on protocol input (process tests).
- Feature phases: the 64-position tactical gate, perft byte-identical,
  shadow-diff unchanged, plus a recorded SPRT/NPS report for behaviour
  changes. Threads=1 stays deterministic; Threads>1 asserts invariants.
- Final: Windows full CTest and `release_verify.ps1`; Linux CI green
  (GCC, Clang, modules-off, tarball, flake); Windows ASan job green.

## Risks

- TT reclamation is subtle; the hardened concurrency test and ASan are the
  guard rails, and the fallback is a shared-pointer snapshot per access.
- The stack fix touches hot-path layout and can move NPS; every phase records
  a benchmark delta.
- `UCI_Elo` calibration needs a large game budget and a usable anchor
  manifest; if the evidence cannot be produced, the options are de-advertised
  instead of shipping an uncalibrated limiter.
- NNUE v5 training needs compute and a corpus decision; the feature stays
  opt-in and evidence-gated.
- The root-forcing XFAIL cluster is one mechanism with many symptoms; it is
  the largest single strength item and may be split into its own plan if the
  evidence shows it needs to be.
