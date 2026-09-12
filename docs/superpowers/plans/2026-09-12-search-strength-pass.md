# Koi Search Strength Pass Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Improve Koi's one-second search accuracy from the reported 93.841% five-round average toward reproducible 95%+ strength using only Stockfish 19 search guidance.

**Architecture:** Keep all adaptive state worker-local. Add a staged picker beside the existing public diagnostic ordering API, then integrate it into the recursive regular and quiescence searches. Retain Koi's existing root completion/fallback contract and make selective pruning conservative around mate, repetition, sparse material, and excluded-move searches.

**Tech Stack:** C++26, MSVC x64, CMake/Ninja, existing Koi `GameState`, fixed move metadata containers, and the vendored Stockfish 19 C++ source.

**Spec:** `docs/superpowers/specs/2026-09-12-search-strength-pass-design.md`

## Global Constraints

- Use only the vendored Stockfish 19 source under `third_party/stockfish-19/stockfish-windows-x86-64-universal/stockfish/src` as search guidance.
- Do not change evaluation, NNUE, tests, fixtures, or the User-tests PGN.
- Do not run the accuracy match during implementation.
- Defer the complete Release and Debug suites until the implementation is judged complete.
- Preserve deterministic `Threads=1` fixed-depth behavior, legal PVs, cancellation, completed-iteration authority, repetition safety, and the 8192 MB Hash boundary.

### Task 1: Add a staged worker-local move picker

**Files:**
- Modify: `src/koi/detail/search_ordering.hpp`
- Modify: `src/koi/search_ordering.cpp`
- Modify: `src/koi/detail/search_ordering_tables.hpp`
- Modify: `src/koi/detail/search_ordering_tables.cpp`

**Interfaces:**
- Consumes: `GameState`, `MoveMetadataList`, optional TT move, `SearchHistoryContext`, and existing history/capture-history accessors.
- Produces: `SearchMovePicker`, constructed as `SearchMovePicker(const SearchMoveOrdering&, const GameState&, const MoveMetadataList&, std::optional<Move>, const SearchHistoryContext&, int, SearchMovePicker::Mode, Move excluded_move = Move::no_move())` and consumed through `std::optional<MoveMetadata> next()`. Existing `order(...)` overloads remain available to diagnostics and root helpers.

- [ ] Preserve the public `order(...)` behavior and current deterministic tie-break key.
- [ ] Define explicit picker stages for TT, good captures/promotions, killer/counter quiets, history-ranked quiets, and deferred losing captures; define a complete evasion stage for checked positions.
- [ ] Validate the TT move against the current metadata list before emitting it and skip excluded moves without duplicating candidates.
- [ ] Compute SEE only when a capture candidate enters a capture stage; use capture history and the existing static-exchange result to separate good and losing captures.
- [ ] Keep picker storage fixed-size and worker-local so the recursive path performs no per-node heap allocation.

### Task 2: Integrate staged selection into regular and quiescence search

**Files:**
- Modify: `src/koi/detail/search_context.hpp`
- Modify: `src/koi/detail/search_context_support.cpp` only if a helper is required by the picker boundary.
- Modify: `src/koi/detail/search_policy.hpp`
- Modify: `src/koi/detail/search_constants.hpp` only for named calibrated limits.

**Interfaces:**
- Consumes: Task 1 picker and existing `SearchFrame`/history context.
- Produces: regular negamax and qsearch loops that consume one staged candidate at a time and preserve current PV, TT, history-update, and cancellation contracts.

- [ ] Replace the regular-search eager ordering loop with staged candidate emission while retaining excluded-move singular probes and legal-move terminal handling.
- [ ] Preserve qsearch stand-pat, complete checked evasions, TT move hints, bounded quiet checks, SEE/delta pruning, and qsearch history updates.
- [ ] Calibrate LMR inputs from depth, move number, PV/cut node, TT/PV state, improving state, history/continuation/capture history, and prior child fail-high state.
- [ ] Re-search every reduced move that exceeds alpha at the authoritative child depth; do not let a reduced score become a PV without verification.
- [ ] Keep ProbCut, null move, singular extension, and multi-cut disabled for excluded/repetition-sensitive/check/mate/sparse-pawn cases where their bounds are not trustworthy.

### Task 3: Preserve root authority and one-second throughput

**Files:**
- Modify: `src/koi/search_service.cpp`
- Modify: `src/koi/detail/search_stack.hpp` only if picker/search frame state needs an explicit field.

**Interfaces:**
- Consumes: Task 1 staged root ordering and Task 2 recursive search results.
- Produces: root iterations that retain the previous PV/TT move, publish only completed iterations as authoritative, and remain deterministic for fixed-depth single-thread searches.

- [ ] Seed each root iteration with the previous authoritative move and staged ordering without changing search-move filters.
- [ ] Preserve aspiration widening, full-window fallback, cancellation, and short-search safety fallbacks.
- [ ] Keep multi-thread root scheduling legal and cancellation-safe; never replace a completed iteration with a partial result.
- [ ] Avoid new evaluator or NNUE calls beyond the existing search contracts.

### Task 4: Final audit and verification

**Files:**
- Modify: none.
- Test: existing Release and Debug build trees and CTest registrations only; no test files or fixtures are edited.

- [ ] Inspect `git diff --name-only` and confirm no tests, fixtures, evaluation, NNUE, weights, training tools, or User-tests paths changed.
- [ ] Run `git diff --check`.
- [ ] Rebuild Release with the Visual Studio x64 environment.
- [ ] Rebuild Debug with the Visual Studio x64 environment.
- [ ] Run `ctest --test-dir build/release --output-on-failure` once after implementation.
- [ ] Run `ctest --test-dir build/debug --output-on-failure` once after implementation.
- [ ] Report exact build and test outcomes and leave the 95% accuracy claim pending the user's En Croissant retest.
