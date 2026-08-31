# Task 4 implementation report — tactical suite and classical tuning

## Commit

Task implementation commit SHA: `cf065e40d96fce9f440373fdd262b0853ddea3b1`

## Changed files

- `src/koi/strength_suite.hpp` — extends `StrengthPosition` with an explicit
  accepted-move allowlist, category, optional score floor, and unique fixture ID;
  adds the optional-corpus accessor without exposing chess-library types.
- `src/koi/strength_suite.cpp` — publishes the deterministic hard gate and optional
  corpus from static fixture data.
- `src/koi/classical_evaluator.cpp` — weights passed-pawn structure more heavily as
  material leaves the board while preserving `EvaluationBreakdown` symmetry.
- `src/koi/search_ordering.cpp` — places quiet checking moves after promotions and
  before killers/history quiet moves; TT and capture ordering stay unchanged.
- `tests/koi_strength_tests.cpp` — validates hard/optional metadata and IDs, uses
  explicit accepted-move allowlists, and executes every hard-gate search.
- `tests/koi_search_tests.cpp` — adds a passed-pawn endgame-scaling regression.
- `tests/search_ordering_tests.cpp` — adds a quiet-check ordering regression.
- `README.md` — documents deterministic strength commands and explicitly defers NNUE,
  opening books, tablebases, and variants.

## Fixture inventory

Hard gate: 64 fixed-depth `Threads=1` records, 8 records in each category: mate,
check, evasion, fork, pin, poisoned capture, promotion, and defense/pawn race.
Every record has a nonzero unique ID, FEN, fixed depth, category, and explicit
accepted-move allowlist.

Optional corpus: 128 records for local tuning, not a CI Elo/NPS gate. Category totals:
56 positional, 48 endgame, and 24 king-safety records. Each record is FEN-validated
and carries the same metadata/allowlist contract.

## TDD red evidence

The added hard-gate count test failed before fixture expansion:

```text
FAIL strength suite: the deterministic hard tactical gate must contain exactly 64 positions
```

The passed-pawn regression failed before evaluator tuning:

```text
FAIL evaluator passed pawn endgame scaling: an advanced passed pawn must receive additional weight as material leaves the board
exit=1
```

The initial ordering test failed before quiet-check priority was added:

```text
FAIL quiet checks before quiet moves: a quiet checking move must be searched before ordinary quiet moves
```

The first evaluator fixture was corrected before implementation because its queens
attacked the pawn's forward square and tested an existing threat penalty rather than
phase scaling. This preserved a behavior-focused red test.

## Tuning gates

| Group | Change | Before hard solve | After hard solve |
| --- | --- | ---: | ---: |
| Fixture gate | Formalize 64 hard records and 128 optional records | 7/7 legacy suite | 64/64 |
| Classical evaluation | Taper passed-pawn structure bonus with game phase | 64/64 | 64/64 |
| Tactical ordering | Prioritize quiet checks over ordinary quiet moves | 64/64 | 64/64 |

The passed-pawn change is `passed_bonus * (32 - game_phase) / 24`; all terms remain
centipawns, diagnostics still use `EvaluationBreakdown`, and normal search consumes
only the total. No `SearchService`, UCI, `SearchResult`, `SearchInfo`, Threads, or
Speed semantics were changed. No Task 5 root-parallel scheduling work was added.

## Final verification

Commands run from the x64 Visual Studio developer environment:

```powershell
cmake --build out\current-release --parallel
ctest --test-dir out\current-release --output-on-failure
.\out\current-release\koi-bench.exe --threads 1 --speed 100
```

Build completed successfully. CTest output was clean: 13/13 passed in 27.91 seconds,
including `koi_search_tests`, `search_ordering_tests`, `koi_strength_tests`, UCI
process validation, benchmark-process validation, UCI-match validation, and Windows
CI configuration validation.

The deterministic benchmark reported `match 1` for all 64 hard records. The eight
category representatives (each exercised eight times) were:

```text
mate_in_one          depth 2 score 99999 expected f7e8  move f7e8  match 1
quiet_check          depth 2 score 1068  expected e2e4  move e2e4  match 1
rook_check_evasion   depth 2 score -545  expected e1d2  move e1d2  match 1
knight_fork          depth 3 score 0     expected c3d5  move c3d5  match 1
pinned_queen         depth 2 score 999   expected e1e7  move e1e7  match 1
poisoned_capture     depth 3 score 1574  expected c4c5  move c4c5  match 1
promotion            depth 2 score 1025  expected a7b8q move a7b8q match 1
pawn_race_defense    depth 2 score 132   expected e3d4  move e3d4  match 1
```

## Remaining concerns

- The optional corpus is metadata-validated in CI but intentionally not searched on
  every run; it remains a local tuning corpus without machine-dependent Elo or NPS
  thresholds.
- NNUE, opening books, tablebases, and variants remain intentionally deferred.
- The local `build/` directory is an abandoned incomplete Ninja tree. Verification
  used the working `out/current-release` MSVC Release tree with the Visual Studio
  developer environment loaded.
