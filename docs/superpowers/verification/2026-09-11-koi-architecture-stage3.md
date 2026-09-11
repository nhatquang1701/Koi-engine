# Koi Architecture Stage 3 Verification

Date: 2026-09-12

## Scope

Stage 3 extracted adaptive move-ordering state and scalar search-policy
decisions behind private seams. `SearchOrderingTables` owns one worker's
killer/history/counter/continuation tables, while `SearchMoveOrdering` remains
the ranking façade. `SearchPolicy` owns null-move, check-extension, LMR,
quiet-futility, and quiescence-capture decision formulas. No new heuristic or
strength change was introduced. `Goal.txt` remains because evaluation/NNUE,
TT/time/parallel runtime, and final tooling/module/strength validation work is
still outstanding.

## Focused Release evidence

The fresh focused build used the Visual Studio x64 developer environment:

```powershell
cmd.exe /c 'call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 && cmake --build build/release --config Release --target search_policy_tests search_architecture_tests koi_search_tests search_ordering_tests perft_tests koi_strength_tests completion_gate_tests uci_controller_tests'
```

The focused test command was:

```powershell
ctest --test-dir build/release -C Release -R 'search_policy_tests|search_architecture_tests|koi_search_tests|search_ordering_tests|perft_tests|koi_strength_tests|completion_gate_tests|uci_controller_tests' --output-on-failure
```

Result: 7/8 focused tests passed. The new `search_policy_tests`, existing
ordering/architecture/rules/strength tests, completion gate, and UCI controller
tests passed. `koi_search_tests` retained the known wall-clock-sensitive
oracle failure:

```text
short oracle b2b4 rook lift
best=a7a5, depth=0, nodes=31 or 32, qnodes=2982 or 3183
```

The node/qnode variation is timing noise at the short deadline. The failure is
not a fixed-depth or deterministic benchmark mismatch.

## Full Release evidence

```powershell
ctest --test-dir build/release -C Release --output-on-failure
```

Result: 39/40 tests passed in 418.51 seconds. CTest test 13,
`koi_search_tests`, was the only failure and reported the short-clock oracle
case above. The other 39 tests passed, including rules and shadow-differential
tests, all search/ordering/policy tests, perft, UCI/completion tests, process
and En Croissant integration, benchmark and match processes, packaging,
hash-memory/cutechess stability, and all Python measurement/training/strength
reports.

## Boundary and parity checks

The public-boundary scan and whitespace check were run after the Stage 3
changes:

```powershell
$leaks = rg -n '#include <chess\.hpp>|chess::(Board|Move|Color|Piece)' src/koi -g '*.hpp' -g '*.ixx' -g '!src/koi/detail/**'
if ($LASTEXITCODE -eq 0) { $leaks; throw 'public Koi headers or modules leak chess-library types' }
git diff --check
```

Both checks were clean. Vendored `chess.hpp` remains private to the compatibility
mirror boundary.

Fixed 64-position, `threads=1`, `speed=100`, untimed profiles were compared
against the Stage 2 after-seam profiles:

| Profile | Hash state | Positions | Nodes | Qnodes | Path |
| --- | --- | ---: | ---: | ---: | --- |
| Stage 2 after | cold | 64 | 2677 | 57638 | `artifacts/verification/stage2-after-cold.json` |
| Stage 3 after | cold | 64 | 2677 | 57638 | `artifacts/verification/stage3-after-cold.json` |
| Stage 2 after | warm | 64 | 2673 | 57616 | `artifacts/verification/stage2-after-warm.json` |
| Stage 3 after | warm | 64 | 2673 | 57616 | `artifacts/verification/stage3-after-warm.json` |

Position-by-position comparisons found zero differences in position id, PV,
score, best move, node count, or qnode count for both cold and warm profiles.

## Status and limitations

The Stage 3 ownership seams, policy boundary tests, ordering regression tests,
full Release gate, public-boundary protection, and deterministic benchmark
parity are recorded. The known short-clock search sensitivity was preserved
verbatim rather than weakening the test. The complete architecture goal is not
complete; later evaluation/NNUE, TT/time/parallel-runtime, and tooling/module/
strength validation stages remain, so `Goal.txt` must not be removed yet.
