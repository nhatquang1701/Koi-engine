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

### Feature set (`src/koi/evaluation_features.hpp/.cpp`)

- Added `kNnueHalfkaKingBucketV1FeatureSet = "halfka-king-bucket-v1"` (21
  bytes), `kNnueKingBucketCount = 12`,
  `kNnueHalfkaKingBucketV1FeatureCount = 9216`, `NnueFeatureVectorV4`.
- `king_bucket()`: file `k % 8`, rank `k / 8`, rank zone (≤2, ≤5, else),
  mirrored file (`file < 4 ? file + 4 : file`), `zone * 4 + (file - 4)`.
- `encode_halfka_king_bucket_v1` and `encode_sparse_v4` share
  `halfka_king_bucket_indices`: own-king bucket base `bucket * 768`, plane
  `own ? type - 1 : type + 5`, index `base + plane * 64 + perspective_square`.
  The sparse path (`kNnueSparseFeatureCapacityV4 = 64`) emits sorted
  increasing indices without materializing the dense 9216 vector; the dense
  path remains available for parity tests.
- Golden vectors pinned in `evaluation_features_tests.cpp`: startpos index
  list (same for white and black to move, 32 active), a midgame FEN list,
  king-bucket boundaries (a1, a5, e4 white / e5 black), and sparse/dense
  equality across positions.

### Container v4 (`src/koi/nnue.hpp/.cpp`)

- New constants: `kKoiNnueHalfkaKingBucketV1FormatVersion = 4`,
  `kKoiNnueOutputBucketCount = 8`, `kKoiNnueMinimumHiddenUnits = 32`,
  `kKoiNnueMaximumHiddenUnits = 8192`, and the named
  `kKoiNnueMaximumShift = 20` that replaces the duplicated literal cap for
  v3 and v4.
- v4 header: 76 bytes before the strings (magic, version, input 9216, even
  hidden 32..8192, 8 output buckets, zero reserved word, `hidden_shift`,
  `output_shift`, two zero reserved bytes, string lengths, payload length,
  payload SHA-256). Payload: feature-major int16 W1, int32 b1, bucket-major
  int8 W2 (`bucket * (hidden / 2) + j`), int32 b2; no output-layer arrays.
- Loader accepts versions 2, 3, and 4. v2/v3 serialization and validation are
  unchanged apart from the named shift cap; v4 rejects anything but
  `halfka-king-bucket-v1`, non-zero reserved fields, shifts above 20, and
  shape mismatches, and falls back to the existing `NnueErrorCode` values.
- `NnueNetwork::synthetic_v4()` provides the minimal-width (hidden 32)
  deterministic fixture used by the boundary tests.
- v4 inference itself is still a guard that returns 0; Phase 3 replaces it
  with the CReLU pair-product scalar and AVX2 paths.

### Tests

- `evaluation_features_tests.cpp`: 4 new cases (startpos golden vector
  ignores the turn, midgame golden vector, king bucket tracks the own king,
  sparse view matches the dense encoding); 10/10 pass.
- `nnue_boundary_tests.cpp`: 3 new cases (v4 container round trip with a
  pinned payload SHA-256 and 590,219-byte container, manifest validation
  rejections, corruption and legacy-version rejection); 14 pass, 1 skip
  (external container path), 0 fail.
- Full Release suite after the change: 56/56 passed in 287.76 s (log
  `ctest-release-phase2.log`); Debug tree built clean and the Debug smoke
  suite is re-run in Phase 8.

### Deferred within Phase 2

- Behavioral v4 evaluation (pair products, buckets) and SIMD live in Phase 3;
  `NnueWorker::evaluate` temporarily returns 0 for v4 networks until then.
  Only synthetic fixtures load v4 at this point; no v4 network is installed
  or shipped, so the classical default is untouched.

## Phase 3 — inference and SIMD

### Scalar reference (`src/koi/nnue.cpp`)

- `piece_count_bucket()` selects the output head from the non-empty piece
  count: `min(7, (32 - pieces) / 4)`; both kings count toward the total.
- `finish_inference_v4_scalar()` reuses the sparse first-layer accumulation
  (int64 sum clamped to int32), clips the activations to `[0, 127]`, forms
  `p[j] = a[j] * a[j + hidden/2]` in int16 (values are at most 127, so the
  product is exact), accumulates `b2[bucket] + Σ W2[bucket][j] * p[j]` in
  int64, applies the arithmetic `output_shift`, and clamps the centipawn
  result to `int`.
- `NnueWorker` sizes `bottleneck_values` to `hidden/2` for v4 (the pair
  products) and `NnueWorker::evaluate` now encodes the sparse v4 inputs,
  selects the piece-count bucket, and returns the score relative to the
  mover; the temporary v4 guard that returned 0 is gone.

### AVX2 paths

- The first layer reuses the existing sparse AVX2 accumulation (16 hidden
  units per iteration) with its int32 overflow guard, which falls back to the
  scalar sum when a bias cannot absorb the worst-case weight delta.
- `compute_pair_products_avx2()` multiplies 16 activation pairs per iteration
  with `_mm256_mullo_epi16` and handles the remainder scalar.
- `bucket_dot_avx2()` uses `_mm256_cvtepi8_epi16` + `_mm256_madd_epi16` into
  int32 lanes; when `pair_count * 127 * 16129` cannot fit an int32 it falls
  back to the int64 scalar dot, so the vector path is always equivalent to
  the reference.
- AVX2 remains Release-only (`KOI_NNUE_COMPILED_AVX2` plus the runtime CPU
  check); the scalar path is the correctness boundary, and Debug exercises
  the same golden vectors through the scalar path.

### Tests (`tests/unit/evaluation/nnue_boundary_tests.cpp`)

- `NNUE v4 golden score`: a hand-crafted hidden-32 network (10 startpos
  inputs at hidden 0 → activation 100, 7 inputs at hidden 16 → 127, every
  output head weight 127) scores exactly 49 with `p[0] = 12700`; the AVX2
  path must reproduce it.
- `NNUE v4 reference`: four positions (startpos, the golden midgame FEN, a
  three-piece position, bare kings) match an independent int64 reference
  implementation written in the test; the opposite perspective is the exact
  negation.
- `NNUE v4 path parity`: a deterministic wide-weight network
  (`output_shift = 12`) matches the reference on both the scalar and
  `avx2_compatible` paths, and both accumulators are identical.
- `NNUE v4 piece bucket`: with only the first output head populated, the full
  board scores non-zero while a three-piece position (bucket 7) scores 0.
- `NNUE v4 wide accumulation`: `INT32_MAX` / `INT32_MIN` biases plus weight
  deltas clamp before the pair products (activations 127 and 0), and both
  paths return 9.
- 19 pass, 1 skip (external container), 0 fail in both Release and Debug.

### Throughput baseline

No trained v4 network exists yet, so the baseline uses a deterministic
full-size synthetic network. `make_synthetic_v4.py` (evidence scaffolding,
not a product tool) writes `artifacts/training/synthetic-v4-1024.nnue`
(18,882,699 bytes; hidden 1024, seed 20260917). Same hard-suite probe:

| Probe | Nodes + qnodes | Elapsed | Aggregate NPS |
| --- | --- | --- | --- |
| Timed Threads=1 classical (Phase 0) | 754,289 | 5,238 ms | 144,003 |
| Timed Threads=1 `koi-sf-v1` v3 (Phase 0) | 445,257 | 3,784 ms | 117,668 |
| Timed Threads=1 synthetic v4 1024-wide | 1,798,332 | 14,774 ms | 121,723 |

Log: `bench-v4-synthetic-timed.log`. The synthetic weights change the search
tree (and therefore the node mix), so this is an inference-throughput
baseline only, not a strength or speed claim; the trained network's gate and
throughput are measured in Phase 6.

### Verification

- Release CTest: 56/56 passed (`ctest-release-phase3.log`).
- Debug smoke (`-LE heavy`): 48/48 passed (`ctest-debug-phase3.log`).

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
