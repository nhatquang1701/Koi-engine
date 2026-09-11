# Koi Strength Continuation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Improve Koi's short and clocked tactical decisions through measured, isolated corrections while preserving legality, UCI stability, deterministic fixed-depth behavior, and the existing classical architecture.

**Architecture:** Keep `SearchService` as the search boundary and use the existing emergency short-search fallback only when a complete iteration cannot fit. Each candidate starts with a concrete failing fixture, is measured against the retained executable, and is promoted only when tactical acceptance, oracle CPL/blunder data, stability, and performance remain acceptable.

**Tech Stack:** C++26/MSVC, CMake/Ninja Release, CTest, Koi benchmark profiles, PowerShell UCI tests, and Stockfish 19 as a diagnostic oracle only.

**Spec:** `docs/superpowers/specs/2026-09-09-strength-improvement-design.md`

## Global Constraints

- Standard FIDE chess only; preserve UCI options, executable names, and protocol-clean stdout.
- Preserve hash safety, completion validation, native/shadow legality, and Cutechess protections.
- Preserve deterministic `Threads=1` fixed-depth behavior; `Threads=2` and `Threads=4` must remain legal and cancellable.
- Keep fixed-depth performance within the established 15% budget.
- Stockfish 19 is an oracle/opponent for diagnostics only; do not add Elo, SPRT, rating, or confidence work.
- Every production behavior change has a focused regression test that fails before the implementation.
- Existing dirty and untracked files are user-owned; do not reset, clean, rename branches, push, tag, or delete unrelated artifacts.
- Durable reports remain below `artifacts/verification/` or `artifacts/stability/`.

## Evidence at the start of this increment

- The previous edge-queen-check candidate was rejected after a repeated oracle replay: mean suggested CPL 172.58 versus the retained 166.82 baseline, with 25 versus 24 blunders.
- Fixed-depth tactical acceptance remained 64/64, so the rejection was based on short-search evidence rather than legality.
- The current reproducible short-search miss is the FEN in `test_short_search_prefers_safe_forcing_exchange_over_quiet_push`: at 50–250 ms it previously returned `b2b4` without a completed iteration, while depth-limited and 500 ms searches selected `e5c6`.

## File map

- Modify `src/koi/search_service.cpp` only for a narrow emergency-fallback tactical correction whose fixture is already failing.
- Extend `tests/unit/search/koi_search_tests.cpp` with concrete tactical regressions and preserve all existing fixtures.
- Use `tools/measurement/elo_oracle.py`, `tools/measurement/strength_report.py`, and `tools/build/performance_gate.ps1` for non-rating evidence.
- Write new profiles, oracle reports, and gate outputs below `artifacts/verification/` with unique names.

### Task 1: Complete the short fallback exchange correction

- [x] Add the failing FEN fixture and confirm the current fallback chooses the quiet `b2b4` move.
- [x] Trace the fallback and confirm a safe equal non-pawn exchange is being rejected by the generic immediate-check quarantine.
- [x] Allow only equal-value, non-pawn, non-negative-SEE exchanges through that emergency guard; retain the guard for quiet moves and pawn captures.
- [x] Run the focused search suite and confirm the new fixture and existing poisoned-capture/queen-trap regressions pass.

### Task 2: Rebuild and measure the retained candidate

- [ ] Build Release `koi-engine.exe`, `koi-bench.exe`, and the focused test targets.
- [ ] Run the 64-position fixed-depth suite at Threads 1 and 4 and normalize both reports.
- [ ] Run the fixed 100 ms Stockfish-19 oracle replay twice with the same PGN, options, executable hash, and hardware metadata.
- [ ] Compare mean CPL, p95 CPL, 100-cp blunders, legal-PV rate, completed depth, nodes, qnodes, and elapsed time against the retained `a5c5` checkpoint.
- [ ] Retain the correction only if it fixes the target without worsening the broad evidence materially; otherwise remove only this correction and its fixture.

### Task 3: Stabilize short-search completion evidence

- [ ] Re-run the flaky 100 ms threaded-authoritative test at least three times and capture completed depth, fallback use, and elapsed diagnostics.
- [ ] If it remains flaky, add a deterministic diagnostic seam or reduce only the short checked-root quiescence workload; do not falsify completed-depth reporting or weaken incomplete-iteration rules.
- [ ] Verify cancellation, node limits, fixed-depth determinism, and UCI legality after any accepted timing/search change.

### Task 4: Select the next measured strength target

- [ ] Rank the repeated oracle report by CPL and classify each candidate as tactical, opening, strategic, or timing-related.
- [ ] Prefer a reproducible tactical consequence over an opening preference; use a legal FEN and a concrete property such as material preservation, mate avoidance, or a forced recapture.
- [ ] Add the failing fixture first, trace qsearch/SEE/LMR/TT/root fallback, and make one minimal correction.
- [ ] Reject any candidate that reduces tactical acceptance, adds illegal PVs, changes deterministic fixed-depth moves, or exceeds the performance budget.

### Task 5: Release checkpoint

- [ ] Run Release CTest, UCI process tests, hash-memory stability, rules/native-shadow tests, and Cutechess smoke serially.
- [ ] Run `git diff --check` and scan for temporary diagnostics or stdout contamination.
- [ ] Report exact executable hashes and evidence deltas without claiming a human win percentage or Elo.
