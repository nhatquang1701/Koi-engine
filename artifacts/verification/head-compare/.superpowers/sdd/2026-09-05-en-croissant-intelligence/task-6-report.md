# Task 6 Report - Classical Evaluation Strength Pass and Optional StrengthMode

Date: 2026-09-05
Branch: `koi-engine-v1`
Base commit: `23132a5594f086461779150c993f0ebd5e823e8e`

## Scope and audit

The current classical evaluator already contained versioned tapered
middlegame/endgame parameters and implementations for material, piece-square,
mobility, pawn structure, king safety, king activity, passed pawns, tempo, and
insufficient-material handling. Existing focused tests covered symmetry,
material sanity, mobility, pawn structure, king safety, dead material, and
endgame scaling. I added a direct bishop-pair regression and did not retune
coefficients or generate a new parameter header.

`StrengthMode` was absent. The smallest safe slice adds a default-false
`SearchOptions::strength_mode` snapshot field, controller state and
case-insensitive UCI parsing, the exact handshake option, and active-search
cancellation on a changed valid value. The search worker remains neutral and
does not consume the field until a later calibrated strength-profile task.

Task 4 was concurrently editing `tests/koi_search_tests.cpp` and
`src/koi/search_service.cpp` was kept untouched. The concurrent Task 4 test
hunks were preserved and left outside this Task 6 commit. Unrelated modified
and untracked README, oracle, tool, plan, Stockfish, database, and cache paths
were also left untouched.

## TDD evidence

### RED - tests before production changes

Added the bishop-pair regression, `StrengthMode` handshake/cancellation test,
and `SearchOptions` default/snapshot assertions before production changes.

The controller target built, then the focused controller test failed as
expected because the option was not advertised and the unknown option did not
cancel the active search:

```powershell
& 'C:\msys64\ucrt64\bin\cmake.exe' --build out\task1-vs --config Debug --target uci_controller_tests --parallel
ctest --test-dir out\task1-vs -C Debug -R '^uci_controller_tests$' --output-on-failure
```

Observed result: `uci_controller_tests` failed with
`FAIL uci handshake and options` and
`FAIL Task 6 StrengthMode`; the remaining tests passed.

The search target also failed at the intended missing interface:

```powershell
& 'C:\msys64\ucrt64\bin\cmake.exe' --build out\task1-vs --config Debug --target koi_search_tests --parallel
```

Observed compiler error: `SearchOptions` had no member named `strength_mode`.

### GREEN - minimal implementation

Added only the snapshot/controller/handshake plumbing. No `search_service.cpp`
change or evaluator coefficient change was made.

```powershell
& 'C:\msys64\ucrt64\bin\cmake.exe' --build out\task1-vs --config Debug --target uci_controller_tests --parallel
& 'C:\msys64\ucrt64\bin\cmake.exe' --build out\task1-vs --config Debug --target koi_search_tests --parallel
& 'C:\msys64\ucrt64\bin\cmake.exe' --build out\task1-vs --config Debug --target koi_engine --parallel
ctest --test-dir out\task1-vs -C Debug -R '^koi_search_tests$|^uci_controller_tests$|^koi_engine_process$' --output-on-failure
```

Observed result:

```text
1/3 Test #5: uci_controller_tests .............   Passed
2/3 Test #6: koi_search_tests .................   Passed
3/3 Test #12: koi_engine_process ..............   Passed
100% tests passed, 0 tests failed out of 3
```

The focused run includes the new bishop-pair regression, the default and
configured `SearchOptions` assertions, mixed-case `StrengthMode` parsing,
active-search cancellation, and the process-level handshake contract. The
whole CTest suite was not run for this bounded slice.

## Implementation commit

`26173cdddde336849d1aaf393154a5997b4f487e` (`feat: add neutral StrengthMode option`)

Report commit: recorded separately after the implementation commit.
