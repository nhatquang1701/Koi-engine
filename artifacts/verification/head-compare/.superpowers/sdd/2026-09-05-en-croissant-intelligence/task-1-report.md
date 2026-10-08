# Task 1 Report: En Croissant UCI Compatibility Contract

## Scope

Implemented the En Croissant compatibility slice while retaining generic
standard UCI behavior:

- added a CTest-launched PowerShell process transcript for handshake,
  readiness, option changes, positions with moves, stopped search, MultiPV,
  infinite analysis, unknown-command tolerance, and clean quit;
- made accepted UCI option names case-insensitive for GUI clients that vary
  option casing;
- made En Croissant the primary README workflow and kept Lucas Chess as a
  generic UCI fallback;
- documented executable-relative licensed `book.bin` placement and recommended
  `Hash`, `Threads`, `Speed`, and deterministic book settings.

## TDD and verification

The implementer observed RED before the parser change: lowercase
`uci_analysemode` and `multipv` were ignored. The final regression test uses a
temporary Polyglot book to prove lowercase `bookfile` is accepted and that
lowercase `multipv` and `uci_analysemode` bypass book selection as intended.

Fresh Release verification:

```text
cmake --build out/task1-vs --config Release --parallel 2
ctest --test-dir out/task1-vs -C Release -R "uci_controller_tests|koi_engine_en_croissant_process" --output-on-failure
```

Result:

```text
100% tests passed, 0 tests failed out of 2
```

The En Croissant process transcript also passed alongside the Task 2 focused
search/tablebase tests in:

```text
ctest --test-dir out/task1-vs -C Release -R "koi_search_tests|syzygy_tablebase_tests|koi_engine_en_croissant_process" --output-on-failure
```

Result: `100% tests passed, 0 tests failed out of 3`.

## Changed files

- `CMakeLists.txt`
- `README.md`
- `src/koi/uci_controller.cpp`
- `tests/uci_controller_tests.cpp`
- `tests/en_croissant_uci_test.ps1`
- `.superpowers/sdd/2026-09-05-en-croissant-intelligence/task-1-report.md`

Lucas-specific wording remains only as a compatibility fallback; the primary
setup and process verification now target En Croissant.
