# Task 2 Report: Search State and Optional Tablebase Lifecycle

## Scope

Implemented only Task 2:

- quiet-history updates retain the side that made the move across `unmake_move()`;
- Fathom ownership is process-global, mutex-guarded, and reference-counted;
- an active Fathom path is shared by matching tablebase instances;
- a second conflicting path falls back disabled without reinitializing or invalidating the active owner;
- the final enabled instance releases Fathom resources;
- missing, malformed, empty, and readable tablebase paths retain optional fallback behavior.

No UCI, README, opening-book, or search-ordering implementation files were changed.

## Tests and TDD evidence

Regression tests were added to `tests/koi_search_tests.cpp` and
`tests/syzygy_tablebase_tests.cpp`.

The RED run was performed before production changes with:

```text
& .\out\task2-tdd\koi_search_tests.exe
& .\out\task2-tdd\syzygy_tablebase_tests.exe
```

Expected failures were observed:

- `quiet history moving side`: the implementation still used `state.side_to_move()` after `unmake_move()`;
- `clearing the second user must not invalidate the first user`: each instance still called process-global `tb_free()` independently.

After the production changes, the focused GREEN verification was:

```text
ctest --test-dir out/task2-tdd -R "^(koi_search_tests|syzygy_tablebase_tests)$" --output-on-failure
```

Result:

```text
100% tests passed, 0 tests failed out of 2
```

## Fixture limitation

The repository does not bundle Syzygy tablebase data. The lifecycle tests create
a temporary `KQvK.rtbw` file with the minimal size Fathom uses to register a
table, allowing the tests to exercise acquisition, shared ownership, and final
release without requiring licensed tablebase files. The fixture is not valid
probe data, so the lifecycle assertions check Fathom registration state rather
than WDL or root-move correctness. Missing, malformed, and empty paths are
tested as disabled fallbacks and do not require any tablebase data.

## Changed files

- `src/koi/search_service.cpp`
- `src/koi/syzygy_tablebase.hpp`
- `src/koi/syzygy_tablebase.cpp`
- `tests/koi_search_tests.cpp`
- `tests/syzygy_tablebase_tests.cpp`
- `.superpowers/sdd/2026-09-05-en-croissant-intelligence/task-2-report.md`
