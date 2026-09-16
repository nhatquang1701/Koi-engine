# Koi UCI Controller Improvement Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Harden and simplify the UCI controller without changing its protocol contract: clear the hash on `ucinewgame`, make book completion null-safe, centralize the bare-`go` fallback, support the standard `debug on|off` command and `info ... hashfull`, replace the hand-written option chain with a single table-driven registry, and convert a running ponder search in place on `ponderhit` instead of restarting it.

**Architecture:** `UciController` remains the only protocol-output owner and `SearchService` remains the asynchronous search owner. `Hash`/`Hashfull` read from the shared `TranspositionTable`; the handshake and `setoption` both read one `kUciOptions` descriptor table. Ponder continuation adds a `request_ponderhit` hand-off from the controller into `SearchSession`, and `search_runner` consumes it at iteration boundaries, where `TimeManager::reconfigure` swaps the ponder limits for the original command limits.

**Tech Stack:** C++26, MSVC x64, CMake/Ninja, PowerShell 7 process tests, existing Koi `GameState`/`SearchService` seams.

**Verification:** `docs/superpowers/verification/2026-09-16-uci-controller-improvement.md`

## Global Constraints

- Windows x64 with MSVC only; stdout stays exclusively controller-written UCI output.
- The `uci` handshake must remain byte-identical, including option order and the dynamic `Threads` maximum.
- Invalid `setoption` values stay complete no-ops: they never stop or mutate an active search.
- Exactly one `bestmove` per search; stale generations remain suppressed.
- Do not commit unless asked.

### Task 0: Baseline

- [x] Record the Release baseline: `uci_controller_tests` and `completion_gate_tests` pass before edits.
- [x] Confirm the toolchain and Ninja build tree (`build\release`, MSVC 14.44, x64) are usable without reconfiguring.

### Task 1: Robustness quick wins

**Files:**
- Modify: `src/koi/uci_controller.cpp`
- Modify: `tests/unit/runtime/uci_controller_tests.cpp`

- [x] `ucinewgame` now stops and suppresses any active search, clears the transposition table (`search_service_.clear_hash()`), records `debug_json_event("ucinewgame", "\"hash_cleared\":true")`, and resets the position.
- [x] `write_book_completion` guards a missing validated move before emitting, logs the condition through the Debug channel, and answers `bestmove 0000` under the generation check, mirroring the search completion path.
- [x] Centralize the bare-`go` fallback as `kBareGoFallback{250}` and use it from `parse_go_limits`, `handle_go`, and both `handle_ponderhit` budget paths.
- [x] Accept `debug on` / `debug off` (case-insensitive) as an alias for the hidden `Debug` option; a change stops and joins the active search before toggling.
- [x] Add `TranspositionTable::hashfull_permill()` (bounded 1000-slot sample across all stripes; `clear()` logically invalidates so it reports 0 afterwards), `SearchService::hashfull_permill()`, and emit `hashfull` between `nps` and `time` in `info` lines.
- [x] Tests: `ucinewgame` Debug-log assertion, `debug on|off` toggle test, and a transposition-table occupancy test (fresh 0, populated 1000, cleared 0); update the strict info parsers in `uci_controller_tests.cpp`, `tests/support/UciSession.psm1`, and `en_croissant_uci_test.ps1`.

### Task 2: Table-driven option registry

**Files:**
- Modify: `src/koi/uci_controller.cpp`
- Modify: `tests/unit/runtime/uci_controller_tests.cpp`

- [x] Introduce `UciOptionKind`, `UciOptionId`, and `UciOptionDescriptor`, plus `kUciOptions` holding all 26 options (24 advertised, `Debug`/`DebugFile` hidden) in the historical advertisement order.
- [x] Rewrite `write_handshake` to render `kUciOptions` (skipping unadvertised entries), preserving byte-identical output.
- [x] Rewrite `handle_setoption` to resolve via `find_uci_option` and dispatch on `UciOptionId`, with shared `apply_boolean`/`apply_unsigned` helpers that reproduce stop/suppress/commit and invalid-value semantics exactly.
- [x] Add a structural drift test: the handshake advertises exactly 24 unique well-formed options, and setting every option to its default emits one `readyok` and no `bestmove` or diagnostics.

### Task 3: In-place ponder continuation

**Files:**
- Modify: `src/koi/time_manager.hpp`, `src/koi/time_manager.cpp`
- Modify: `src/koi/detail/search_session.hpp`, `src/koi/detail/search_session.cpp`
- Modify: `src/koi/search_service.hpp`, `src/koi/search_service.cpp`
- Modify: `src/koi/detail/search_runner.cpp`
- Modify: `src/koi/uci_controller.cpp`
- Modify: `tests/unit/runtime/uci_controller_tests.cpp`, `tests/unit/search/koi_search_tests.cpp`

- [x] Factor the `TimeManager` constructor body into `initialize(...)` and add `reconfigure(...)` that replaces limits/root context and restarts budget accounting (movetime or clock) from the ponderhit moment.
- [x] Add `SearchSession::request_ponderhit(SearchLimits)`, `ponderhit_requested()`, `take_ponderhit_limits()`, and `wait_until_stopped_or_ponderhit()`; forward through `SearchHandle::request_ponderhit`.
- [x] In `search_runner`, hold a mutable local copy of the session limits and consume a pending conversion at the top of both the single-thread and root-pool iteration loops before the depth/unbounded gates, recomputing `maximum_depth` and `unbounded`.
- [x] Wake a terminal ponder root on ponderhit as well as stop so it still answers.
- [x] Rewrite `handle_ponderhit`: a running ponder search receives the converted limits (ponder cleared, bare fallback only when no normal limit exists) and keeps its generation, PV, and accumulated work; a finished or absent search keeps the bounded fallback restart of the current position.
- [x] Update the controller lifecycle test to continuation semantics (one `bestmove`, one `search start generation` Debug entry, `"continued":true` event) and add a search-level in-place conversion test.

### Task 4: Documentation

- [x] Update `README.md`: `ucinewgame` hash clear, `info ... hashfull`, in-place `ponderhit`, `debug on|off`, and the Lucas Chess ponder note.
- [x] Add this plan and its verification record.

### Deferred

- Hashfull sampling is a bounded approximation rather than an exact count, matching the UCI field's purpose.
- Pre-loop heuristics and the tablebase eligibility gate in `search_runner` are computed from the original ponder limits and are not recomputed after a ponderhit conversion.
- `go mate`, `currmove`/`cpuload`, Chess960, and `register` remain out of scope.
