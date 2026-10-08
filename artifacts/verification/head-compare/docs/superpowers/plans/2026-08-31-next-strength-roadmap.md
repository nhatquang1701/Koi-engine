# Koi Engine Next Strength Roadmap Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Improve Koi's measurable classical strength, response time, and full Lucas Chess UCI compatibility while preserving deterministic single-thread behavior.

**Architecture:** Keep `chess-library` private to `GameState`; expose all search, evaluation, profiling, and replay behavior through Koi-owned types. Implement compatibility and measurement first, optimize the single-thread reference second, tune strength third, and only then change the root-worker implementation.

**Tech Stack:** C++26-capable x64 MSVC, CMake 3.31+, Ninja, vendored Disservin chess-library, standard-library threads/atomics/chrono/filesystem, PowerShell process tests.

**Spec:** `docs/superpowers/specs/2026-08-31-next-strength-roadmap-design.md`

## Global Constraints

- Windows x64 is the release target; use the existing CMake architecture checks.
- C++26 is required; keep extensions disabled and portable Release optimization without CPU-specific instructions.
- Standard chess only; preserve legal castling, en passant, promotion, checkmate, stalemate, FEN, repetition, and fifty-move behavior.
- No public Koi header may expose `chess.hpp` types.
- Stdout remains protocol-clean; only the controller writes UCI output.
- `Threads=1` remains deterministic and is the reference path; `Speed=100` preserves current timing behavior.
- Use only Terra or Luna subagents; never use 5.6 Sol.
- Every production change has a failing behavior test first, followed by focused and full verification.

### Task 1: Timing, UCI limits, ponder, and benchmark profiling

**Files:**
- Modify: `src/koi/search_types.hpp`, `src/koi/time_manager.cpp`, `src/koi/time_manager.hpp`, `src/koi/uci_controller.cpp`, `src/koi/uci_controller.hpp`, `tools/koi_bench.cpp`
- Test: `tests/koi_search_tests.cpp`, `tests/uci_controller_tests.cpp`, `tests/uci_process_test.ps1`, `tests/koi_bench_process_test.ps1`

**Interfaces:**
- Add `std::optional<Move> ponder_move` to `SearchResult`.
- Keep `SearchLimits` public fields unchanged; the controller represents bare `go` by setting `movetime` to 250 ms.
- Make `TimeManager::node_limit()` honor `limits.nodes` even when `infinite` or `ponder` is true.
- Add `koi-bench --profile-json <path>` and `--warm-hash`; preserve current default text output.

- [ ] Write failing tests for bare-go timing, infinite-plus-nodes, independent clocks, ponder output/hit, profile JSON, and deterministic default benchmark output.
- [ ] Run the focused tests and observe the expected failures.
- [ ] Implement the minimum controller/time-manager/result changes, including legal second-PV ponder selection and stale-search suppression.
- [ ] Implement deterministic profile serialization and warm/cold TT labeling without timestamps unless `--timed` is requested.
- [ ] Run focused tests, then the full Release suite; commit `feat: harden timing and uci diagnostics`.

### Task 2: Lucas workflow coverage and v2 replay reports

**Files:**
- Modify: `tools/uci_match.ps1`, `tests/uci_match_process_test.ps1`, `README.md`, `CMakeLists.txt`
- Create: `tools/koi_replay.cpp`, `tests/koi_replay_tests.cpp`

**Interfaces:**
- `koi-replay` accepts an initial `startpos` or six-field FEN followed by coordinate moves and reports legal replay plus terminal result using `GameState`.
- `uci_match.ps1` writes `schema = "koi-uci-match-v2"` with actual per-ply `root_fen`, legality, result, winner, termination, and process status while continuing to write PGN.

- [ ] Add failing replay and report assertions for illegal moves, checkmate, stalemate, actual per-ply FENs, and v2 metadata.
- [ ] Run the focused process tests and observe the expected failures.
- [ ] Implement the replay executable and wire it into CMake.
- [ ] Update the harness to validate both engines' moves before appending them and to classify terminal/max-ply/timeout/process-exit results.
- [ ] Document Lucas play, analysis, tutor, and report commands; run Release process tests and commit `feat: add replay-validated match reports`.

### Task 3: Single-thread hot-path optimization

**Files:**
- Modify: `src/koi/static_exchange.cpp`, `src/koi/static_exchange.hpp`, `src/koi/game_state.cpp`, `src/koi/game_state.hpp`, `src/koi/search_service.cpp`, `src/koi/classical_evaluator.cpp`, `src/koi/transposition_table.cpp`, `src/koi/transposition_table.hpp`
- Test: `tests/static_exchange_tests.cpp`, `tests/koi_search_tests.cpp`, `tests/koi_core_tests.cpp`

**Interfaces:**
- Preserve existing public Koi types and behavior.
- Keep all new data in private search/evaluation helpers; do not expose native chess-library types.
- Preserve exact single-thread scores/moves for the existing tactical cases.

- [ ] Add regression tests for SEE exchange outcomes, qsearch checking continuations, TT concurrent access, and fixed-depth baseline results.
- [ ] Run focused tests and observe at least one failure for each newly required optimization behavior.
- [ ] Replace SEE's repeated full legal-move copies with direct occupancy/attacker exchange analysis.
- [ ] Split qsearch tactical generation so quiet moves are not scanned after the checking horizon and captures/promotions/evasions remain complete.
- [ ] Make feature/attack extraction single-pass and add only measured pawn/king-safety caching.
- [ ] Replace per-probe exclusive TT locking with safe shared/striped access while keeping resize/clear serialized.
- [ ] Run the benchmark in cold/warm modes, compare counters, run all Release tests, and commit `perf: optimize single-thread search hot paths`.

### Task 4: Tactical suite and classical strength tuning

**Files:**
- Modify: `src/koi/strength_suite.cpp`, `src/koi/strength_suite.hpp`, `src/koi/search_ordering.cpp`, `src/koi/search_service.cpp`, `src/koi/classical_evaluator.cpp`, `README.md`
- Test: `tests/koi_strength_tests.cpp`, `tests/koi_search_tests.cpp`, `tests/search_ordering_tests.cpp`

**Interfaces:**
- Strength cases carry an ID, FEN, fixed depth, accepted moves, and optional score/category expectations.
- Diagnostics use existing `SearchInfo`/`SearchStats`; normal UCI output remains valid.

- [ ] Add failing tests for the expanded tactical categories, evaluator symmetry/material/endgame behavior, and no-regression solve counts.
- [ ] Run the strength tests and record the expected failures before adding cases or tuning.
- [ ] Add the 64-position hard gate and 128-position opt-in corpus with explicit multi-solution allowlists.
- [ ] Tune check/threat extensions, aspiration/PVS, conservative LMR/null-move conditions, move ordering, tapered evaluation, king safety, pawn structure, and endgame scaling one change at a time.
- [ ] Run the fixed-depth suite after each tuning group, require no hard-gate regression, run the full Release suite, and commit `feat: expand tactical strength regression suite`.

### Task 5: Final deterministic multithreading pass

**Files:**
- Modify: `src/koi/search_service.cpp`, `src/koi/search_types.hpp`, `src/koi/uci_controller.cpp`, `tools/koi_bench.cpp`
- Test: `tests/koi_search_tests.cpp`, `tests/uci_controller_tests.cpp`, `tests/koi_bench_process_test.ps1`

**Interfaces:**
- Keep `SearchOptions::threads` and `SearchOptions::speed_percent` unchanged.
- Root workers share cancellation, one node budget, and one time origin; inner workers never write UCI output.
- `Threads=1` follows the reference path; `Threads>1` uses stable root order and earliest-move tie-breaking.

- [ ] Add failing tests for fixed-depth Threads 1/2 equality, global node limits, prompt threaded cancellation, one completion callback, and benchmark thread configuration.
- [ ] Run the focused tests and observe the expected failures where the current root pool is nondeterministic or slower.
- [ ] Improve root scheduling by searching the stable PV/root prefix first, distributing remaining roots dynamically, reducing shared lock contention, and aggregating worker stats once per iteration.
- [ ] Ensure partial iterations never replace the last completed result and all worker joins occur before option/position mutation.
- [ ] Run deterministic and timed benchmarks, all Debug/Release tests, UCI process tests, and commit `perf: improve deterministic root parallel search`.

### Task 6: Whole-branch verification and release documentation

**Files:**
- Modify: `README.md`, `CMakeLists.txt`, `.github/*` only if required by existing CI checks
- Test: all CTest targets, Lucas-style process tests, optional local match harness

- [ ] Run a fresh Release and Debug configure/build/test cycle.
- [ ] Run default and profiled benchmarks, replay validation, and UCI smoke transcripts with clean stderr.
- [ ] Run the 20+ ply Lucas-style process scenario with `Hash=512`, `Threads=4`, and `Speed=100`.
- [ ] Run optional Jack/Stockfish matches and retain v2 JSON/PGN reports outside the repository.
- [ ] Update README with measured behavior and known deferred work; commit `docs: document next strength baseline`.
