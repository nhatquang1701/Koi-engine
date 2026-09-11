# Koi Architecture Stage 5 Verification

Date: 2026-09-12

## Scope

Stage 5 isolated search-facing transposition-table access and node-budget
accounting. `detail::SearchTableAccess` is a per-context enabled/disabled view
over the existing shared `TranspositionTable`; physical storage, locking,
generations, clear/resize behavior, and mate-score normalization remain owned
by that table. `detail::SearchBudget` snapshots the `TimeManager` node limit,
validates serial local counts, and reserves a bounded shared atomic counter for
root-parallel node-limited contexts. Time allocation, cancellation, and worker
scheduling were not changed. `Goal.txt` remains because final tooling/module,
measurement, strength, and adversarial architecture review are still
outstanding.

## TDD evidence

The RED target was added before either private header existed:

```powershell
cmake --build build/release --config Release --target search_runtime_tests
```

It failed at dependency scanning with the expected missing-interface error:

```text
fatal error C1083: Cannot open include file: 'koi/detail/search_budget.hpp'
```

After the minimal headers and `SearchContext` integration were added:

```powershell
cmake --build build/release --config Release --target search_runtime_tests
ctest --test-dir build/release -C Release -R '^search_runtime_tests$' --output-on-failure
```

Result: the target built and 1/1 CTest target passed. The test executable
reported all four cases as passing: enabled TT access, disabled TT access,
local node budget, and shared node budget.

## Focused Release evidence

The fresh focused build used the Visual Studio x64 developer environment:

```powershell
cmd.exe /c 'call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 && cmake --build build/release --config Release --target search_runtime_tests search_architecture_tests search_policy_tests koi_search_tests search_ordering_tests perft_tests koi_strength_tests'
```

The focused test command was:

```powershell
ctest --test-dir build/release -C Release -R '^(search_runtime_tests|search_architecture_tests|search_policy_tests|koi_search_tests|search_ordering_tests|perft_tests|koi_strength_tests)$' --output-on-failure
```

Result: 6/7 focused tests passed. `search_runtime_tests`,
`search_architecture_tests`, `search_policy_tests`, `search_ordering_tests`,
`perft_tests`, and `koi_strength_tests` passed. `koi_search_tests` retained
the known short-clock oracle failure:

```text
short oracle b2b4 rook lift
best=a7a5, depth=0, nodes=31, qnodes=3116
```

This is the existing wall-clock-sensitive case; fixed-depth search, rules,
ordering, policy, runtime seam, strength, and perft results remained green.

## Full Release evidence

All Release executables were rebuilt before the full gate:

```powershell
cmd.exe /c 'call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 && cmake --build build/release --config Release'
ctest --test-dir build/release -C Release --output-on-failure
```

Result: 41/42 tests passed in 424.67 seconds. CTest test 14,
`koi_search_tests`, was the only failure and reported the same short-clock
oracle case above. The new runtime seam test was CTest test 18 and passed.
All other rules, differential, UCI/process, benchmark, replay, package,
measurement, training-wrapper, and strength tests passed.

## Boundary and parity checks

The public-boundary scan and whitespace check were run after the code changes:

```powershell
$leaks = rg -n '#include <chess\.hpp>|chess::(Board|Move|Color|Piece)' src/koi -g '*.hpp' -g '*.ixx' -g '!src/koi/detail/**'
if ($LASTEXITCODE -eq 0) { $leaks; throw 'public Koi headers or modules leak chess-library types' }
git diff --check
```

The scan produced no public Koi-header/module leak and `git diff --check` was
clean. Both new seams remain private under `src/koi/detail`.

Fresh deterministic profiles were generated with:

```powershell
.\build\release\koi-bench.exe --threads 1 --speed 100 --profile-json .\artifacts\verification\stage5-after-cold.json
.\build\release\koi-bench.exe --threads 1 --speed 100 --warm-hash --profile-json .\artifacts\verification\stage5-after-warm.json
```

Compared with the Stage 4 profiles, both runs contained 64 positions and had
zero position-by-position differences in id, PV, score, best move, nodes, or
qnodes:

| Profile | Hash state | Positions | Nodes | Qnodes | Path |
| --- | --- | ---: | ---: | ---: | --- |
| Stage 4 after | cold | 64 | 2677 | 57638 | `artifacts/verification/stage4-after-cold.json` |
| Stage 5 after | cold | 64 | 2677 | 57638 | `artifacts/verification/stage5-after-cold.json` |
| Stage 4 after | warm | 64 | 2673 | 57616 | `artifacts/verification/stage4-after-warm.json` |
| Stage 5 after | warm | 64 | 2673 | 57616 | `artifacts/verification/stage5-after-warm.json` |

## Status and limitations

The Stage 5 TT-access and budget ownership seams, focused tests, full Release
gate, public-boundary protection, and deterministic benchmark parity are
recorded. The known short-clock sensitivity was preserved verbatim. Lazy SMP,
shared histories, additional scheduling, final module/tooling alignment,
strength validation, and the overall adversarial architecture review remain;
`Goal.txt` must not be removed yet.
