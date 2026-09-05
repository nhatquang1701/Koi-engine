# Task 3 Report: Strength-First Opening-Book Safety

## Scope

Implemented deterministic and safety-aware book selection:

- added `BookSafety` (default `true`) and `BookSafetyDepth` (default `2`,
  range `0..3`) UCI options;
- retained highest-weight deterministic selection when `BookRandom=false`,
  including coordinate tie-breaking, and weighted selection only when the
  random option is enabled;
- added a bounded forcing-material probe that rejects an immediately hanging
  valuable piece and falls back silently to search;
- retained legal-move filtering and all analysis/MultiPV/ponder/infinite/
  `searchmoves` book bypass behavior;
- documented the En Croissant defaults and licensed executable-relative book
  placement.

## TDD and verification

RED was observed first from the new opening-book test calling the not-yet-
available safety arguments and from the handshake expectation for the missing
options. GREEN verification was then run with a fresh Release build:

```text
cmake --build out/task1-vs --config Release --target opening_book_tests uci_controller_tests --parallel 2
ctest --test-dir out/task1-vs -C Release -R "opening_book_tests|uci_controller_tests" --output-on-failure
```

Result: `100% tests passed, 0 tests failed out of 2`.

The safety fixture places a white queen on e4 where the configured e4-e3 book
move can be legally captured by a black rook. Safety rejects that move, while
`BookSafety=false` preserves it as the selected legal book move.

### Review fix round 1

The review fix keeps the public range honest at `BookSafetyDepth 0..3`,
selects the deterministic or weighted book move before applying safety (so an
unsafe highest-weight move falls back to search instead of silently selecting a
lower-weight move), and caps each forcing probe at 512 visited nodes. If the
probe budget is exhausted, Koi conservatively falls back to search. The safety
fixture now contains both an unsafe high-weight move and a safe low-weight
move, proving that fallback is retained.

Fresh focused verification after the fix:

```text
cmake --build out/task1-vs --config Release --target opening_book_tests uci_controller_tests --parallel 2
ctest --test-dir out/task1-vs -C Release -R "opening_book_tests|uci_controller_tests" --output-on-failure
```

Result: `100% tests passed, 0 tests failed out of 2`.

## Changed files

- `src/koi/opening_book.hpp`
- `src/koi/opening_book.cpp`
- `src/koi/uci_controller.hpp`
- `src/koi/uci_controller.cpp`
- `tests/opening_book_tests.cpp`
- `tests/uci_controller_tests.cpp`
- `README.md`
- `.superpowers/sdd/2026-09-05-en-croissant-intelligence/task-3-report.md`
