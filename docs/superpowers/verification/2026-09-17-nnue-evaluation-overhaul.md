# NNUE and evaluation overhaul verification (2026-09-17)

Scope: the king-bucketed NNUE, incremental inference, container v4, trainer,
and classical-evaluation modernization described in
`docs/superpowers/plans/2026-09-17-nnue-evaluation-overhaul.md`
(spec: `docs/superpowers/specs/2026-09-17-nnue-evaluation-overhaul-design.md`).

## Environment

- Windows x64; MSVC 19.44 (VS 2022 Community, `14.44.35207`), Ninja, CMake
  from the VS toolchain; `build/release` and `build/debug` trees.
- Branch `koi-engine-v1`; Phase 0 baseline at commit `54080cd`.
- Host: 8 logical CPUs, 31.9 GB RAM. Python 3.14.5, torch 2.14.0+cpu
  (CUDA unavailable), python-chess 1.11.2, numpy 2.5.3.
- Existing corpus: `artifacts/training/positions.txt` and `labels.txt`
  (README: 1,461,259 positions, 1,200,002 depth-10 labels), reference
  network `artifacts/training/koi-sf-v1.nnue` (v3, 501,011 bytes).
- Evidence root: `artifacts/verification/nnue-evaluation-overhaul/`.

## Phase 0 — baseline

Commands (Release/Debug trees built from the current checkout):

- `ctest --test-dir build/release -j 4 --output-on-failure`
- `ctest --test-dir build/debug -LE heavy -j 4 --output-on-failure`
- `koi-bench --threads {1,2,4} --profile-json ...`
- `koi-bench --timed --profile-json ...`
- `koi-bench --nnue artifacts/training/koi-sf-v1.nnue [--timed]`

Results (`baseline-summary.json`):

| Check | Result |
| --- | --- |
| Release CTest | 54/54 passed in 237.84 s |
| Debug smoke (`-LE heavy`) | 46/46 passed in 75.60 s |
| Hard suite Threads=1 | 64 rows, all match, deterministic across repeats |
| Hard suite Threads=2 | 64 rows, 53 match, deterministic, rows differ from Threads=1 |
| Hard suite Threads=4 | 64 rows, 53 match, deterministic, rows differ from Threads=1 |
| Timed Threads=1 classical | 754,289 nodes, 5,238 ms, 144,003 nps |
| Timed Threads=1 `koi-sf-v1` | 445,257 nodes, 3,784 ms, 117,668 nps |
| `koi-bench --nnue koi-sf-v1` gate | 64 rows, 60 match |

Baseline findings recorded for later phases:

1. `tools/build/release_verify.ps1` currently cannot pass on this checkout:
   it requires every hard-suite row to be `match 1` and requires the
   Threads 1/2/4 rows to be identical; Threads=2 and Threads=4 produce 11
   `match 0` rows each. The mismatch is deterministic and pre-existing (the
   only existing release-verify directory, `release-verify-eefb3e8e...`,
   contains only `cmake-version` files). Phase 8 must either fix the engine
   side or correct the harness with recorded evidence.
2. `koi-bench` usage text omits `--nnue` and the profile JSON has no
   evaluator identity; both are additive fixes planned for the overhaul.
3. The existing network matches 60/64 on the default hard suite, so NNUE
   candidate gates are reported by match count rather than the classical
   64/64 rule.

## Phase 1 — dataset pipeline

Pending.

## Phase 2 — feature set and container v4

Pending.

## Phase 3 — inference and SIMD

Pending.

## Phase 4 — incremental accumulators

Pending.

## Phase 5 — trainer overhaul

Pending.

## Phase 6 — training campaign and strength gates

Pending.

## Phase 7 — classical evaluation modernization

Pending.

## Phase 8 — release verification

Pending.

## Limitations

- No Elo or CPL claim is made anywhere in this record.
- NNUE remains opt-in; the classical evaluator stays the engine default.
- CPU-only training on this host; the GTX 1060 is not usable with current
  torch wheels.
