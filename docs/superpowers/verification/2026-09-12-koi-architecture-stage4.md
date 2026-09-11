# Koi Architecture Stage 4 Verification

Date: 2026-09-12

## Scope

Stage 4 separated evaluation execution from recursive search. `EvaluatorWorker`
is an optional capability with an empty default; `NnueEvaluator` supplies a
private adapter around one `NnueWorker`, and `detail::EvaluationContext` owns
the worker-or-fallback choice for one search context. Classical evaluation,
feature encoding, NNUE network format, fallback behavior, and scalar/AVX2
inference formulas were not changed. `Goal.txt` remains because TT/time/
parallel-runtime and final tooling/module/strength validation stages are still
outstanding.

## Focused Release evidence

The fresh build used the Visual Studio x64 developer environment:

```powershell
cmd.exe /c 'call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 && cmake --build build/release --config Release --target evaluation_architecture_tests evaluation_boundary_tests nnue_boundary_tests search_policy_tests search_architecture_tests koi_search_tests perft_tests koi_strength_tests'
```

The focused test command was:

```powershell
ctest --test-dir build/release -C Release -R 'evaluation_architecture_tests|evaluation_boundary_tests|nnue_boundary_tests|search_policy_tests|search_architecture_tests|koi_search_tests|perft_tests|koi_strength_tests' --output-on-failure
```

Result: 7/8 focused tests passed. The evaluation architecture, classical
evaluation, NNUE boundary, policy, search architecture, perft, and strength
tests passed. `koi_search_tests` retained the known short-clock oracle
failure:

```text
short oracle b2b4 rook lift
best=a7a5, depth=0, nodes=31, qnodes=3183
```

This is a wall-clock-sensitive timed test and not a fixed-depth or benchmark
signature mismatch.

## Full Release evidence

All Release executables were rebuilt before the full gate:

```powershell
cmd.exe /c 'call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 && cmake --build build/release --config Release'
ctest --test-dir build/release -C Release --output-on-failure
```

Result: 40/41 tests passed in 418.45 seconds. CTest test 14,
`koi_search_tests`, was the only failure and reported the short-clock oracle
case above. The other 40 tests passed, including evaluation/NNUE boundaries,
rules and shadow differential tests, all search/ordering/policy tests, perft,
UCI/completion and process tests, benchmark/match/package/stability tests, and
all Python measurement/training/strength reports.

## Boundary and parity checks

The public-boundary scan and whitespace check were run after the Stage 4
changes:

```powershell
$leaks = rg -n '#include <chess\.hpp>|chess::(Board|Move|Color|Piece)' src/koi -g '*.hpp' -g '*.ixx' -g '!src/koi/detail/**'
if ($LASTEXITCODE -eq 0) { $leaks; throw 'public Koi headers or modules leak chess-library types' }
git diff --check
```

Both checks were clean. Vendored `chess.hpp` remains private to the
compatibility mirror boundary, and the evaluation context remains private to
`src/koi/detail`.

Fixed 64-position, `threads=1`, `speed=100`, untimed profiles were compared
against the Stage 3 after-seam profiles:

| Profile | Hash state | Positions | Nodes | Qnodes | Path |
| --- | --- | ---: | ---: | ---: | --- |
| Stage 3 after | cold | 64 | 2677 | 57638 | `artifacts/verification/stage3-after-cold.json` |
| Stage 4 after | cold | 64 | 2677 | 57638 | `artifacts/verification/stage4-after-cold.json` |
| Stage 3 after | warm | 64 | 2673 | 57616 | `artifacts/verification/stage3-after-warm.json` |
| Stage 4 after | warm | 64 | 2673 | 57616 | `artifacts/verification/stage4-after-warm.json` |

Position-by-position comparisons found zero differences in position id, PV,
score, best move, node count, or qnode count for both cold and warm profiles.
Existing NNUE tests also continued to cover feature vectors, manifests,
fallback, worker isolation, scalar/AVX2 parity, wide accumulation, and invalid
worker guards.

## Status and limitations

The Stage 4 worker/fallback ownership seam, evaluation and NNUE regression
tests, full Release gate, public-boundary protection, and deterministic
benchmark parity are recorded. The known short-clock search sensitivity was
preserved verbatim. The overall architecture objective is not complete;
TT/time/parallel runtime and final tooling/module/strength validation remain,
so `Goal.txt` must not be removed yet.
