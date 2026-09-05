# Koi Engine En Croissant Intelligence Roadmap Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make Koi a measurably stronger classical UCI engine for En Croissant while preserving standard UCI interoperability, deterministic single-thread search, legal move output, and portable Windows x64 Release builds.

**Architecture:** Keep `UciController` as the only protocol writer and snapshot all search options into `SearchOptions` before a search starts. Keep chess-library ownership inside `GameState`, use Koi-owned move/search/evaluation metadata in hot paths, and isolate optional book/tablebase/diagnostic facilities behind small interfaces. En Croissant becomes the primary documented GUI workflow, while the engine continues to speak generic UCI so other GUIs remain compatible.

**Tech Stack:** C++26, CMake/Ninja or Visual Studio x64, CTest, PowerShell process tests, the pinned chess library, optional Fathom/Syzygy, Python 3 plus `python-chess` only for measurement tooling, and Stockfish 19 for oracle/match reports.

**Spec:** User-approved Koi Engine Long-Horizon Elo and Compatibility Roadmap in the current task request, including the En Croissant compatibility decision.

## Global Constraints

- Windows x64, standard chess only, C++26, and portable Release optimization remain required.
- `Threads=1` is the correctness and deterministic reference path; threaded output must preserve legal moves and fixed-depth parity.
- `OwnBook=true`, `BookFile=book.bin`, `BookDepth=16`, `BookRandom=false`, and book safety enabled remain the defaults.
- Generic UCI behavior is preserved; En Croissant is the primary documentation and process-test target, not a protocol fork.
- No NNUE, variants, unverified opening data, or embedded third-party book data.
- No Elo claim is made without reproducible CPL or color-balanced match evidence.
- Only Terra and Luna subagents may be dispatched; never use Sol.
- Preserve untracked `tests/__pycache__/` and `third_party/Stockfish 19/`.
- Production code changes follow TDD: add a focused failing behavior test, observe RED, implement the smallest change, observe GREEN, then refactor.

---

### Task 1: En Croissant compatibility contract and process harness

**Files:**
- Modify: `README.md`, `CMakeLists.txt`, `tools/uci_process_test.ps1` or the repository’s current UCI process-test entry point.
- Create or modify: `tests/en_croissant_uci_test.ps1`, `tests/uci_transcript_helpers.ps1` when a helper is needed.
- Inspect: `src/koi/uci_controller.cpp`, `src/koi/uci_controller.hpp`.

**Interfaces:**
- Consumes the existing standard UCI controller and executable target.
- Produces an En Croissant-style transcript test that launches Koi as a child process, sends handshake/options/position/go/stop/quit and analysis commands, and asserts clean UCI output with exactly one legal `bestmove` per search.

- [ ] Write a failing transcript test covering `uci`, `isready`, `setoption`, `position startpos moves`, `go`, `stop`, `MultiPV`, `go infinite`, and `quit`.
- [ ] Run the focused process test and capture the expected RED result for any missing compatibility behavior.
- [ ] Implement only the missing parser/dispatch behavior, retaining Lucas-compatible standard UCI aliases and ignoring unknown GUI commands safely.
- [ ] Add README setup instructions centered on En Croissant, including the executable path, `book.bin` placement, recommended options, analysis/tutor/MultiPV expectations, and generic UCI fallback.
- [ ] Run the focused transcript test, then the existing UCI tests.
- [ ] Commit the task and record the exact transcript and test commands in the ledger report.

### Task 2: Correctness fixes for search state and optional tablebases

**Files:**
- Modify: `src/koi/search_service.cpp`, `src/koi/syzygy_tablebase.hpp`, `src/koi/syzygy_tablebase.cpp`.
- Test: `tests/koi_search_tests.cpp`, `tests/syzygy_tablebase_tests.cpp` or the existing tablebase test file.

**Interfaces:**
- Consumes the current search and Fathom adapter interfaces.
- Produces correct saved-side history updates and shared/ref-counted Fathom lifecycle behavior so multiple tablebase instances cannot free process-global state prematurely.

- [ ] Add a regression test that exercises a quiet-history update after `unmake_move` and verifies the update is attributed to the moving side.
- [ ] Add a lifecycle test that creates two tablebase users, initializes/clears them in different orders, and verifies neither invalidates the other.
- [ ] Run the focused tests RED before changing production code.
- [ ] Save `moving_side` before unmake and use a process-wide guarded Fathom ownership record with reference counting or equivalent serialized acquire/release semantics.
- [ ] Test missing, malformed, empty, and valid-path fallback behavior without requiring bundled tablebase files.
- [ ] Run all existing search/tablebase tests and document any fixture limitation.

### Task 3: Strength-first opening-book safety and audit visibility

**Files:**
- Modify: `src/koi/opening_book.hpp`, `src/koi/opening_book.cpp`, `src/koi/uci_controller.hpp`, `src/koi/uci_controller.cpp`, `README.md`.
- Test: `tests/opening_book_tests.cpp`, `tests/uci_controller_tests.cpp`, process transcript tests.

**Interfaces:**
- Consumes Polyglot book loading and current option snapshot behavior.
- Produces `BookSafety`, `BookSafetyDepth`, `BookRandom`, deterministic highest-weight selection, safe fallback, and optional non-protocol audit diagnostics.

- [ ] Add failing tests for deterministic equal-weight ordering, unsafe early-book fallback, `BookRandom` opt-in, missing/malformed book fallback, and bypass during analysis/MultiPV/ponder/infinite/searchmoves.
- [ ] Run the focused book tests RED.
- [ ] Implement legal-move filtering and a conservative safety gate: when enabled, verify the selected book move remains legal and does not immediately lose material by the configured shallow safety probe; otherwise fall back to search.
- [ ] Keep runtime randomness restricted to `BookRandom=true`; preserve repeatability for nonzero `RandomSeed`.
- [ ] Expose only valid UCI options through the public handshake and route book audit details to debug/profile output rather than stdout noise.
- [ ] Run book, UCI, and process tests, including an executable-relative `book.bin` lookup check.

### Task 4: Tactical search reliability and move-ordering quality

**Files:**
- Modify: `src/koi/search_service.cpp`, `src/koi/search_types.hpp`, `src/koi/move_picker.hpp` and corresponding implementation files if present.
- Test: `tests/koi_search_tests.cpp`, `tests/search_ordering_tests.cpp`, tactical fixtures under `tests/data/` or the existing fixture location.

**Interfaces:**
- Consumes current `SearchContext`, TT, evaluator, move metadata, and cancellation interfaces.
- Produces stronger tactical reliability without changing the stable `Threads=1` tie-break rule.

- [ ] Add failing tests for checks ahead of quiet moves, checks excluded from LMR, reduced-move full-depth verification, sparse-endgame null-move suppression, mate-distance ordering, poisoned captures, and check evasions.
- [ ] Run the focused tactical tests RED.
- [ ] Generate `gives_check` metadata once per node; keep TT moves, captures, promotions, checks, killers, and forcing moves out of LMR.
- [ ] Re-search every reduced move that improves alpha with a full-depth search; disable null move below the configured endgame phase and keep existing verification safeguards.
- [ ] Preserve complete qsearch checks/evasions, SEE, delta pruning, TT mate normalization, cancellation, and last-completed-iteration semantics.
- [ ] Run the 64-position tactical gate at `Threads=1`, `2`, and `4`, plus fixed-depth parity tests.

### Task 5: Search hot-path and threaded efficiency improvements

**Files:**
- Modify: `src/koi/search_service.cpp`, `src/koi/transposition_table.cpp`, `src/koi/transposition_table.hpp`, `src/koi/game_state.cpp`, `src/koi/game_state.hpp`, `CMakeLists.txt`.
- Test: `tests/search_service_tests.cpp`, `tests/transposition_table_tests.cpp`, `tests/benchmark_tests.cpp`.

**Interfaces:**
- Consumes the current root worker pool, TT, direct move conversion, and benchmark interfaces.
- Produces lower overhead while preserving global node limits, cancellation, deterministic root order, and `Threads=1` reference results.

- [ ] Add failing tests for global node-limit accounting, prompt threaded cancellation, TT concurrent probe/store stress, exactly one completion callback, and `Threads=1`/`Threads=4` fixed-depth parity.
- [ ] Run focused concurrency tests RED.
- [ ] Remove any redundant serial confirmation search in the authoritative multi-thread single-PV path; use full-window root jobs and stable tie-breaking.
- [ ] Replace hot-path UCI conversions and recursive heap PV vectors with direct move metadata and fixed-size buffers where existing interfaces permit.
- [ ] Use striped TT synchronization for probe/store and serialize only clear/resize/generation transitions; keep no CPU-specific instruction requirement.
- [ ] Add benchmark profiles for cold/warm hash, `--threads`, `--speed`, and optional `--timed`; enforce only correctness/parity in CI.
- [ ] Run deterministic benchmark comparisons and verify the fixed-depth median does not regress by more than 5% against the accepted baseline.

### Task 6: Classical evaluation strength pass and optional StrengthMode

**Files:**
- Modify: `src/koi/classical_evaluator.cpp`, `src/koi/classical_evaluator.hpp`, `src/koi/search_types.hpp`, `src/koi/uci_controller.cpp`, `src/koi/uci_controller.hpp`.
- Create or modify: `src/koi/evaluation_parameters_generated.hpp`, `tools/tune_evaluation.py` if the current evaluator is ready for generated parameters.
- Test: `tests/classical_evaluator_tests.cpp`, `tests/koi_search_tests.cpp`, evaluator fixtures.

**Interfaces:**
- Consumes Koi-owned position features and existing evaluator entry points.
- Produces tapered middlegame/endgame scoring, material/PST/mobility/pawn/king-safety/endgame terms, diagnostic breakdowns, and a deterministic `StrengthMode` check option that uses more conservative strength-oriented search settings without random weakening.

- [ ] Add failing evaluator tests for color symmetry, material sanity, bishop pair, mobility, pawn structure, king safety, insufficient material, and endgame scaling; add option tests for `StrengthMode` snapshotting and search cancellation on change.
- [ ] Run evaluator tests RED.
- [ ] Implement or refine the tapered terms one coefficient family at a time, preserving centipawn sign conventions and side-to-move symmetry.
- [ ] Generate a checked-in parameter header only when tuning tooling has a reproducible corpus/hash; otherwise keep versioned constants in the existing header with provenance comments.
- [ ] Add `option name StrengthMode type check default false` without changing the default fast profile; make the enabled profile improve tactical depth/order within the same time manager.
- [ ] Run evaluator/search tests and compare tactical solves before and after each coefficient group.

### Task 7: Diagnostics, Stockfish oracle integration, and En Croissant documentation

**Files:**
- Modify: `src/koi/debug_logger.cpp`, `src/koi/debug_logger.hpp`, `src/koi/uci_controller.cpp`, `README.md`.
- Create or modify: `tools/elo_oracle.py`, `tools/stockfish_match.py`, `tools/book_audit.py`, `tools/README.md`.
- Test: `tests/debug_logging_tests.cpp`, `tests/oracle_extract_test.py`, `tests/uci_process_test.ps1`.

**Interfaces:**
- Consumes UCI process behavior, Stockfish 19 archive configuration, PGN inputs when supplied, and licensed `book.bin`.
- Produces reproducible extract-only/oracle reports with FEN, actual move, Koi move, Stockfish score/CPL, options, hashes, and timestamps, while keeping stdout protocol-clean.

- [ ] Add failing extract-only tests for mainline SAN PGN parsing, comments/variations, castling, promotion, and FEN/ply records.
- [ ] Run the Python extraction test RED when the requested behavior is absent.
- [ ] Implement standard-library/`python-chess` tooling only outside the engine, with explicit engine IDs and option capture; keep reports outside the repository by default.
- [ ] Verify debug logs are opt-in, executable-relative when unspecified, rotating at 8 MiB with three backups, and never emitted on stdout or ordinary stderr.
- [ ] Add legal `UCI_ShowWDL` info formatting and profile counters for nodes/qnodes/seldepth/TT/pruning/tbhits where supported.
- [ ] Document En Croissant setup, book placement beside the launched executable, recommended options, analysis/tutor/MultiPV use, and Stockfish comparison methodology.
- [ ] Run extraction, debug, UCI, and documentation smoke checks.

### Task 8: Full release validation and review

**Files:**
- Modify: `.github/workflows/windows.yml`, `README.md`, release scripts/package manifests if present.
- Create: `.superpowers/sdd/2026-09-05-en-croissant-intelligence/task-8-report.md`.

**Interfaces:**
- Consumes all prior tasks and the accepted branch baseline.
- Produces verified Debug/Release artifacts, CTest/process/benchmark evidence, and a release checklist for En Croissant.

- [ ] Configure and build fresh Visual Studio x64 Debug and Release trees with C++26.
- [ ] Run the full CTest suite, tactical suite at threads 1/2/4, UCI transcript, book safety, tablebase lifecycle, debug rotation, and deterministic benchmark checks.
- [ ] Run En Croissant-style multi-ply process play with book enabled and disabled; assert legal moves and one `bestmove` per search.
- [ ] Run Stockfish 19 oracle/match validation only when PGNs and executable paths are available; report measured CPL rather than claiming Elo.
- [ ] Run a final whole-branch review package and address Critical/Important findings before completion.
- [ ] Record remaining deferred items and the exact `book.bin` placement guidance in the final report.

