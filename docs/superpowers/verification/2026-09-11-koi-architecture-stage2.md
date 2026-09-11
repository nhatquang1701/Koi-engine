# Koi Architecture Stage 2 Verification

Date: 2026-09-11

## Scope

Search-session lifecycle, fixed-capacity search state, worker context, and
deterministic root-line ranking were extracted behind the existing
`SearchService` and `SearchHandle` contracts. Search formulas, native rules
authority, UCI output ownership, cancellation behavior, and deterministic
fixed-depth behavior were preserved. `Goal.txt` remains because ordering and
search policy, evaluation/NNUE, TT/time/parallel runtime, and final tooling
and strength validation stages are still outstanding.

## Focused Release evidence

The fresh focused build used the Visual Studio x64 developer environment:

```powershell
cmd.exe /c 'call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 && cmake --build build/release --config Release --target search_architecture_tests completion_gate_tests uci_controller_tests koi_search_tests search_ordering_tests perft_tests koi_strength_tests'
```

The focused test command was:

```powershell
ctest --test-dir build/release -C Release -R 'search_architecture_tests|completion_gate_tests|uci_controller_tests|koi_search_tests|search_ordering_tests|perft_tests|koi_strength_tests' --output-on-failure
```

Result: 6/7 focused tests passed. `search_architecture_tests`,
`completion_gate_tests`, `uci_controller_tests`, `search_ordering_tests`,
`perft_tests`, and `koi_strength_tests` passed. `koi_search_tests` reached the
same short-clock oracle failure as the pre-seam baseline:

```text
short oracle b2b4 rook lift
best=a7a5, depth=0, nodes=31, qnodes=3020
```

The result is wall-clock-sensitive and is not a deterministic fixed-depth
parity failure.

## Full Release evidence

```powershell
ctest --test-dir build/release -C Release --output-on-failure
```

Result: 38/39 tests passed in 423.78 seconds. The only failing test was
`koi_search_tests` (CTest test 13), with the short-clock failure recorded
above. The benchmark process, UCI process, En Croissant process, UCI match and
clock tests, cutechess smoke test, hash-memory stability, packaging, Python
measurement/tooling tests, and strength-report tests passed.

## Boundary and parity checks

The public-boundary scan and whitespace check were run after the extraction:

```powershell
$leaks = rg -n '#include <chess\.hpp>|chess::(Board|Move|Color|Piece)' src/koi -g '*.hpp' -g '*.ixx' -g '!src/koi/detail/**'
if ($LASTEXITCODE -eq 0) { $leaks; throw 'public Koi headers or modules leak chess-library types' }
git diff --check
```

Both checks were clean. The private compatibility mirror remains the only
intentional owner of vendored `chess.hpp` state.

Fixed 64-position, `threads=1`, `speed=100`, untimed benchmark profiles were
compared against the Stage 1 after-seam profiles:

| Profile | Hash state | Positions | Nodes | Qnodes | Path |
| --- | --- | ---: | ---: | ---: | --- |
| Stage 1 after | cold | 64 | 2677 | 57638 | `artifacts/verification/stage1-after-cold.json` |
| Stage 2 after | cold | 64 | 2677 | 57638 | `artifacts/verification/stage2-after-cold.json` |
| Stage 1 after | warm | 64 | 2673 | 57616 | `artifacts/verification/stage1-after-warm.json` |
| Stage 2 after | warm | 64 | 2673 | 57616 | `artifacts/verification/stage2-after-warm.json` |

Position-by-position comparisons found zero differences in position id, PV,
score, best move, node count, or qnode count for both cold and warm profiles.
These profiles establish behavioral parity; they do not claim a strength or
Elo improvement.

## Status and limitations

Stage 2's ownership boundary, focused structural tests, deterministic root
ranking, public-boundary protection, full Release gate, and benchmark parity
are recorded. The known short-clock search sensitivity is preserved verbatim
and was reproduced during the fresh focused and full Release runs. The overall
architecture objective is not complete, so `Goal.txt` must remain until the
later architecture stages and their final acceptance review are finished.
