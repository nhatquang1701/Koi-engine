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

### Advisory hook seam

`EvaluatorWorker` gained four default no-op hooks — `on_make_move(child,
metadata, ply, parent_key)`, `on_unmake_move(child_ply)`,
`on_make_null_move(child, ply, parent_key)` and `on_unmake_null_move(child_ply)`
— documented as hints only: an implementation that receives no notifications
must still score correctly. `EvaluationContext` forwards them through
`notify_make_move`, `notify_unmake_move`, `notify_make_null_move` and
`notify_unmake_null_move`, and only when a private worker exists; workerless
evaluators see a complete no-op. `SearchContext` observed every make and
unmake in `search_context.cpp` through `make_observed` / `unmake_observed` /
`make_null_observed` / `unmake_null_observed` (quiescence, null move, ProbCut,
the main negamax loop, and all four early-exit/final unmakes). Root moves in
`search_runner.cpp` and the fallback scanners are deliberately not hooked; the
worker's key check treats every missed notification as a refresh request.

### Dual-perspective accumulators (`src/koi/nnue.cpp`)

`NnueWorker` keeps two int32 accumulator sets indexed by slot
(`kIncrementalSlotCount = 132`, sized for the search stack plus root), a
per-perspective piece-count bucket per slot, slot keys and validity flags, a
cursor for the current ply, and a one-position scratch rebuild used when no
valid parent is available. `on_make_move` prepares the child slot from the
scratch parent or the cursor's parent slot only when the stored key matches
the caller's `parent_key`; otherwise the child slot is invalidated, the cursor
is cleared and `incremental_fallback_count()` increments. Deltas are applied to
both perspectives: the moving piece leaves `from` and arrives at `to` (capture
removes the captured piece at its square, with the en-passant target offset by
rank), promotions replace the pawn with the promoted piece, and a king move
refreshes that perspective outright, which also covers castling's rook. On
unmake the cursor steps back to the parent slot when it is still valid, so
parent evaluations never invert deltas. Evaluations use the cursor slot or the
scratch when their key equals `state.position_key()`; the side-to-move
perspective is finished through the existing v4 inference paths and the
caller's perspective sign is unchanged. `NnueEvaluatorWorker` delegates the
hooks to its worker, and `incremental_make_count()` / `incremental_fallback_count()`
expose the counters. Making the encoder perspective-aware added
`encode_sparse_v4(features, perspective)`, `halfka_king_bucket_for(features,
perspective)` and `halfka_king_bucket_feature_index(bucket, perspective,
piece, square)`; the single-argument overloads keep their Phase 3 behavior.

### Tests

- `nnue_boundary_tests` (23 cases: 22 pass, 1 environment skip): the
  incremental walk verifies scores, hidden activations and pair products
  against a fresh recompute worker at every node and after every unmake over
  four fixtures — startpos (28 plies), an en-passant position, a
  promotion-heavy ending and a castling position — and requires the fallback
  counter to stay put while hooks are provided; the king-bucket test scripts
  `Kd2`/`Kd3` (white) and `Kd6` (black) across bucket boundaries and unwinds
  them; the recovery test makes a move without notifying the worker, proves the
  score still matches a full recompute, and asserts the fallback counter rises
  once per un-hooked position.
- `evaluation_architecture_tests` (4/4): a counting worker proves
  `EvaluationContext` forwards each notification to the private worker and
  that a workerless evaluator ignores them.
- `search_service_tests` (8/8): a hooked evaluator over a depth-4 search
  proves the engine notifies makes to the private worker, that makes and
  unmakes balance, and that null-move notifications balance.

### Verification

- Release CTest: 56/56 passed (`ctest-release-phase4.log`, 257.70 s).
- Debug smoke (`-LE heavy`): 48/48 passed (`ctest-debug-phase4.log`).

### Deferred within Phase 4

No strength claim is made: no trained v4 network exists yet, so the
incremental path is exercised with synthetic weights and reference
comparisons only. Accumulator deltas stay scalar int32 adds; a SIMD delta
update is not part of this phase. `evaluate(const EvaluationFeatures&, ...)`
still recomputes from scratch for tests and direct callers, and the v1/v2/v3
formats keep their previous full-recompute behavior.

## Phase 5 - trainer overhaul

### Trainer (`tools/measurement/train_nnue_koi.py`)

The new trainer consumes either the resumable binary `koi-dataset-v1` corpus
(written by `koi_dataset.py`) or the legacy `FEN;cp;best_move` text corpus. The
model is a 9216-input `EmbeddingBag` with a bias, clipped to `[0, 1]`, whose
first and second halves form CReLU pair products feeding one linear head per
piece-count bucket; outputs are in units of 100 cp. Defaults are hidden 1024,
batch 8192, AdamW (weight decay 1e-4) with a cosine schedule, SmoothL1 on
`cp / 100`, and a seeded-permutation 5% validation split. Quantization searches
`s1 in {6, 7, 8}` and `k3 in {12, 14, 16, 18, 20}` on up to 4000 validation
samples, prints `quantization v4 ...` per candidate and `selected v4 ...`, and
reports the W1/W2 saturation fractions. Export is deterministic and
byte-identical across runs; `--float-out` writes a checkpoint and `--float-in`
re-quantizes one without training. Metadata uses schema
`koi-nnue-training-metadata-v2` (architecture, activation `crelu-pair`,
shifts, corpus sizes, validation MAEs, saturation, payload/network SHA-256 and
the exact command).

Quantization follows the design exactly with `S1 = 1 << s1`:

- `W1_q = clamp(round(W1f * S1), +/-32767)` int16, feature-major.
- `b1_q = round(b1f * S1)` int32.
- `W2_q = clamp(round(W2f * 100 * 2^k3 / S1^2), +/-127)` int8, bucket-major.
- `b2_q = round(b2f * 100 * 2^k3)` int32.

### Cross-language parity

- `nnue_boundary_tests --emit-v4-fixture <path>` serializes a deterministic
  wide-weight v4 network and prints one JSON line with the hidden width, both
  shifts and, for three reference positions, the FEN, the integer score and the
  sparse index list.
- `tests/python/nnue/koi_trainer_test.py` (5 cases) checks that the Python
  encoder reproduces the C++ sparse indices and integer scores for those
  positions, that the integer reference clips and shifts as specified, that the
  binary dataset reader round-trips and rejects malformed headers, and (with
  torch present) that a toy export is byte-deterministic, carries the documented
  metadata, and that `--float-in` reuses a checkpoint unchanged.
- CMake registers `koi_trainer_python` with
  `KOI_NNUE_BOUNDARY_EXE=$<TARGET_FILE:nnue_boundary_tests>`, so the
  cross-language check runs wherever the boundary executable is built.

### Studio wiring

`tools/nnue/backends/koi_backend.py` wraps the new trainer and is the first
entry in the backend registry; `studio_core.default_config()`, the studio CLI
and `tools/nnue/train.ps1` now default to it. The legacy
`train_nnue_sf.py` v2/v3 trainer remains available as the `torch` backend and
its tests stay green. The studio selftest caps the v4 hidden width at 64 so the
wiring check stays fast.

### Tests and verification

- `tests/python/nnue/koi_trainer_test.py`: 5/5.
- `tests/python/nnue/studio_test.py`: 11/11 (registry order, v4 dry-run,
  legacy dry-run, bullet rejection, GUI smoke, torch-gated selftest).
- Release CTest: 57/57 passed (`ctest-release-phase5.log`, 275.49 s).
- Debug smoke (`-LE heavy`): 49/49 passed (`ctest-debug-phase5.log`).

### Deferred within Phase 5

No network is trained, installed or strength-tested here; the campaign and its
gates belong to Phase 6. The fixture network is a parity instrument with wide
deterministic weights, not a candidate evaluator. CPU-only training still
applies (no usable CUDA path on this host).

## Phase 6 — training campaign and strength gates

### Corpus and candidates

The Phase 1 expansion finished before the campaign: `artifacts/training/labels.txt`
holds 2,249,171 labeled rows and `positions.txt` 2,341,934 positions. Two dataset
snapshots were encoded with `koi_dataset.py`:

- `artifacts/training/koi-dataset.bin` — 2,172,420 records, 92,185,963 bytes
  (captured while the expansion was still labeling).
- `artifacts/training/koi-dataset-full.bin` — 2,249,171 records, 95,181,393 bytes
  (after the expansion finished).

Candidate A (`artifacts/training/koi-v4-1024.nnue`) is the recorded deliverable:
10 epochs, hidden 1024, batch 8192, learning rate 0.002, AdamW weight decay 1e-4,
6 threads, seeded 5% validation split, trained on `koi-dataset.bin`. Float
validation MAE 141.9 cp; the quantization grid selected `s1=7 k3=14` with
round-trip validation MAE 142.1 cp; container 18,882,699 bytes. Log
`artifacts/training/train-v4-20260917-231547.out.log`.

Candidate B (`artifacts/training/koi-v4-1024-full.nnue`) used the same settings on
the full snapshot: float 143.3 cp, selected `s1=7 k3=14` at 144.7 cp
(`train-v4-full-20260917-233831.out.log`). Candidate A stays the deliverable because
it has the lower round-trip error.

The shift grid reproduces the v2/v3 lesson at the new architecture: `s1=6` rows
land at 256–445 cp and `s1=8` at 353–419 cp, while `s1=7` with `k3=12`/`k3=14`
reaches 142–145 cp. The trainer reports W1/W2 saturation in the metadata.

### Gates (candidate A unless noted; all local reports, no Elo claim)

| Gate | Result | Evidence |
| --- | --- | --- |
| Classical fixed-depth non-regression | 64 rows byte-identical to the Phase 0 baseline | `bench-classical-phase6.log` |
| Tactical 64-position suite with the net loaded | 61 match / 3 mismatch (`check_05`, `fork_04`, `pin_03`); classical stays 64/64 | `bench-v4-1024-hard.log` |
| Candidate B tactical suite | 59 match / 5 mismatch | `bench-v4-1024-full-hard.log` |
| Timed single-thread throughput | classical 143,948 nps; v4 76,523 nps (6,741 nodes + 481,627 qnodes in 6,382 ms) | `bench-classical-timed-phase6.log`, `bench-v4-1024-timed-clean.log` |
| Color-balanced equal-node A/B vs classical (20 games, 20,000 nodes, 1 thread, hash 64, own book off) | +0 =10 -10, 25%, `classical-stronger` | `ab-v4-classical.json` |
| Color-balanced equal-node A/B vs `koi-sf-v1` (version 3) | +0 =20 -0, 50%, `inconclusive` (all draws) | `net-v4-vs-v3.json` |

### Harness

- `tools/stability/uci_match.ps1` gained an `-OpponentOptions` parameter that sends
  additional `setoption` lines to the opponent only. The net-match leg reports show
  the candidate received `EvalFile` through `-KoiOptions` and the reference through
  `-OpponentOptions`.
- New `tools/nnue/net_match.ps1` (schema `koi-nnue-net-match-v1`) runs two color legs
  at an equal node limit and reports a candidate/reference verdict; `ab_match.ps1`
  remains the classical comparison.
- `README.md` now points the pipeline and studio sections at the v4 trainer and
  records the measured gate numbers; the stale claim that the trained net passes the
  64/64 tactical gate was corrected.

### Verification

- Release full CTest after the harness change: 57/57 in 227.16 s
  (`ctest-release-phase6.log`).
- Debug smoke (`-LE heavy`): 49/49 in 72.53 s (`ctest-debug-phase6.log`).

### Interpretation and deferred within Phase 6

The version 4 architecture, incremental accumulators, container and trainer run end
to end from `FEN;cp;best_move` to an installable network. The network is still
weaker than the classical evaluator at equal nodes, so the classical evaluator stays
the default and NNUE stays opt-in. Deferred: campaigns longer than 10 epochs or
larger than the current corpus, king-bucket/threat feature variants, hidden sizes
other than 1024 (for example 512 or 1536 speed/strength trade-offs), GPU training,
and an Elo-scale campaign with external anchors. No further training was run after
candidate B; the shortfall is recorded rather than tuned around.

## Phase 7 — classical evaluation modernization

Behavior-preserving deduplication first, then a real (but not adopted) tuning
pipeline.  The classical evaluator stays the default and its fixed-depth
benchmark rows stay byte-identical.

### Deduplication (behavior-preserving)

- Single material source: `evaluation_parameters.hpp` now includes
  `piece_values.hpp`, and five `static_assert`s bind the evaluator's
  pawn/knight/bishop/rook/queen values to
  `kPawnMaterialValue`/`kKnightMaterialValue`/`kBishopMaterialValue`/
  `kRookMaterialValue`/`kQueenMaterialValue`, so the evaluator and the
  SEE/ordering/book table can no longer silently diverge.
- Attack-table routing: `native_feature_attacks` (`game_state.cpp`) now uses
  the precomputed tables instead of hand-walked rays, and the evaluator's
  `sliding_mobility`, `knight_mobility`, `piece_attacks_square`, and
  `king_ring_attack_units` go through `detail::attack_tables` with one
  mailbox occupancy scan per call.
- Dead-position unification: the evaluator no longer owns an
  `insufficient_material` copy; `ClassicalEvaluator::breakdown` defers to
  `GameState::is_dead_position()`, which covers native insufficient material
  plus the known locked pawn wall (with the legal en-passant exception).
  This is the one deliberate behavioral extension: the locked-wall FIDE
  example now evaluates as 0.  New test `classical evaluator locked pawn
  wall` pins the fixture and the zero total.

### Evidence

| Check | Result | Evidence |
|---|---|---|
| Fixed-depth classical rows | 64 rows byte-identical to the Phase 0 baseline | `bench-classical-phase7.log` vs `bench-baseline-t1.log` (`Compare-Object`: 0 diff lines) |
| `classical_evaluator_tests` | 8/8 | direct run |
| `evaluation_boundary_tests` | 12/12 | direct run |
| `evaluation_architecture_tests` | 4/4 | direct run |
| `koi_strength_tests` (64/64 tactical gate) | 7/7 | direct run |
| Release CTest | 58/58, 233.93 s | `ctest-release-phase7.log` |
| Debug smoke | 50/50, 73.48 s | `ctest-debug-phase7.log` |

### Tuning pipeline

- `koi-eval-features` (`tools/engine/koi_eval_features.cpp`, built as
  `koi-eval-features.exe`) reads `FEN` or `FEN;cp;...` rows and writes
  `fen,cp,phase,<12 terms>,total` breakdowns from the side-to-move
  perspective (matching the label corpus convention).
- `tune_classical.py` ridge-fits the 12 documented term columns to `cp` when
  labels are present, otherwise to `total`, and writes a candidate header
  (`kTunedClassicalScale<Term>` + offset) and a JSON report; it never edits
  `src/`.
- Sample run: first 50,000 labeled positions
  (`classical-features-sample.csv`) — current terms MAE 196.74 cp / R² 0.7638;
  fitted MAE 181.17 cp / R² 0.8085.  Candidate scales live in
  `tuned-classical-report.json` (offset +57.10; mobility 0.10, king_activity
  6.57, tempo −3.07, initiative 4.31, ...).
- **Not adopted.** The fit is a label-regression report, not a gated strength
  result: there is no classical-vs-classical equal-node A/B harness, the
  candidate scales are confounded (collinearity plus a +57 cp intercept), and
  adoption would require the 64/64 gate + full suite + A/B evidence.  The
  canonical weights stay in place, satisfying the plan's revert-with-evidence
  clause.
- New Python test `tune_classical_python` (5 cases) covers known-scale
  recovery, degenerate corpora, header/report emission, and an end-to-end
  `koi-eval-features` CSV round trip (skipped when the tool is not built).

### Deferred

- PSQT (768 literals) weighting and trapped bishop/rook terms were not
  attempted; both are behavioral candidates that need the same gated adoption
  path this phase could not provide.  The fitted term scales are available for
  a future campaign once a classical A/B harness exists.

## Phase 8 — release verification

Pending.

## Limitations

- No Elo or CPL claim is made anywhere in this record.
- NNUE remains opt-in; the classical evaluator stays the engine default.
- CPU-only training on this host; the GTX 1060 is not usable with current
  torch wheels.
