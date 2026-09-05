# Koi Engine Task 4 Syzygy Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add optional, safe, pinned Fathom-backed Syzygy root probing without changing existing Koi UCI, book, cancellation, or deterministic search behavior.

**Architecture:** `GameState` exposes only a Koi-owned read-only tablebase snapshot containing piece bitboards and rule metadata. `SyzygyTablebase` owns the Fathom handle and conversion, serializes root probes, permits concurrent WDL probes, and returns Koi-owned WDL/root-choice results. `SearchService` receives an optional tablebase and performs a single-PV root probe before ordinary search; the controller owns the UCI option state and reports `tbhits` through `SearchInfo`/UCI output.

**Tech Stack:** C++26, CMake, Visual Studio x64 Debug/Release, CTest, PowerShell process tests, and the pinned jdart1/Fathom snapshot.

**Spec:** `.superpowers/sdd/2026-09-05-long-horizon-elo-roadmap/task-4-brief.md`

## Global Constraints

- Work directly in `C:\Users\ntATh\AI test\Koi engine`.
- Preserve all prior UCI behavior, licensed book defaults, legal/deterministic `Threads=1` search, cancellation, and clean stdout.
- Keep `chess.hpp` private to `GameState` and expose only Koi-owned tablebase/rule metadata.
- Use Fathom commit `c9c6fef0dddc05d2e242c183acf5833149ab676d` and preserve its license in `third_party/fathom`.
- Do not bundle tablebase files; absent or unreadable paths disable probing safely.
- Syzygy probe limit is `0..5`, probe depth is `1..100`, and the UCI names are exactly `SyzygyPath`, `SyzygyProbeDepth`, `SyzygyProbeLimit`, and `Syzygy50MoveRule`.
- Do not dispatch subagents; use the Terra execution path only.

### Task 1: Add Koi-owned metadata and Syzygy tests first

**Files:**
- Modify: `tests/koi_core_tests.cpp`, `tests/koi_search_tests.cpp`, `tests/uci_controller_tests.cpp`, and `CMakeLists.txt`
- Create: `tests/syzygy_tablebase_tests.cpp`

**Interfaces:**
- Tests consume the planned `GameState::tablebase_snapshot()`, `SyzygyTablebase`, and `SearchInfo::tbhits` APIs.
- Tests produce explicit RED evidence for absent/malformed paths, piece-count gating, metadata conversion, legal WDL root choices, draw/win/loss conversion, concurrent WDL access, and UCI parsing.

- [ ] Write tests for snapshot castling flags, en-passant square, halfmove clock, side-to-move, and twelve Koi-owned piece bitboards without including `chess.hpp`.
- [ ] Write tests for disabled absent/malformed paths, piece counts above five, unsupported castling, and valid legal root filtering when a small supplied fixture is available.
- [ ] Write tests for WDL-to-score/mate conversion, `tbhits`, concurrent WDL calls, and exact UCI option advertisement plus safe invalid values.
- [ ] Register `syzygy_tablebase_tests` and extend existing UCI/search assertions for tablebase bypass preservation.
- [ ] Run the focused Debug tests and capture the expected missing-interface/behavior failures before adding production code.

### Task 2: Vendor Fathom and implement the private adapter

**Files:**
- Create: `third_party/fathom/tbprobe.h`, `third_party/fathom/tbprobe.cpp`, `third_party/fathom/LICENSE`
- Create: `src/koi/syzygy_tablebase.hpp`, `src/koi/syzygy_tablebase.cpp`
- Modify: `src/koi/game_state.hpp`, `src/koi/game_state.cpp`, and `CMakeLists.txt`

**Interfaces:**
- `GameState::tablebase_snapshot()` returns a Koi-owned immutable snapshot and never exposes native chess types.
- `SyzygyTablebase(std::filesystem::path, std::uint8_t probe_limit, std::uint8_t probe_depth, bool fifty_move_rule)` safely disables itself when initialization fails.
- `SyzygyTablebase::probe_wdl(const TablebaseSnapshot&)` is safe for concurrent calls; `probe_root(...)` serializes Fathom root access and returns legal Koi moves plus a normalized score/mate.
- `SyzygyTablebase::hits()` returns the adapter’s probe count.

- [ ] Add the exact pinned Fathom sources and license, with no tablebase data files.
- [ ] Implement immutable configuration validation, path existence/readability checks, and Fathom initialization/teardown.
- [ ] Implement square/color/piece conversion, castling and en-passant mapping, five-piece gating, WDL probes, and root serialization.
- [ ] Implement Koi-owned result conversion for draw, win, loss, and mate-distance scores.
- [ ] Run the new tests GREEN and refactor only while keeping them green.

### Task 3: Integrate root probes and UCI options

**Files:**
- Modify: `src/koi/search_types.hpp`, `src/koi/search_service.hpp`, `src/koi/search_service.cpp`, `src/koi/uci_controller.hpp`, `src/koi/uci_controller.cpp`, and `CMakeLists.txt`
- Modify: `tests/koi_search_tests.cpp`, `tests/uci_controller_tests.cpp`, and `tests/uci_process_test.ps1`

**Interfaces:**
- `SearchOptions` carries a shared optional `SyzygyTablebase` without affecting default construction.
- `SearchStats`/`SearchInfo` carry `tbhits` and UCI prints `tbhits` on valid info lines.
- Controller advertises exact defaults/ranges, applies only valid values, stops and joins before changing tablebase configuration, and snapshots options per search.

- [ ] Add a pre-search root-probe branch only for `MultiPV=1`, non-analysis, non-ponder, and non-`searchmoves` searches; leave book eligibility and book bypass unchanged.
- [ ] Filter to legal tablebase root choices, emit a legal best move and mate/draw score, and fall back to ordinary search when no root result is available.
- [ ] Preserve cancellation, Threads=1 determinism, threaded behavior, analysis/MultiPV/ponder/searchmoves/book bypass, and clean stdout.
- [ ] Advertise and parse `SyzygyPath`, `SyzygyProbeDepth`, `SyzygyProbeLimit`, and `Syzygy50MoveRule` with safe invalid handling.
- [ ] Run focused tests GREEN and then all existing rules/search/UCI suites.

### Task 4: Build, self-review, report, and commit

**Files:**
- Modify: `.superpowers/sdd/2026-09-05-long-horizon-elo-roadmap/task-4-report.md`

**Interfaces:**
- Consumes the tested source tree and produces reproducible Debug/Release builds, focused/full verification evidence, a requirement-by-requirement self-review, and one focused commit.

- [ ] Configure and build fresh Debug and Release trees with the Visual Studio x64 environment.
- [ ] Run focused Syzygy tests, all existing rules/search/UCI tests, full Debug and Release CTest, and supplied-fixture checks only when available.
- [ ] Inspect the final diff for accidental changes, verify the Fathom snapshot/license/hash, and confirm no tablebase files are bundled.
- [ ] Write the detailed report at `.superpowers/sdd/2026-09-05-long-horizon-elo-roadmap/task-4-report.md` before committing.
- [ ] Commit with a focused subject and record the resulting commit in the report.
