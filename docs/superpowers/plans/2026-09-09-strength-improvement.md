# Koi Strength Improvement Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Improve Koi's practical playing strength through measured search and classical-evaluation increments without weakening its legal-UCI and deterministic stability guarantees.

**Architecture:** Keep `SearchService` responsible for search and `ClassicalEvaluator` responsible for static evaluation. Add a small measurement/reporting layer around the existing bench profiles and Stockfish-19 oracle output. Each behavior change is introduced through a regression fixture, measured against the frozen baseline, and retained only when tactical, protocol, determinism, and performance gates pass.

**Tech Stack:** C++26, CMake, Release CTest, PowerShell process tests, Python 3 standard library tooling, Koi bench profiles, and Stockfish 19 as a diagnostic oracle only.

**Spec:** `docs/superpowers/specs/2026-09-09-strength-improvement-design.md`

## Global Constraints

- Standard FIDE chess only; preserve all current UCI options and executable names.
- Preserve hash safety, adaptive time management, completion validation, and all current stability safeguards.
- `Threads=1` fixed-depth searches remain deterministic; `Threads=2` and `Threads=4` remain legal and cancellable.
- Fixed-depth median slowdown must remain at or below 15% relative to the recorded baseline.
- Stockfish 19 is used only for tactical/oracle move and CPL diagnostics; no Elo, SPRT, rating, or confidence campaign is implemented.
- Do not alter Git history with push, tag, reset, branch rename, or cleanup. Keep generated reports below ignored `artifacts/`.
- Production code is written only after a focused test or regression fixture has been observed failing for the intended reason.

## File Map

- Modify `src/koi/search_service.cpp` and `src/koi/search_service.hpp` only for isolated search behavior, root move ordering, quiescence, pruning, or search diagnostics.
- Modify `src/koi/search_ordering.cpp`, `src/koi/search_ordering.hpp`, and `src/koi/detail/search_ordering.hpp` only for ordering state and scoring.
- Modify `src/koi/classical_evaluator.cpp`, `src/koi/classical_evaluator.hpp`, `src/koi/evaluation_features.cpp`, and `src/koi/evaluation_parameters.hpp` for classical evaluation feature families and versioned parameters.
- Modify `tools/engine/koi_bench.cpp` only to enrich new profile records with the fixture's expected and accepted moves; keep schema `koi-bench-profile-v1` backward-compatible.
- Extend `src/koi/strength_suite.cpp` and `tests/unit/search/koi_strength_tests.cpp` with permanent tactical/strategic fixtures; keep the existing 64-position hard gate unchanged.
- Extend `tests/unit/search/koi_search_tests.cpp` and `tests/unit/evaluation/evaluation_boundary_tests.cpp` for search/evaluation regressions.
- Add `tools/measurement/strength_report.py` and `tests/python/evaluation/strength_report_test.py` for deterministic category reports from Koi bench JSON.
- Add `tools/measurement/cpl_report.py` and `tests/python/measurement/cpl_report_test.py` for non-rating Stockfish oracle diagnostics.
- Modify `CMakeLists.txt` only to register new Python tests or new C++ fixtures without renaming existing CTest targets.
- Use `artifacts/verification/` for ignored reports and preserve the two baseline JSON files as immutable inputs.

---

### Task 1: Freeze and summarize the strength baseline

**Files:**
- Create: `tools/measurement/strength_report.py`
- Create: `tests/python/evaluation/strength_report_test.py`
- Modify: `tools/engine/koi_bench.cpp` to include `expected_move` and `accepted_moves` in newly written profile positions
- Modify: `CMakeLists.txt` in the existing Python-test registration block
- Preserve: `artifacts/verification/strength-baseline-t1.json`
- Preserve: `artifacts/verification/optional-baseline-t1.json`

**Interfaces:**
- `strength_report.py` accepts `--profile <path>` one or more times, `--output <path>`, and `--expected-suite <strength|optional_strength>`.
- It writes schema `koi-strength-report-v1` containing executable/build/options metadata, profile hash, position count, category counts, expected-move acceptance, mean score, mean elapsed time, total nodes, total qnodes, and a list of mismatches.
- The report rejects malformed profiles, mixed suite names, mixed thread/speed/hash settings, and missing position IDs.

- [ ] **Step 1: Write the failing parser tests.** Test malformed JSON, a missing `pv`, category aggregation, deterministic JSON key ordering, and rejection of mixed options. Use temporary files created by the test and delete them in `tearDown`.

- [ ] **Step 2: Run the focused Python test and verify it fails because `strength_report.py` does not exist.**

Run:

```powershell
python -m unittest tests/python/evaluation/strength_report_test.py -v
```

- [ ] **Step 3: Implement the report parser.** Treat the first PV move as the selected move, compare it with `expected_move` only when the profile includes an expected move field, and otherwise report `selected_move` without inventing correctness. Compute all means with integer-safe decimal output and sort positions/categories by source order/name.

- [ ] **Step 4: Run the focused tests and generate the baseline reports.**

```powershell
python -m unittest tests/python/evaluation/strength_report_test.py -v
python tools/measurement/strength_report.py --profile artifacts/verification/strength-baseline-t1.json --expected-suite strength --output artifacts/verification/strength-baseline-t1.report.json
python tools/measurement/strength_report.py --profile artifacts/verification/optional-baseline-t1.json --expected-suite optional_strength --output artifacts/verification/optional-baseline-t1.report.json
```

Expected: tests pass; the tactical report records 64 positions and 64 accepted expected moves; the optional report records 128 positions split into positional, king_safety, and endgame categories.

- [ ] **Step 5: Register the Python test without changing existing CTest names.** Run the CMake configure step and the focused CTest target to verify registration.

### Task 2: Add permanent strategic regression fixtures

**Files:**
- Modify: `src/koi/strength_suite.cpp`
- Modify: `tests/unit/search/koi_strength_tests.cpp`
- Modify: `CMakeLists.txt` only if a new fixture source is needed
- Test output: `artifacts/verification/strategic-regressions-before.json`

**Interfaces:**
- Keep `strength_positions()` at exactly 64 entries and its accepted-move contract unchanged.
- Add a separate `strategic_regression_positions()` span with stable IDs, FEN, expected move, accepted alternatives, category, and minimum score where meaningful.

- [ ] **Step 1: Add failing tests for a small, representative corpus.** Select at least four positions from each optional category, prioritizing positions where the current profile selects a move different from the fixture expectation and positions involving pawn breaks, king shelter, passed pawns, and king activity. Assert FEN validity, legal expected moves, category coverage, and that the current engine fails at least one selected fixture at the diagnostic depth.

- [ ] **Step 2: Run `koi_strength_tests` and confirm the new regression test fails for a real move mismatch, not a malformed fixture.** Record the exact position ID, selected move, expected move, score, and depth in `artifacts/verification/strategic-regressions-before.json`.

- [ ] **Step 3: Implement only the fixture API and test helpers.** Do not change evaluator/search behavior in this task. Keep the suite diagnostic until a later task proves a behavior change.

- [ ] **Step 4: Run the focused C++ tests and verify the fixture inventory is stable.**

```powershell
cmake --build build/release --config Release --target koi_strength_tests
ctest --test-dir build/release -C Release -R koi_strength_tests --output-on-failure
```

### Task 3: Establish evaluator explainability and controlled parameter versioning

**Files:**
- Modify: `src/koi/classical_evaluator.hpp`
- Modify: `src/koi/classical_evaluator.cpp`
- Modify: `src/koi/evaluation_parameters.hpp`
- Modify: `tests/unit/evaluation/evaluation_boundary_tests.cpp`

**Interfaces:**
- Add a test-facing `EvaluationBreakdown` containing material, PST, mobility, pawn structure, activity, development, center, initiative, king safety, king activity, passed-pawn, and tempo terms.
- Add `ClassicalEvaluator::explain(const GameState&, Color) const` returning the breakdown without changing `evaluate()` semantics.
- Keep `evaluate()` as the sum of the breakdown and preserve the current parameter struct fields and defaults; if a parameter is renamed, retain a compatibility alias and update the generated metadata version.

- [ ] **Step 1: Write failing symmetry and decomposition tests.** Assert color symmetry, `sum(terms) == evaluate()`, no term changes sign incorrectly when the board is mirrored, and neutral material fixtures score near the expected tempo/development range.

- [ ] **Step 2: Run `evaluation_boundary_tests` and verify the missing `explain()` API or decomposition assertion fails.**

- [ ] **Step 3: Refactor the existing evaluator calculation into named term helpers without changing weights.** Keep each helper pure and use the existing `EvaluationFeatures` snapshot. `evaluate()` sums the returned terms in the same perspective convention as before.

- [ ] **Step 4: Run the full evaluation test target and compare old/new scores over the 64 tactical positions.** Any unexplained score change outside the breakdown refactor blocks the task.

### Task 4: Improve quiet-position move ordering with measured, safe signals

**Files:**
- Modify: `src/koi/search_ordering.cpp`
- Modify: `src/koi/search_ordering.hpp`
- Modify: `src/koi/detail/search_ordering.hpp`
- Modify: `src/koi/search_service.cpp`
- Extend: `tests/unit/search/search_ordering_tests.cpp`
- Extend: `tests/unit/search/koi_search_tests.cpp`

**Interfaces:**
- Keep the existing ordering containers and history storage source-compatible.
- Add deterministic quiet ordering components for: TT move, killer, countermove, continuation history, quiet history, improving flag, and static exchange safety.
- Expose no new UCI options. Fixed-depth `Threads=1` root ordering must remain stable for identical state and hash generation.

- [ ] **Step 1: Add failing ordering tests.** Construct positions where a legal TT move must remain first, a losing capture must rank below a safe capture, a killer must outrank an untrained quiet move, and a high-history move must not outrank a legal forcing move solely because of an unbounded history value.

- [ ] **Step 2: Run the focused ordering/search tests and verify each test fails against the current ordering behavior.**

- [ ] **Step 3: Implement bounded, phase-aware quiet scores.** Use saturating arithmetic, preserve explicit exclusions for checks/promotions/forcing captures, and do not make ordering depend on wall-clock time or thread scheduling. Store only worker-local speculative history until the existing merge point.

- [ ] **Step 4: Re-run focused tests, the 64-position suite, and the strategic diagnostic subset.** Reject the change if any tactical fixture regresses or if the fixed-depth node count rises beyond the recorded performance budget.

### Task 5: Improve quiescence and tactical consequence search only with regressions

**Files:**
- Modify: `src/koi/search_service.cpp`
- Modify: `src/koi/search_types.hpp` only for new diagnostic counters
- Extend: `tests/unit/search/koi_search_tests.cpp`
- Extend: `tests/unit/search/static_exchange_tests.cpp`

**Interfaces:**
- Add counters for safe checking continuations, quiet forcing continuations, and rejected losing captures while preserving existing counter names.
- Keep check evasions complete and keep cancellation/node-limit checks at every qsearch node.

- [ ] **Step 1: Add failing fixtures for concrete tactical consequences.** Include a poisoned capture where a check or recapture refutes the apparent gain, a quiet pawn break that opens a decisive line, a checking move that must be searched after a capture, and a sparse endgame where null move must not prune the only defense.

- [ ] **Step 2: Run `koi_search_tests` and confirm each new fixture fails or exposes the wrong counter before code changes.**

- [ ] **Step 3: Make one minimal search change at a time.** Preserve SEE and delta-pruning safety margins, search legal checking continuations when the side is not in check, and retain the last complete iteration when cancellation occurs. Do not simultaneously alter LMR/null-move formulas.

- [ ] **Step 4: Run tactical tests, `koi-bench.exe --threads 1`, and the cancellation/UCI process tests after each change.** Keep a JSON checkpoint with nodes, qnodes, score, PV, and elapsed time for every fixture.

### Task 6: Improve classical evaluation by feature family

**Files:**
- Modify: `src/koi/classical_evaluator.cpp`
- Modify: `src/koi/evaluation_parameters.hpp`
- Modify: `src/koi/evaluation_parameters_generated.hpp` only through the existing generator/update convention
- Modify: `src/koi/evaluation_features.cpp` only when a missing feature is required
- Extend: `tests/unit/evaluation/evaluation_boundary_tests.cpp`
- Extend: `tests/unit/search/koi_strength_tests.cpp`

**Interfaces:**
- Preserve the public evaluator interface and UCI `EvaluationMode` behavior.
- Keep feature values bounded and tapered by the existing game phase.
- Version parameter metadata for every accepted weight change; never silently replace a candidate network or parameter set.

- [ ] **Step 1: Add failing behavior tests for one feature family at a time.** Cover: development/castling readiness in the opening, king shelter/open files under enemy pressure, central pawn-break potential, passed-pawn support and promotion races, and king activity/opposition in low-material positions. Assert symmetry and monotonicity where the fixture has an unambiguous relation.

- [ ] **Step 2: Run the focused evaluation tests and record the current breakdowns for the selected strategic regressions.**

- [ ] **Step 3: Implement the smallest feature-family correction.** Use existing feature extraction where possible; do not add bonuses merely to force depth-one synthetic expected moves. A candidate must improve the deeper replay or Stockfish CPL signal as well as the fixture.

- [ ] **Step 4: Re-run evaluation tests, tactical tests, strategic regressions, and the optional profile.** Retain only candidates that do not reduce tactical acceptance or create material/king-safety symmetry failures.

- [ ] **Step 5: Repeat Steps 1–4 for the next feature family only after the prior family has a clean checkpoint.** Keep each parameter version and report under `artifacts/verification/`.

### Task 7: Add non-rating Stockfish-19 CPL and blunder diagnostics

**Files:**
- Create: `tools/measurement/cpl_report.py`
- Create: `tests/python/measurement/cpl_report_test.py`
- Modify: `CMakeLists.txt` in the existing Python-test registration block
- Use: `tools/measurement/stockfish_match.py` and existing forensic fixtures

**Interfaces:**
- `cpl_report.py` accepts a Koi move record/JSONL, a Stockfish oracle record/JSONL, and `--output`.
- It emits schema `koi-cpl-report-v1` with engine hashes, option metadata, corpus hash, move count, mean CPL, p95 CPL, blunder count, mate-normalization count, incomplete records, and category summaries.
- It rejects mismatched root FEN, side-to-move, executable hash, evaluator mode, or time-control metadata instead of merging incomparable records.

- [ ] **Step 1: Write failing tests for aligned records, FEN mismatch, mate score normalization, incomplete games, and deterministic p95 calculation.**

- [ ] **Step 2: Run the focused Python test and observe the expected missing-module failure.**

- [ ] **Step 3: Implement strict record alignment and CPL calculation.** Treat a draw/result as evidence only for the report’s result fields; do not convert it into an Elo estimate. Keep all raw records outside the report summary.

- [ ] **Step 4: Run the tests and one dry-run against existing forensic data.** Store only the report below `artifacts/verification/`.

### Task 8: Integrate gated benchmarks and process stability checks

**Files:**
- Modify: `tools/build/performance_gate.ps1`
- Modify: `tools/build/release_verify.ps1`
- Modify: `tests/integration/uci/uci_process_test.ps1` only for strength-phase command coverage if needed
- Add: `artifacts/verification/strength-checkpoint-<label>.json` as ignored generated output

**Interfaces:**
- Existing script parameters and executable names remain unchanged.
- Add an optional strength-report path and baseline path without changing the default Release verification contract.
- The gate checks tactical 64/64, report schema/options/hash compatibility, legal PVs, deterministic Threads=1 repeated runs, and the 15% timing ceiling.

- [ ] **Step 1: Add a failing PowerShell test case for a report with mismatched executable hash and for a tactical regression below 64/64.**

- [ ] **Step 2: Run the targeted PowerShell test and verify the gate rejects each invalid report.**

- [ ] **Step 3: Implement strict comparison and checkpoint writing.** Never compare a candidate to a report with different build hash, options, hardware, time control, or corpus hash. Keep diagnostics file-only.

- [ ] **Step 4: Run the gate against the frozen baseline and current candidate.** Confirm it rejects synthetic bad reports and accepts the unchanged baseline.

### Task 9: Full validation and final measured checkpoint

**Files:**
- No production file changes unless a preceding gate identifies a specific regression.
- Generate ignored reports under `artifacts/verification/` and `artifacts/stability/`.

- [ ] **Step 1: Build and run the complete Release test suite.**

```powershell
cmake --build build/release --config Release
ctest --test-dir build/release -C Release --output-on-failure
```

- [ ] **Step 2: Run the fixed-depth deterministic suite twice at Threads=1 and compare move, score, PV, nodes, and completion status.**

- [ ] **Step 3: Run the 64-position tactical benchmark at Threads=1, 2, and 4.** Require all 64 expected moves, legal PVs, no duplicate completions, and no stability failures.

- [ ] **Step 4: Run the optional strategic suite and generate category reports.** Compare against `optional-baseline-t1.report.json` with matching executable/options metadata.

- [ ] **Step 5: Run the no-book UCI/Cutechess smoke with Stockfish 19 at Threads=1, 2, and 4, preserving the existing incident-capture artifacts.** Reject any illegal move, crash, disconnect, stale output, time forfeit, or fallback.

- [ ] **Step 6: Run Stockfish-19 CPL diagnostics on the fixed corpus when the oracle executable is available.** Report only moves, CPL, blunders, PV legality, and search diagnostics.

- [ ] **Step 7: Apply the verification-before-completion checklist.** Report exact commands, exit codes, test counts, tactical/strategic metrics, performance comparison, and any remaining limitations. Do not claim a 70% human win rate unless actual paired human games exist; state the measured checkpoint instead.
