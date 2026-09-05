# Task 4 Report - Optional Syzygy Tablebase Support

## Scope and baseline

Implemented Task 4 from `task-4-brief.md` on baseline HEAD `7c15bc22028abf469e8377b3de5d8e2356c6db04`. The change keeps `chess.hpp` private to `GameState`, uses only Koi-owned tablebase metadata at the adapter boundary, preserves the licensed opening-book defaults and existing UCI/search lifecycle, and does not bundle tablebase data files.

The Fathom source is the pinned jdart1/Fathom snapshot at commit `c9c6fef0dddc05d2e242c183acf5833149ab676d`. Its MIT license is preserved in `third_party/fathom/LICENSE`; the source files retain their upstream license notices.

## TDD evidence

The RED phase added `tests/syzygy_tablebase_tests.cpp`, registered it in CMake, extended UCI handshake expectations, and added invalid-option/fallback coverage before the production interface existed.

Focused RED commands and results:

```text
cmake --build out/task4-red-vs --target syzygy_tablebase_tests uci_controller_tests -- -j2
FAILED: syzygy_tablebase_tests.cpp: fatal error C1083: koi/syzygy_tablebase.hpp was not found

cmake --build out/task4-red-vs --target uci_controller_tests -- -j2
out/task4-red-vs/uci_controller_tests.exe
FAIL uci handshake and options: uci response must advertise the identity, hash, thread, speed, and clear-hash options
All unrelated UCI tests passed.
```

The GREEN cycle then implemented the smallest Koi snapshot/adapter/search/controller path and corrected one invalid test FEN. Focused Debug results were:

```text
out/task4-green-vs/syzygy_tablebase_tests.exe: PASS syzygy tablebase tests
out/task4-green-vs/uci_controller_tests.exe: 44/44 PASS
out/task4-green-vs/koi_core_tests.exe: 36/36 PASS
out/task4-green-vs/koi_rules_tests.exe: 35/35 PASS
out/task4-green-vs/koi_search_tests.exe: 69/69 PASS
```

## Implementation

- Added `TablebaseSnapshot` and `GameState::tablebase_snapshot()` with Koi-owned white/black piece bitboards, castling flags, en-passant square, side to move, and halfmove clock. `chess.hpp` remains included only by `game_state.cpp`.
- Added `SyzygyTablebase` as an opaque Koi-owned adapter. Empty, absent, malformed, unreadable, zero-limit, over-five-piece, and castling positions safely decline probing. Probe limit is clamped to `0..5`; probe depth is clamped to `1..100`.
- Converted Koi bitboards and moves to/from Fathom. WDL calls use a process-wide lock for safe access to Fathom’s global state; root WDL calls use the same lock, satisfying Fathom’s non-thread-safe root requirement. Successful adapter calls count `hits()`.
- Added root tablebase filtering before ordinary search only for single-PV, non-analysis, non-ponder, non-`searchmoves` searches. It retains legal move filtering and deterministic root order, returns Koi mate/draw scores, and reports `tbhits` only on tablebase info lines.
- Added exact UCI options and safe parsing: `SyzygyPath`, `SyzygyProbeDepth` (`1..100`, default `1`), `SyzygyProbeLimit` (`0..5`, default `5`), and `Syzygy50MoveRule` (default `true`). Option changes stop/join active searches before rebuilding the adapter.
- Updated the process harness handshake and `info` grammar for the new options and optional `tbhits` field.

## Verification

Fresh Visual Studio x64 Ninja builds completed successfully:

```text
Debug:  cmake --build out/task4-green-vs -- -j2       PASS
Release: cmake --build out/task4-release-vs -- -j2    PASS
```

Full CTest results:

```text
Debug:   17/17 tests passed, 0 failed
Release: 17/17 tests passed, 0 failed
```

The direct focused Debug core, rules, search, and UCI runs also passed. The Koi-authored diff passes `git diff --check`; upstream Fathom files retain their original trailing whitespace and license text. No `.rtbw`, `.rtbz`, `.tbw`, or `.tbz` fixture exists in the repository, so fixture-dependent real WDL/root assertions were not run; all tests pass without external tablebase files as required.

## Self-review and concerns

- Existing book eligibility and bypass rules remain in the controller before search: normal eligible book hits still return directly; analysis, MultiPV, ponder, and `searchmoves` continue to bypass the book and tablebase root shortcut.
- Existing cancellation, generation suppression, clean stdout/stderr, legal move filtering, and deterministic default `Threads=1` paths passed the full regression matrix.
- Fathom is a process-global tablebase implementation. The adapter serializes all access and controller option changes stop active searches before replacement; concurrent WDL calls on one adapter are safe. A real fixture should be used in a future verification pass to validate move ranking and 50-move outcomes end-to-end.
- The unrelated generated `tests/__pycache__/` directory remains untouched.

## Commit

The implementation and this report are committed together with a focused Task 4 subject after the verification above.
