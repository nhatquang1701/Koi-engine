# Task 2 report - legal root filtering and deterministic MultiPV

## Status

DONE_WITH_CONCERNS: the Task 2 behavior is implemented and the focused search
binary passes. The worktree also contains earlier performance/strength-roadmap
changes; those existing changes were preserved rather than discarded.

## Changed files

- `src/koi/search_service.cpp`
  - Filters generated legal root metadata against `searchmoves` by direct move
    comparison.
  - Returns an empty best move for an empty legal restriction.
  - Scores MultiPV roots with full windows, stable score/index ordering, and
    per-line `SearchInfo.multipv` values.
- `tests/koi_search_tests.cpp`
  - Adds legal/illegal root restriction, deterministic MultiPV, legal PV,
    distinct-line, and completion-result coverage.

## RED

The focused Task 2 build initially failed during implementation because the
new root metadata container does not expose the attempted vector-style
`erase` operation. The failure identified the need to construct a filtered
`MoveMetadataList` by walking the generated legal list and assigning it back.

## GREEN

Command:

```text
$env:VSCMD_SKIP_SENDTELEMETRY='1'; cmd.exe /d /c 'call VsDevCmd.bat -arch=x64 && cmake --build out\\current-debug --target koi_search_tests && out\\current-debug\\koi_search_tests.exe'
```

Result: exit code 0; all 30 search tests passed, including root filtering,
deterministic MultiPV, threaded parity, global node accounting, cancellation,
and transposition-table concurrency.

Additional check:

```text
git diff --check
```

Result: no whitespace errors.

## Concerns

The Task 2 implementation is currently in a worktree containing other
uncommitted roadmap changes in the same production/test files. A later
checkpoint should preserve those changes when creating focused commits.

## Round 1 fix

### Status

DONE_WITH_CONCERNS: addressed the Terra review findings without resetting the
pre-existing roadmap work in the checkout.

### Changed files

- `src/koi/search_service.cpp`
  - Carries each root line's generated legal-root index through TT/root move
    ordering and uses it only to break equal-score MultiPV ties.
  - Sends `Threads=1, MultiPV=1` searches through the existing reference path
    even when `searchmoves` is present, while restricting that path's root move
    list to the filtered legal moves.
- `tests/koi_search_tests.cpp`
  - Adds a real `Threads=2, MultiPV=3` equal-score regression that verifies
    generated-order tie handling, consecutive ranks, distinct legal roots, and
    rank-one completion parity.
- `.superpowers/sdd/2026-08-31-uci-lucas-compatibility/task-2-report.md`
  - Records this fix round.

### RED

Command:

```text
cmake --build out\debug-vs --config Debug --target koi_search_tests
$env:KOI_TEST_FILTER='threaded multipv original root ties'
.\out\debug-vs\koi_search_tests.exe
```

Output:

```text
[8/8] Linking CXX executable koi_search_tests.exe
FAIL threaded multipv original root ties: equal-score threaded MultiPV must retain the earliest generated legal root move
```

### GREEN

Command:

```text
cmake --build out\debug-vs --config Debug --target koi_search_tests
ctest --test-dir out\debug-vs -C Debug -R koi_search_tests --output-on-failure
git diff --check
```

Output:

```text
PASS threaded multipv original root ties
Test project C:/Users/ntATh/AI test/Koi engine/out/debug-vs
    Start 4: koi_search_tests
1/1 Test #4: koi_search_tests .................   Passed   13.26 sec

100% tests passed, 0 tests failed out of 1
Total Test time (real) =  13.27 sec

git diff --check: exit code 0; no whitespace errors.
```

### Concerns

`src/koi/search_service.cpp` and `tests/koi_search_tests.cpp` already contain
uncommitted roadmap changes in addition to Task 2 work. The focused commit is
limited to the two Task 2 paths and this report, but their existing mixed
contents prevent a perfectly hunk-isolated commit without reconstructing or
discarding user changes. All unrelated paths remain untouched.
