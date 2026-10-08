# Koi Engine Data-Driven Elo Intelligence Sprint

## Goal

Improve Koi's practical 1+0 strength using Stockfish position-oracle data while preserving C++26 Windows x64 builds, Lucas Chess UCI compatibility, deterministic `Threads=1` behavior, and the existing `Threads`/`Speed` controls.

## Global constraints

- Use only standard chess and keep stdout protocol-clean.
- `Threads=1` is the deterministic reference path; `Threads=4` is the optimization target.
- Stockfish is the reference oracle at 250 ms per position; primary metric is mean CPL, with 95th-percentile CPL, blunders, completed depth, and NPS as secondary metrics.
- Fixed-depth threaded searches must return the same move and score as the single-thread reference; fixed-depth median time may not regress by more than 5%.
- `Speed` scales only time-based limits. Explicit depth, nodes, and infinite limits are unchanged.
- Keep existing quiescence checks, SEE, delta pruning, check extensions, TT rules, and stable tie-breaking unless a regression test proves a narrowly scoped correction is required.
- `OwnBook=true`, `BookFile=book.bin`, and `BookDepth=16` remain defaults. The licensed book is user-supplied and not committed.
- `BookRandom=false` is the deterministic default; book selection must be highest-weight legal move with coordinate-order tie-breaking. `BookRandom=true` retains weighted random selection.
- Missing or malformed books silently fall back to search. Analysis, MultiPV, ponder, infinite, and `searchmoves` bypass the book.
- No Python dependency is added to the engine; Python tooling may use Python 3 and `python-chess`.
- Route all subagents to `gpt-5.6-luna` only; never use Sol.

## Task 1: Stockfish position oracle and measurement tooling

Create `tools/elo_oracle.py` and, when practical, an extraction-focused Python test. Parse standard SAN PGN mainlines with `python-chess`, support `--extract-only`, record FEN/ply/side/actual move/SAN, run Koi with book disabled and Stockfish at the requested time, calculate separate Koi-suggested and actual-game CPL values with mate scores normalized to +/-100000 cp, and write reproducibility metadata (engine IDs/versions/options, PGN hash, timestamps, limits). Add a separate licensed-book audit using `OwnBook=true`, `BookFile`, `BookDepth=16`, `BookRandom=false`, recording book hits separately from search metrics. Do not require any supplied external executable or book for extraction tests.

## Task 2: Tactical search reliability

Modify `src/koi/search_service.cpp` and add focused regression tests in `tests/koi_search_tests.cpp` and `tests/search_ordering_tests.cpp`. Ensure regular search metadata computes `gives_check`; keep checks, captures, promotions, TT moves, and killers out of LMR; re-search reduced moves at full depth whenever the reduced score exceeds alpha, including fail-high scores; and disable null-move pruning when `position_features().game_phase < 8`. Preserve existing tactical safeguards and deterministic tie-breaking. Cover checking-order priority, LMR verification, sparse-endgame null safety, mate distance, and existing fixtures.

## Task 3: Authoritative deterministic threaded root search

Modify the threaded path in `src/koi/search_service.cpp` and its tests. For `Threads>1` single-PV searches, use parallel root lines as authoritative, search every root move with a full [-infinity,+infinity] window, skip root aspiration and the redundant serial reference search, and select the best score with stable root-order tie-breaking. Preserve shared cancellation, global nodes, TT safety, discarded partial iterations, one controller-owned output path, exactly one completion callback, and the unchanged `Threads=1` reference path. Extend `koi-bench` only if needed for `--threads N --speed 1-100 [--timed]`.

## Task 4: Strength-first deterministic book option

Modify `src/koi/opening_book.hpp`, `src/koi/opening_book.cpp`, `src/koi/uci_controller.hpp`, and `src/koi/uci_controller.cpp`; add tests. Advertise `option name BookRandom type check default false`. When false, choose the highest-weight legal Polyglot move and break equal weights by deterministic coordinate ordering. When true, use weighted random selection; seed zero is runtime-random only in this mode, while nonzero seeds remain repeatable. Preserve existing book defaults, fallbacks, and bypass rules, and stop/join before applying option changes.

## Task 5: Verification and documentation

Run extraction tests, full Debug/Release CTest, tactical suites at Threads 1/2/4, fixed-depth parity/timing checks, UCI handshake/process transcripts, and the oracle/book audits when the user supplies PGN, Stockfish, and licensed-book paths. Keep reports outside the repository. Update README with oracle usage, book audit usage, and the recommended Lucas settings. Do not claim an Elo increase without before/after CPL evidence.
