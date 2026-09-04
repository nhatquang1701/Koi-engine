# Koi Elo Improvement and External Opening Book Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use `superpowers:subagent-driven-development` (recommended) or `superpowers:executing-plans` to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Improve Koi's practical Elo against available UCI opponents while adding safe default opening-book support for Lucas Chess.

**Architecture:** Preserve the existing Koi-owned rules/search boundary. Tune the single-thread tactical search and classical evaluator first, add a separate Polyglot reader and UCI integration, then improve root multithreading after strength behavior is stable.

**Tech Stack:** C++26, x64 MSVC, CMake 3.31+, Ninja, PowerShell UCI harnesses, standard-library threading and Polyglot binary parsing.

**Spec:** `docs/superpowers/specs/2026-08-31-next-strength-roadmap-design.md` plus the approved external-book decisions in this plan.

## Global Constraints

- Windows x64 and C++26 remain mandatory; use portable Release optimization without CPU-specific instructions.
- Standard chess and Lucas Chess UCI compatibility remain mandatory.
- Public Koi headers must not expose `chess.hpp` types.
- `Threads=1` remains deterministic and the reference path; `Speed=100` preserves current timing behavior.
- Stdout contains only valid UCI output and every search produces at most one completion.
- The practical Elo gate uses reproducible Koi-vs-Stockfish or another supplied UCI opponent matches at `1+0` and `5+3`; Jack is unavailable and is not a required dependency.
- Keep the existing 64-position tactical hard gate as a non-regression guard.
- Maintain at least 80% of the current timed NPS unless a documented match-strength gain justifies the exception.
- Default book settings are `OwnBook=true`, `BookFile=book.bin`, and `BookDepth=16`.
- `book.bin` is user-supplied and is not redistributed by this repository.
- Relative book paths resolve beside `koi-engine.exe`; missing books silently fall back to search.
- Bypass the book in `UCI_AnalyseMode`, `go infinite`, `go ponder`, and all `searchmoves` searches.
- If subagents are used, route only to Terra or Luna; never use Sol.
- Use TDD: each production behavior must have a failing test before implementation.

---

### Task 1: Reproducible strength measurement without Jack

**Files:**

- Modify: `tools/uci_match.ps1`
- Modify: `tests/uci_match_process_test.ps1`
- Modify: `README.md`
- Create: `tests/data/elo-openings.txt`

**Interfaces:**

- Add `-OpeningFile`, with lines formatted as `name | uci move uci move`.
- Add `-TimeControl`, accepting only `<minutes>+<increment>` such as `1+0` and `5+3`.
- Add `-KoiRandomSeed`, `-KoiOwnBook`, `-KoiBookFile`, and `-KoiBookDepth`.
- Preserve `koi-uci-match-v2`, PGN output, FEN replay validation, and all existing depth/movetime/node modes.
- Record time control, book options, `book_used`, and `book_move` in the JSON report.

**Steps:**

- [ ] Add an eight-line legal opening suite covering e4, d4, English, Scandinavian, French, Caro-Kann, Sicilian, and Queen's Gambit starts.
- [ ] Add failing process assertions for opening replay, color swapping, clock commands, Koi book options, and book markers.
- [ ] Run the focused process test and confirm failure is caused by missing behavior.
- [ ] Implement opening replay and reject illegal opening moves before a game begins.
- [ ] Implement clock-mode UCI `wtime`, `btime`, `winc`, and `binc` accounting.
- [ ] Parse `info string book move <uci> depth <ply>` without treating it as a search PV.
- [ ] Run the focused process test and the complete Release suite.
- [ ] Commit `test: add reproducible Elo match measurements`.

### Task 2: Polyglot key and book reader

**Files:**

- Create: `src/koi/opening_book.hpp`
- Create: `src/koi/opening_book.cpp`
- Modify: `src/koi/game_state.hpp`
- Modify: `src/koi/game_state.cpp`
- Modify: `CMakeLists.txt`
- Create: `tests/opening_book_tests.cpp`

**Interfaces:**

```cpp
struct BookChoice {
    Move move;
    std::uint16_t weight = 0;
    std::uint32_t learn = 0;
};

class OpeningBook {
public:
    explicit OpeningBook(std::filesystem::path executable_directory = {});
    void set_file(std::filesystem::path path);
    void clear_cache() noexcept;
    [[nodiscard]] std::optional<BookChoice> choose(
        const GameState& state, std::uint32_t root_ply, bool enabled,
        std::uint8_t maximum_depth, std::uint64_t random_seed) const;
};
```

Add:

```cpp
[[nodiscard]] std::uint64_t GameState::polyglot_key() const noexcept;
[[nodiscard]] std::uint16_t GameState::fullmove_number() const noexcept;
```

**Steps:**

- [ ] Add failing tests for the published start key `0x463b96181691fc9c`, a post-e4 key, castling, en passant, promotions, weighted seeded selection, malformed records, and illegal moves.
- [ ] Run `ctest --test-dir out/elo-release -C Release -R opening_book_tests --output-on-failure` and confirm the expected missing-interface failure.
- [ ] Implement the reference Polyglot Zobrist key inside `GameState`, including pieces, side, castling, and the reference en-passant rule.
- [ ] Parse big-endian 16-byte records, reject non-multiple file sizes, index by key, and cache by canonical path plus timestamp.
- [ ] Decode Polyglot promotions and king-to-rook castling encodings, then validate every move with `GameState::is_legal()`.
- [ ] Select only positive-weight legal entries using `std::mt19937_64`; seed nonzero choices with `random_seed ^ polyglot_key()` and seed zero from runtime entropy.
- [ ] Enforce `BookDepth=0` as unlimited and return no choice for missing, malformed, empty, or unusable files.
- [ ] Run Debug and Release book tests.
- [ ] Commit `feat: add Polyglot opening book reader`.

### Task 3: Lucas Chess book defaults and tutor-safe UCI integration

**Files:**

- Modify: `src/koi/uci_controller.hpp`
- Modify: `src/koi/uci_controller.cpp`
- Modify: `src/main.cpp`
- Modify: `tests/uci_controller_tests.cpp`
- Modify: `tests/uci_process_test.ps1`
- Modify: `tools/uci_match.ps1`
- Modify: `README.md`

**Interfaces:**

Advertise exactly:

```text
option name OwnBook type check default true
option name BookFile type string default book.bin
option name BookDepth type spin default 16 min 0 max 40
```

**Steps:**

- [ ] Add failing controller/process tests for handshake, valid and invalid values, executable-relative paths, seeded choice, missing-book fallback, analysis bypass, searchmoves bypass, and exactly one bestmove.
- [ ] Run those tests and confirm failure before production changes.
- [ ] Pass the executable directory from `main.cpp` into the controller/book object.
- [ ] Stop and join active searches before applying changes to `OwnBook`, `BookFile`, or `BookDepth`.
- [ ] Attempt book selection before `SearchService` only for eligible normal-play commands.
- [ ] Emit `info string book move <uci> depth <ply>` followed by one legal `bestmove` on a hit; emit no diagnostic for fallback.
- [ ] Preserve ponder, MultiPV, analysis, `stop`, `quit`, EOF, position replacement, and stale-generation suppression.
- [ ] Update the match harness and README with book placement and search-only commands.
- [ ] Run all UCI process tests and verify valid transcripts have empty stderr.
- [ ] Commit `feat: integrate Lucas opening book defaults`.

### Task 4: Practical Elo search and evaluator tuning

**Files:**

- Modify: `src/koi/search_service.cpp`
- Modify: `src/koi/search_ordering.cpp`
- Modify: `src/koi/static_exchange.cpp`
- Modify: `src/koi/classical_evaluator.cpp`
- Modify: `src/koi/classical_evaluator.hpp`
- Modify: `tests/koi_search_tests.cpp`
- Modify: `tests/koi_strength_tests.cpp`
- Modify: `tests/search_ordering_tests.cpp`
- Create: `tests/data/evaluation-positions.txt`

**Steps:**

- [ ] Add failing safety tests for checks/evasions, poisoned captures, quiet defenses, zugzwang, mate distance, null move, LMR, evaluator symmetry, passed pawns, king safety, mobility, and endgame scaling.
- [ ] Run focused tests and verify failures are feature failures rather than test errors.
- [ ] Keep all existing tactical safeguards, then tune PVS/aspiration widths, qsearch bounds, SEE/delta pruning, null-move conditions, LMR conditions, TT replacement, killers/history, and root ordering one group at a time.
- [ ] Centralize evaluator coefficients in a private parameter block while preserving `evaluate()` and `breakdown()` signatures.
- [ ] Tune tapered PST/material values first, followed by mobility/activity, pawn structure, king safety, and endgame scaling.
- [ ] Run the 64-position hard gate after each group with book disabled.
- [ ] Run timed `koi-bench` at Threads 1, 2, and 4, recording nodes, qnodes, TT hits, pruning counts, elapsed time, and NPS.
- [ ] Run the paired Stockfish-or-supplied-opponent matrix at 1+0 and 5+3 with `OwnBook=false`; retain only changes that preserve the tactical gate and improve or preserve paired score.
- [ ] Commit `strength: tune tactical search and evaluation`.

### Task 5: Final root-parallel performance pass

**Files:**

- Modify: `src/koi/search_service.cpp`
- Modify: `src/koi/search_types.hpp`
- Modify: `tests/koi_search_tests.cpp`
- Modify: `tests/uci_controller_tests.cpp`
- Modify: `tools/koi_bench.cpp`

**Steps:**

- [ ] Add failing tests for fixed-depth Threads 1/2 parity, stable equal-score ties, global nodes, prompt timed/infinite cancellation, worker reuse, and one completion callback.
- [ ] Run the focused tests and capture the current threaded baseline.
- [ ] Reuse an internal worker pool, search the stable PV prefix first, distribute remaining root indices dynamically, and store results by original root index.
- [ ] Preserve one global node counter, one time origin, one cancellation flag, one iteration-aborted flag, and one UCI output owner.
- [ ] Discard partial iterations and join all workers before any option, position, or book mutation.
- [ ] Keep `Threads=1` on the reference path and avoid parallel overhead for fewer than two legal roots.
- [ ] Verify fixed-depth move/score equality between Threads 1 and 2.
- [ ] Require timed NPS to remain at least 80% of the reference unless the match report documents a strength gain.
- [ ] Commit `perf: improve deterministic root parallel search`.

### Task 6: Full release and Lucas verification

**Files:**

- Modify: `README.md`
- Modify: `CMakeLists.txt` only when registering new targets/tests

**Steps:**

- [ ] Configure fresh Debug and Release Ninja builds with x64 MSVC and C++26.
- [ ] Build both configurations and run every CTest target.
- [ ] Run the hard tactical gate, optional positional corpus, default/timed benchmarks, and book unit tests.
- [ ] Run UCI handshake, analysis, tutor MultiPV, ponder, book-hit, book-fallback, stop, quit, and EOF transcripts.
- [ ] Run 40 paired games per supplied opponent and time control at 1+0 and 5+3 with book disabled, then repeat with the user-supplied licensed book.
- [ ] Store JSON/PGN reports outside the repository and record opponent executable/version, options, openings, and results.
- [ ] Register the Release executable manually in Lucas Chess and play a short game with Hash 512, Threads 4, Speed 100, OwnBook true, BookFile book.bin, and BookDepth 16.
- [ ] Document measured results, book placement, fallback behavior, and known limitations.
- [ ] Commit `docs: document Elo and opening book baseline`.

## Acceptance Criteria

- The engine remains legal and Lucas-recognizable with protocol-clean stdout.
- A missing `book.bin` never prevents startup or search.
- A supplied Polyglot book produces legal weighted choices and nonzero-seed repeatability.
- Analysis/tutor searches bypass the book.
- The 64-case tactical hard gate has no regression.
- Threads 1 and 2 return identical fixed-depth moves and scores.
- No duplicate or stale `bestmove` is emitted.
- Practical paired match score against each available opponent is preserved or improved.
- Timed NPS remains within the approved 20% floor unless a documented Elo gain justifies the exception.
