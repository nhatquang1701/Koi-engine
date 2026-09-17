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

### Generator fixes (`tools/measurement/gen_training_data.py`)

- `--resume` now seeds the position-dedup set from an existing positions
  file (`load_seen_hashes`), so re-running `games` no longer appends FENs
  that are already present.
- The label-resume reader moved into `load_labeled_fens` and strips blank
  lines (previously a blank line could be recorded as a labeled FEN).
- The noisy-playout weighting moved into `move_weight` with unchanged
  weights (capture +3, check +2, promotion +4).
- CLI construction moved into `build_parser()` so tests can pin defaults
  without running a stage.
- Label-depth hint reconciled with the code default (9): the studio
  generation note no longer claims depth 10, and the README pipeline
  paragraph now states the example uses depth 10 while the generator
  default is 9.

### Binary encoder (`tools/measurement/koi_dataset.py`)

- Implements `halfka-king-bucket-v1` sparse encoding (12 own-king buckets ×
  12 planes × 64 squares, perspective `sq ^ 56` for black, piece-count
  output bucket) with the golden start-position index list pinned in tests.
- Writes `koi-dataset-v1`: magic `KOI-DATA`, version 1, feature-set length
  and string, u64 position count, then per record `u16 count`,
  `u16 indices[count]`, `i32 score_cp`.
- Streaming and resumable: records plus a `<output>.state.json`
  checkpoint (input byte offset, record count, record bytes) let
  `--limit N` stop after N records and `--resume` continue; finishing
  patches the header count in place and removes the state file. Existing
  output without `--resume` is refused; `--resume` on a complete dataset is
  a no-op. Invalid FENs, missing scores, and `|cp| > 4000` rows are skipped.
- `info` prints the header as JSON.

### Tests

- `tests/python/evaluation/koi_dataset_test.py` (9 cases): startpos golden
  indices, black-startpos mirror symmetry, king-bucket boundaries,
  output-bucket piece counts, deterministic byte-identical round trip,
  malformed-row skipping, checkpoint/resume byte equality, existing-output
  refusal, `info` output.
- `tests/python/evaluation/gen_training_data_test.py` (4 cases): parser
  defaults, resume-seeded dedup, label-resume reader, noisy weighting.
- Registered in CMake as `koi_dataset_python` and
  `gen_training_data_python` (label `python`); the local Release suite is
  now 56 tests. Both pass (2.82 s / 0.49 s under CTest).

### Detached corpus expansion

Launched 2026-09-17 21:46 local (pid 9732) with three Stockfish workers:

```powershell
python tools/measurement/gen_training_data.py all --games 8000 --workers 3 --resume `
    --label-depth 10 --positions artifacts/training/positions.txt `
    --output artifacts/training/labels.txt
```

Logs and pid: `artifacts/training/expansion/expansion-20260917-214650.*`.
The run appends new positions and labels to the existing corpus (dedup
seeded from the 1.4M-position file, labels resume-skips already-written
FENs). Observed throughput at start: ~960 positions/s across three
workers.

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
