# Koi Architecture Stage 1 Verification

Date: 2026-09-11

## Scope

Private state-ownership seam only; no search formula, UCI contract, or public
rules signature change.

`Position` remains the authoritative rules state. `CompatibilityMirror` now
owns the private vendored-board compatibility state and shadow history, while
`FeatureState` owns native-derived feature-cache storage, publication, and
invalidation. The search path still updates the mirror transactionally and
skips interior mirror comparison; explicit validation and diagnostic paths
retain comparison.

## Build and test evidence

Focused Release command:

```powershell
cmake --build build/release --config Release --target native_rule_state_tests koi_rules_tests koi_core_tests koi_shadow_diff_tests
ctest --test-dir build/release -C Release -R "native_rule_state_tests|koi_rules_tests|koi_core_tests|koi_shadow_diff_tests" --output-on-failure
```

Result: 4/4 tests passed; total test time 3.62 seconds.

Full Release command:

```powershell
ctest --test-dir build/release -C Release --output-on-failure
```

Result: 37/38 tests passed; total test time 466.05 seconds. The only failure
was `koi_search_tests`, which stopped at the short timed `b2b4` oracle with
`best=a7a5`, `completed_depth=0`, `nodes=31`, and `qnodes=3115`. This is a
wall-clock-sensitive test failure, not a deterministic fixed-depth result
change. An isolated pre-seam worktree at commit `8c74bac` reproduced the same
failure twice (`best=a7a5`, `completed_depth=0`), so the failure is not
attributable uniquely to this ownership seam.

Full Debug command:

```powershell
ctest --test-dir build/debug -C Debug --output-on-failure
```

Result: 36/38 tests passed; total test time 918.60 seconds. `koi_search_tests`
failed the short timed authoritative-root case with `completed_depth=0`,
`nodes=1`, `qnodes=73`, and `elapsed_ms=907`. `koi_benchmark_process` reached
its configured 300.10-second timeout. The other 36 tests passed. Debug is not
a suitable timed benchmark configuration for this repository's Release-sized
search/process limits; this record preserves the failure rather than relaxing
the gate.

Public-boundary and diff checks:

```powershell
$leaks = rg -n '#include <chess\.hpp>|chess::(Board|Move|Color|Piece)' src/koi -g '*.hpp' -g '*.ixx' -g '!src/koi/detail/**'
if ($LASTEXITCODE -eq 0) { $leaks; throw 'public Koi headers or modules leak chess-library types' }
git diff --check
```

Result: the public-header/module scan produced no matches and `git diff
--check` exited 0. The private `src/koi/detail/compatibility_mirror.hpp`
header intentionally contains the vendored compatibility types and is excluded
from this public-boundary scan.

## Benchmark profiles

All profiles used the fixed 64-position strength suite with `threads=1` and
`speed=100`. Aggregate values are taken from the profile JSON files.

| Profile | Hash state | Positions | Nodes | Qnodes | Path |
| --- | --- | ---: | ---: | ---: | --- |
| before-cold | cold | 64 | 2677 | 57638 | `artifacts/verification/stage1-before-cold.json` |
| before-warm | warm | 64 | 2673 | 57616 | `artifacts/verification/stage1-before-warm.json` |
| after-cold | cold | 64 | 2677 | 57638 | `artifacts/verification/stage1-after-cold.json` |
| after-warm | warm | 64 | 2673 | 57616 | `artifacts/verification/stage1-after-warm.json` |

Corresponding before/after profiles had zero differences in position id,
principal variation, score, or node count for all 64 positions in both cold and
warm comparisons. The benchmark is a correctness/performance baseline. It does
not establish an Elo or playing-strength improvement.

## Status and limitations

The ownership seam, focused rules tests, differential checks, public-boundary
scan, and deterministic benchmark parity are verified. The full gates retain
the existing short-timed search sensitivity and Debug benchmark timeout shown
above; neither failure was hidden or converted into a weaker assertion. The
temporary `Goal.txt` remains in place because Stage 1 is only one stage of the
complete architecture objective.
