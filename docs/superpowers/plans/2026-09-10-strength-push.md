# Koi Strength Push Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Increase Koi's practical strength in short and clocked searches while preserving legal UCI behavior, deterministic fixed-depth Threads=1 behavior, and the existing Release stability gates.

**Architecture:** Keep the current classical evaluator and `SearchService` boundary. First correct the adaptive time manager's low-clock hard-position decision so a position with unstable or forcing search evidence may use its already-budgeted hard window; then measure the resulting depth and oracle diagnostics before making one tactical or evaluation change at a time. No Elo or rating machinery is added.

**Tech Stack:** C++26/MSVC, CMake Release, CTest, PowerShell UCI process tests, Koi bench profiles, and Stockfish 19 as a diagnostic oracle only.

**Spec:** `docs/superpowers/specs/2026-09-09-strength-improvement-design.md`

## Global Constraints

- Standard FIDE chess only; preserve public UCI options, executable names, and protocol-clean stdout.
- Preserve hash safety, segmented allocation, completion validation, and current Cutechess protections.
- `Threads=1` fixed-depth searches remain deterministic; `Threads=2` and `Threads=4` remain legal and cancellable.
- Fixed-depth median slowdown must remain at or below 15% against the current recorded baseline.
- Stockfish 19 is an oracle/opponent for diagnostics only; no Elo, SPRT, rating, or confidence campaign is implemented.
- Every production behavior change is preceded by a focused test that fails for the intended reason.
- Existing dirty and untracked files are user-owned; do not reset, clean, rename branches, push, tag, or delete unrelated artifacts.
- Durable reports remain below the organized repository path `artifacts/verification/` or `artifacts/stability/`.

## Evidence captured before this increment

- Current Release executable: `build/release/koi-engine.exe`, SHA-256 `89f0b83429a4c5af0d55faf05d39b52f0f611d927e049842a6082e4a8135b18b`.
- Existing Release CTest gate: 38/38 passed before this plan.
- The disputed FEN `rnb1kb1r/pp3ppp/4p3/q1ppN1B1/3Pn3/2N5/PPP1PPPP/R2QKB1R w KQkq - 6 8` returns `g5d2` consistently at Hash 16/512, Threads 1/4, depths 1–6 in fresh processes.
- Existing 100 ms oracle records show several hard tactical positions where the engine's timed result matches a shallow depth-1/2 choice while a depth-3/4 search changes to the stronger move.

## File Map

- Modify `src/koi/time_manager.cpp` and `src/koi/time_manager.hpp` only for the low-clock hard-position budget decision.
- Extend `tests/unit/search/koi_search_tests.cpp` with deterministic injected-clock tests for hard versus stable low-clock behavior.
- Modify `src/koi/search_service.cpp` only if the timing regression shows that a complete hard iteration is not allowed to consume the hard budget; do not alter move heuristics in the timing task.
- Add `tools/measurement/short_search_diagnostic.py` and its test only if the existing oracle records cannot expose depth/time data in a stable, reusable report; otherwise preserve the current measurement scripts.
- Preserve all baseline JSON files and write new profiles with unique names below `artifacts/verification/`.

---

### Task 1: Make low-clock hardness use the existing hard window

**Files:**
- Modify: `src/koi/time_manager.cpp`
- Test: `tests/unit/search/koi_search_tests.cpp`

**Interface:** Keep the existing `TimeManager` constructors and diagnostic fields. `should_stop_after_iteration()` and `should_start_next_iteration()` must continue to respect the reserve and hard deadline, but low-clock pacing may stop at the soft budget only for a stable/easy position. An initial hard root or an iteration that becomes unstable may continue toward `hard_budget`.

- [ ] **Step 1: Add a failing test for an initial hard low-clock root.** Construct `SearchLimits` with 2,000 ms remaining, zero increment, a deterministic `RootTimingContext` with a TT miss and forcing move majority, and an injected monotonic clock. Advance past the soft budget but before the hard budget; assert `should_stop_after_iteration()` is false and `should_start_next_iteration()` is true.
- [ ] **Step 2: Add a failing test for a stable low-clock root.** Use the same clock but an exact recent TT entry, stable observations, and the same budget. Advance past the soft budget; assert the manager stops after the completed iteration and does not start another iteration.
- [ ] **Step 3: Run only `koi_search_tests` and verify the hard-root test fails because emergency pacing currently stops unconditionally at soft time.
- [ ] **Step 4: Change only the emergency-pacing guards.** Gate the soft-time early stop on `!hard_position_`; leave `budget_`, reserve calculations, node limits, explicit movetime ceilings, infinite, ponder, and cancellation unchanged. Keep `hard_budget <= usable` and never spend the reserve.
- [ ] **Step 5: Re-run the focused tests and then the existing timing and cancellation tests.** Record the hard/soft elapsed behavior in `artifacts/verification/time-management-hard-root.json`.

### Task 2: Measure the short-search strength effect

**Files:**
- Use: `tools/engine/koi_bench.cpp`, `tools/measurement/elo_oracle.py`, `tools/measurement/strength_report.py`
- Output: `artifacts/verification/strength-short-before.json`, `artifacts/verification/strength-short-after.json`

- [ ] **Step 1: Run the current 64-position timed benchmark at Threads 1 and 4 with a fresh executable hash recorded.** Keep the existing hard gate at 64/64 and record depth, nodes, qnodes, elapsed time, and NPS.
- [ ] **Step 2: Replay the existing Stockfish-19 oracle corpus at its recorded short time control.** Compare move legality, depth, mate normalization, mean CPL, p95 CPL, and blunder count. Do not convert any result into Elo.
- [ ] **Step 3: Compare before/after only when executable hash, options, hardware, time control, and corpus hash match.** If the timing change does not improve hard-position depth or worsens tactical acceptance/performance, revert only that timing change before proceeding.

### Task 3: Add one reproducible tactical consequence regression

**Files:**
- Modify: `tests/unit/search/koi_search_tests.cpp`
- Modify: `src/koi/search_service.cpp` only after the test is red

- [ ] **Step 1: Use the existing oracle evidence to add a legal FEN fixture where depth 2 chooses a materially worse move but depth 4 finds the forcing continuation.** Assert only the concrete tactical property (mate avoidance, material preservation, or forced recapture), not a Stockfish-only stylistic move.
- [ ] **Step 2: Run the focused test and confirm it fails against the current behavior at the bounded search limit.
- [ ] **Step 3: Trace the root path through qsearch, check evasions, SEE, LMR, null move, and root completion before changing code.** Add one diagnostic counter or narrow condition only when the trace identifies the missed consequence.
- [ ] **Step 4: Implement the smallest correction and verify that incomplete iterations still leave the last complete result authoritative.
- [ ] **Step 5: Run the focused search tests, the 64-position hard suite, and the UCI lifecycle tests.** Reject any change that affects legal PVs, deterministic Threads=1 results, or the 15% performance budget.

### Task 4: Improve one evaluation family using deeper evidence

**Files:**
- Modify: `src/koi/classical_evaluator.cpp`
- Modify: `src/koi/evaluation_parameters.hpp` and generated metadata only through the existing parameter convention
- Test: `tests/unit/evaluation/evaluation_boundary_tests.cpp`
- Test: `tests/unit/search/koi_strength_tests.cpp`

- [ ] **Step 1: Select the highest-impact non-tactical category from the deeper oracle/strategic corpus, not from a depth-one expected-move mismatch alone.
- [ ] **Step 2: Add a paired monotonicity or symmetry test that fails with the current feature family.
- [ ] **Step 3: Adjust one bounded feature family (development/castling, king shelter, pawn breaks, passed-pawn support, or endgame king activity) without changing public evaluator APIs.
- [ ] **Step 4: Verify the breakdown, tactical suite, optional suite, and Stockfish diagnostic report.** Keep the parameter version and report under `artifacts/verification/` only if the candidate improves the chosen metric without a tactical or stability regression.

### Task 5: Release checkpoint

- [ ] Build `build/release` and run the complete Release CTest suite.
- [ ] Run deterministic Threads 1/2/4 UCI searches on the reproduced FEN and the tactical suite.
- [ ] Run the Cutechess smoke/stability tests with no book and the normal book configuration when available.
- [ ] Run `git diff --check` and verify no diagnostic output reached UCI stdout.
- [ ] Report exact hashes, tests, benchmark deltas, and remaining evidence gaps. Do not report a human win percentage or an Elo claim.

## Self-review

- The plan keeps the first behavior change isolated to time management and does not mix it with search/evaluation heuristics.
- The plan covers the current observed boundary (shallow short searches) and retains the existing legal/UCI/performance gates.
- All production changes have a preceding failing-test step.
- The plan does not claim that a benchmark or Stockfish oracle proves a 70% win rate against an unspecified human opponent.
