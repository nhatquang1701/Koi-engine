# Koi Engine Lucas Chess UCI Compatibility Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make Koi usable by Lucas Chess for normal play, analysis, tutor/MultiPV workflows, and ponder commands while preserving legal deterministic search and clean UCI output.

**Architecture:** Keep `UciController` as the only protocol-output owner and `SearchService` as the asynchronous search owner. Extend the value types crossing that boundary with parsed root restrictions, ponder state, MultiPV options, and per-line search information; retain the existing `Threads=1`/`MultiPV=1` search path as the reference implementation. Ponderhit is implemented as a safe stop-and-restart transition using a snapshot of the ponder root and limits.

**Tech Stack:** C++26, CMake 3.31+, Ninja, Windows x64, vendored `Disservin/chess-library`, PowerShell process tests, and CTest.

**Spec:** `docs/superpowers/specs/2026-08-31-uci-lucas-compatibility-design.md`

## Global Constraints

- Build remains Windows x64 and requires C++26-capable MSVC in the documented configuration.
- Standard chess only; Chess960, variants, NNUE, tablebases, and Elo limiting remain unadvertised and unsupported.
- `Threads=1` and `MultiPV=1` remain the deterministic reference behavior.
- Only the controller emits UCI output; workers emit no stdout and callbacks are generation-filtered.
- Normal, stopped, and ponderhit-resumed searches emit exactly one final `bestmove`; replaced or quit searches emit none.
- `go infinite` and `go ponder` remain cancellable; time and node boundaries do not end a ponder search before `stop` or `ponderhit`.
- Invalid options, malformed `go` fields, invalid FENs, and illegal moves never crash or hang the process.
- Existing `Hash`, `Threads`, `Speed`, `RandomSeed`, and `Clear Hash` semantics and ranges remain unchanged.

---

### Task 1: Extend protocol value types and parse Lucas root restrictions

**Files:**
- Modify: `src/koi/search_types.hpp`
- Modify: `src/koi/uci_controller.cpp` in `uci::parse_go_limits`
- Test: `tests/uci_controller_tests.cpp`

**Interfaces:**
- Produces `SearchLimits::ponder`, `SearchLimits::search_moves_specified`, and `SearchLimits::search_moves` (`std::vector<Move>`).
- Produces `SearchInfo::multipv` (`int`, default `1`).
- Keeps `SearchOptions` compatible and adds `multi_pv` (`std::size_t`, default `1`) and `analyse_mode` (`bool`, default `false`).

- [ ] **Step 1: Write failing parser and info-shape tests.** Add tests that call `koi::uci::parse_go_limits` with:

```cpp
const auto limits = koi::uci::parse_go_limits(
    "ponder searchmoves e2e4 g1f3 depth 5");
require(limits.ponder, "ponder must be parsed");
require(limits.search_moves_specified && limits.search_moves.size() == 2,
        "searchmoves must preserve both coordinate moves");
require(limits.depth == 5, "searchmoves must not consume a following depth field");
```

Also parse `"searchmoves a2a3"` and assert the restriction flag is true, and parse an absent `searchmoves` field and assert the flag is false. Construct a `SearchInfo` without explicit fields and assert `multipv == 1`.

- [ ] **Step 2: Run the focused test to verify it fails for the missing fields/parser behavior.**

Run from the configured build directory:

```powershell
cmake --build out\debug-vs --config Debug --target uci_controller_tests
ctest --test-dir out\debug-vs -C Debug -R uci_controller_tests --output-on-failure
```

Expected: compilation fails because the new fields are not present, or the new parser assertions fail before implementation.

- [ ] **Step 3: Add the value fields and parser logic.** In `SearchLimits`, add:

```cpp
bool ponder = false;
bool search_moves_specified = false;
std::vector<Move> search_moves;
```

In `SearchInfo`, add `int multipv = 1` after the existing public fields so old aggregate construction remains source-compatible. In `SearchOptions`, add `std::size_t multi_pv = 1` and `bool analyse_mode = false`.

In `parse_go_limits`, recognize `ponder`. On `searchmoves`, set `search_moves_specified = true`, parse consecutive coordinate tokens using `Move::parse_uci`, append valid moves, and stop at the first non-move token so later `depth`, `nodes`, `movetime`, or clock fields continue to parse. Keep malformed tokens defensive and retain the existing depth-1 fallback when no ordinary limit is present.

- [ ] **Step 4: Run the focused tests and existing controller tests.**

Run:

```powershell
cmake --build out\debug-vs --config Debug --target uci_controller_tests
ctest --test-dir out\debug-vs -C Debug -R uci_controller_tests --output-on-failure
```

Expected: all controller tests, including the new parser tests, pass; no unrelated handshake behavior has been changed yet.

- [ ] **Step 5: Commit the isolated value/parser change.**

```powershell
git add src/koi/search_types.hpp src/koi/uci_controller.cpp tests/uci_controller_tests.cpp
git commit -m "feat: parse Lucas Chess root search controls"
```

### Task 2: Filter legal root moves and implement deterministic MultiPV lines

**Files:**
- Modify: `src/koi/search_service.cpp`
- Modify: `src/koi/search_service.hpp` only if a private helper declaration is needed; keep public `start` unchanged
- Test: `tests/koi_search_tests.cpp`
- Test: `tests/uci_controller_tests.cpp` for service-independent output parsing helpers if needed

**Interfaces:**
- Consumes `SearchLimits::search_moves_specified/search_moves` and `SearchOptions::multi_pv`.
- Emits `SearchInfo` callbacks with `multipv` values `1..N`.
- Produces `SearchResult.best_move` from MultiPV line 1 and `std::nullopt` when a restricted root has no legal move.

- [ ] **Step 1: Write failing root-filter and MultiPV tests.** Add service tests with real `GameState` and `ClassicalEvaluator` objects:

```cpp
SearchLimits restricted;
restricted.depth = 2;
restricted.search_moves_specified = true;
restricted.search_moves = {*koi::Move::parse_uci("e2e4")};
auto restricted_result = run_search(koi::GameState::startpos(), restricted);
require(restricted_result.best_move.has_value() &&
            restricted_result.best_move->uci() == "e2e4",
        "a single legal searchmoves entry must be selected");

restricted.search_moves = {*koi::Move::parse_uci("a1a1")};
auto empty_result = run_search(koi::GameState::startpos(), restricted);
require(!empty_result.best_move.has_value(),
        "an entirely illegal searchmoves list must produce no best move");
```

Add a callback test for `SearchOptions{.multi_pv = 3}` at fixed depth 2. Collect the final-depth `SearchInfo` events, assert there are at most three, their `multipv` values are consecutive starting at one, their first moves are distinct and legal in the root, and the completion result matches line 1. Run it twice and assert the serialized `(multipv, score_cp, pv)` data is identical.

- [ ] **Step 2: Run the focused search tests and confirm the new tests fail.**

```powershell
cmake --build out\debug-vs --config Debug --target koi_search_tests
ctest --test-dir out\debug-vs -C Debug -R koi_search_tests --output-on-failure
```

Expected: the restricted search currently searches an unlisted move, and no MultiPV events are emitted.

- [ ] **Step 3: Implement root filtering before any root evaluation.** After `root.legal_moves_with_metadata(legal_moves)`, if `search_moves_specified` is true, build a new `MoveMetadataList` by walking the generated legal moves in their existing order and retaining a move when it equals any requested `Move`. Assign the filtered list back. Do not use UCI strings for comparison. Keep duplicates in the request from duplicating root lines.

When the resulting list is empty, finish with `bestmove 0000` and do not fabricate a legal move. Preserve the existing terminal-root handling and score conventions.

- [ ] **Step 4: Preserve the existing single-PV path and add sequential MultiPV root scoring.** Keep the current `options.threads == 1 && options.multi_pv == 1` branch behavior unchanged except for the filtered root list and `info.multipv = 1`. Add a separate branch for `multi_pv > 1` that, for each completed iterative-deepening depth, searches each filtered root move with a full root window, records `RootLine{score, pv}`, and sorts line indices by descending score with the original legal-root index as the stable tie-break. The first sorted line becomes `SearchResult`; emit one info event per selected line, up to `min(multi_pv, legal_root_count)`, with `multipv` set to its one-based rank.

Use the existing fixed-capacity `PrincipalVariation`; only convert selected lines to `std::vector<Move>` while constructing the callback object. Abort the iteration if `SearchContext::aborted` becomes true, retaining the last fully completed iteration as the final result.

- [ ] **Step 5: Extend root-parallel handling for MultiPV without changing the `Threads=1` reference.** In the existing threaded branch, pass a full window for `multi_pv > 1` so each line has a complete score, then apply the same stable score/index ordering and emit up to N lines. For `multi_pv == 1`, retain the current root ordering, aspiration windows, and earliest-index tie break. Aggregate stats once per iteration and ensure workers never call the event sink.

- [ ] **Step 6: Run the focused and regression search tests.**

```powershell
cmake --build out\debug-vs --config Debug --target koi_search_tests
ctest --test-dir out\debug-vs -C Debug -R "koi_search_tests|static_exchange_tests|koi_rules_tests" --output-on-failure
```

Expected: root restrictions, deterministic MultiPV, legal PVs, existing tactical tests, node limits, and threaded reference tests all pass.

- [ ] **Step 7: Commit the search behavior.**

```powershell
git add src/koi/search_service.cpp tests/koi_search_tests.cpp
git commit -m "feat: add legal root filtering and multipv search"
```

### Task 3: Advertise Lucas options and wire controller state

**Files:**
- Modify: `src/koi/uci_controller.hpp`
- Modify: `src/koi/uci_controller.cpp`
- Modify: `tests/uci_controller_tests.cpp`

**Interfaces:**
- Controller stores `bool analyse_mode_`, `std::size_t multi_pv_`, and `bool ponder_enabled_`.
- `handle_setoption` accepts `UCI_AnalyseMode`, `MultiPV`, and `Ponder`; unknown options remain quiet.
- `write_search_info` emits `multipv <N>` in every valid info line.

- [ ] **Step 1: Write failing handshake, option, and output-format tests.** Update the exact handshake expectation to include, in this order after the existing speed option:

```text
option name UCI_AnalyseMode type check default false
option name MultiPV type spin default 1 min 1 max 16
option name Ponder type check default false
```

Add a controller transcript that sets `UCI_AnalyseMode true`, `MultiPV 3`, `Ponder true`, runs a depth-limited search, and asserts every `info` line contains `multipv`, the values are valid, and the final output has exactly one legal `bestmove`. Add invalid values (`MultiPV 0`, `MultiPV 17`, `Ponder maybe`) and assert the process remains usable. Update `is_valid_search_info` to parse `multipv` between `seldepth` and `score`.

- [ ] **Step 2: Run controller tests and observe the expected handshake/format failures.**

```powershell
cmake --build out\debug-vs --config Debug --target uci_controller_tests
ctest --test-dir out\debug-vs -C Debug -R uci_controller_tests --output-on-failure
```

- [ ] **Step 3: Add controller fields and option parsing.** Add the three state fields to `UciController`, initialize defaults, and add handshake lines exactly as shown. Parse booleans case-insensitively for `true` and `false`; stop and suppress an active search before applying `MultiPV` or `Ponder`, and stop/suppress before applying `UCI_AnalyseMode` when it changes. Accept values only in `MultiPV` range `1..16`; ignore invalid values without changing the current setting.

At `go`, snapshot `threads_`, `speed_percent_`, `multi_pv_`, and `analyse_mode_` into `SearchOptions`, preserving the existing Hash behavior. Set `SearchInfo.multipv` at the search-service event boundary and write:

```text
info depth <d> seldepth <sd> multipv <n> score <cp|mate> <v> nodes <nodes> nps <nps> time <ms> pv <moves>
```

- [ ] **Step 4: Run all controller regression tests.**

```powershell
cmake --build out\debug-vs --config Debug --target uci_controller_tests
ctest --test-dir out\debug-vs -C Debug -R uci_controller_tests --output-on-failure
```

Expected: handshake, valid/invalid option, analysis, and protocol-clean tests pass.

- [ ] **Step 5: Commit controller option support.**

```powershell
git add src/koi/uci_controller.hpp src/koi/uci_controller.cpp tests/uci_controller_tests.cpp
git commit -m "feat: advertise Lucas Chess analysis options"
```

### Task 4: Implement ponder lifecycle and ponderhit restart

**Files:**
- Modify: `src/koi/uci_controller.hpp`
- Modify: `src/koi/uci_controller.cpp`
- Modify: `src/koi/search_service.cpp`
- Modify: `tests/koi_search_tests.cpp`
- Modify: `tests/uci_controller_tests.cpp`

**Interfaces:**
- `SearchLimits::ponder` controls speculative search lifetime.
- Controller adds `handle_ponderhit()`, `start_search(GameState, SearchLimits)`, and snapshots `std::optional<GameState> ponder_root_`, `std::optional<SearchLimits> ponder_limits_`, plus `bool active_ponder_`.

- [ ] **Step 1: Write failing service and controller lifecycle tests.** Add a service test:

```cpp
SearchLimits limits;
limits.depth = 1;
limits.ponder = true;
std::atomic_int completions = 0;
SearchEventSink sink;
sink.on_complete = [&](const SearchResult&) { completions.fetch_add(1); };
auto handle = service.start(GameState::startpos(), limits, sink);
std::this_thread::sleep_for(std::chrono::milliseconds(50));
require(handle.running(), "ponder must not finish at its requested depth");
handle.stop();
handle.wait();
require(completions == 1, "stopping ponder must complete exactly once");
```

Add a controller transcript `position startpos`, `go ponder depth 2`, `ponderhit`, `isready`, `quit`; assert one final legal bestmove and no duplicate. Add a `go ponder`, `stop`, `quit` transcript with the same assertion. Add a `go ponder searchmoves e2e4`, `stop` case and assert the only result is `e2e4`.

- [ ] **Step 2: Run focused lifecycle tests and confirm they fail.**

```powershell
cmake --build out\debug-vs --config Debug --target koi_search_tests uci_controller_tests
ctest --test-dir out\debug-vs -C Debug -R "koi_search_tests|uci_controller_tests" --output-on-failure
```

Expected: `go ponder` currently completes at the requested depth, `ponderhit` is ignored, and controller options cannot restart a ponder search.

- [ ] **Step 3: Make the search service hold ponder searches open.** In `TimeManager`, treat `limits.ponder` like `limits.infinite` for time and node termination. In both search loops, use `limits.infinite || limits.ponder` as the unbounded-loop condition so depth, nodes, movetime, and clock limits do not complete speculative work. Continue checking `stop_requested` and `SearchContext::interrupted` at every node. Keep the last fully completed iteration in `SearchResult`; on `stop`, emit it once. A valid ponder root therefore cannot emit a completion until cancellation.

- [ ] **Step 4: Add controller ponder state and restart behavior.** Parse `go ponder` into `SearchLimits.ponder`. Refactor the existing launch code into `start_search(GameState root, SearchLimits limits)` so both normal go and ponderhit use the same callback/generation setup. On `go ponder`, snapshot `position_` and the complete limits, set `active_ponder_`, and start the search. On `ponderhit`, if an active ponder exists, copy the stored root/limits, clear `ponder`, increment generation and stop/join/suppress the speculative handle, then start the copied root with normal limits. On `stop`, join without suppressing so the ponder's current result becomes the one final `bestmove`. On `position`, `ucinewgame`, option changes, `quit`, and EOF, clear the ponder snapshot while suppressing the replaced search.

Do not apply a position update or option update to the speculative root. If `ponderhit` arrives with no active ponder, ignore it quietly. Ensure generation increments before joining a suppressed worker, preventing late info or completion output.

- [ ] **Step 5: Run lifecycle and regression tests.**

```powershell
cmake --build out\debug-vs --config Debug --target koi_search_tests uci_controller_tests
ctest --test-dir out\debug-vs -C Debug -R "koi_search_tests|uci_controller_tests" --output-on-failure
```

Expected: ponder stop and ponderhit each produce one legal result, replaced searches produce none, and existing infinite cancellation remains prompt.

- [ ] **Step 6: Commit ponder support.**

```powershell
git add src/koi/time_manager.cpp src/koi/time_manager.hpp src/koi/search_service.cpp src/koi/uci_controller.hpp src/koi/uci_controller.cpp tests/koi_search_tests.cpp tests/uci_controller_tests.cpp
git commit -m "feat: support Lucas Chess ponder lifecycle"
```

### Task 5: Add Lucas-style process coverage and documentation

**Files:**
- Modify: `tests/uci_process_test.ps1`
- Modify: `tests/uci_match_process_test.ps1` only if its parser must accept MultiPV fields
- Modify: `CMakeLists.txt` if a separate process test is added
- Modify: `README.md`

**Interfaces:**
- Process tests accept `multipv` in all `info` lines and recognize the three new handshake options.
- README documents normal play, analysis, tutor/MultiPV, and ponder behavior with the existing Hash/Threads/Speed settings.

- [ ] **Step 1: Write the failing interactive process assertions.** Extend the PowerShell session test to:

```powershell
Send-UciCommand $session 'setoption name UCI_AnalyseMode value true'
Send-UciCommand $session 'setoption name MultiPV value 3'
Send-UciCommand $session 'setoption name Ponder value true'
Send-UciCommand $session 'position startpos'
Send-UciCommand $session 'go ponder depth 2'
Send-UciCommand $session 'ponderhit'
```

Read until one `bestmove`, assert every received info line matches the updated grammar and at least one line has `multipv 2`, then run `go ponder searchmoves e2e4`, send `stop`, and assert `bestmove e2e4`. Keep the existing normal game, infinite, replacement, EOF, and terminal `0000` scenarios.

- [ ] **Step 2: Run the process test before implementation and record the expected failure.**

```powershell
ctest --test-dir out\debug-vs -C Debug -R koi_engine_process --output-on-failure
```

Expected: the old handshake or process grammar rejects the new options/fields, or ponder does not produce the expected lifecycle.

- [ ] **Step 3: Update process grammar and the existing CTest registration.** Add the three exact handshake lines, parse `multipv [1-9][0-6]?` between `seldepth` and `score`, and ensure the process test reads until the single final bestmove without treating intermediate MultiPV info as a duplicate completion. Keep the existing 30-second process-test timeout and temporary-output cleanup behavior.

- [ ] **Step 4: Update README.** Document these commands and semantics:

```text
setoption name UCI_AnalyseMode value true
setoption name MultiPV value 3
setoption name Ponder value true
go ponder wtime 60000 btime 60000
ponderhit
```

Explain that MultiPV emits one `info` line per principal variation, `searchmoves` restricts legal root moves, `go infinite` is analysis mode, and v1 ponderhit safely restarts from the saved root rather than retaining speculative work. Keep the recommended Lucas settings:

```text
setoption name Hash value 512
setoption name Threads value 4
setoption name Speed value 100
```

- [ ] **Step 5: Run process and documentation checks.**

```powershell
cmake --build out\debug-vs --config Debug
ctest --test-dir out\debug-vs -C Debug -R "koi_engine_process|koi_uci_match_process|koi_benchmark_process" --output-on-failure
git diff --check
```

- [ ] **Step 6: Commit Lucas compatibility coverage and docs.**

```powershell
git add CMakeLists.txt README.md tests/uci_process_test.ps1 tests/uci_match_process_test.ps1
git commit -m "test: cover Lucas Chess UCI workflows"
```

### Task 6: Full verification and manual Lucas acceptance

**Files:**
- No production files; inspect the full diff and generated build artifacts.

- [ ] **Step 1: Build both configurations with the documented Windows x64 C++26 toolchain.**

```powershell
cmake -S . -B out\debug-vs -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=cl
cmake --build out\debug-vs --config Debug
cmake -S . -B out\release-vs -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=cl
cmake --build out\release-vs --config Release
```

- [ ] **Step 2: Run the complete Debug and Release CTest suites.**

```powershell
ctest --test-dir out\debug-vs -C Debug --output-on-failure
ctest --test-dir out\release-vs -C Release --output-on-failure
```

The suites must include rules/perft, evaluator/search, UCI controller, process, benchmark, and Lucas-style match tests with zero failures.

- [ ] **Step 3: Run protocol smoke and compatibility transcripts against the Release executable.** Confirm stdout contains only `id`, `option`, `uciok`, `readyok`, `info`, and `bestmove` protocol responses; confirm stderr is empty for valid sessions; confirm `bestmove 0000` for checkmate/stalemate and `bestmove e2e4` for `searchmoves e2e4`.

- [ ] **Step 4: Inspect requirements against evidence.** Check that all advertised options match actual behavior, `MultiPV` PVs are legal/distinct, `ponderhit` emits one result, replacement commands suppress stale output, and default single-thread single-PV searches remain deterministic.

- [ ] **Step 5: Manually add `out\release-vs\koi-engine.exe` to Lucas Chess.** Run a normal game, open Lucas analysis with MultiPV 3, invoke tutor analysis, enable Ponder and start/stop a short game, and verify that Lucas remains responsive and receives no duplicate or malformed responses. Record the engine path and settings used in the final handoff.

- [ ] **Step 6: Run `git diff --check` and report only claims supported by the fresh build/test output.**
