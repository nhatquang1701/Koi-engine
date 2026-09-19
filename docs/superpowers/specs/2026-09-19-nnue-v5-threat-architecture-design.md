# NNUE v5 threat architecture design (2026-09-19)

**Plan:** `docs/superpowers/plans/2026-09-19-nnue-v5-threat-architecture.md`

This document is the implementation contract for `KOI-NNUE` container version 5
and the `threat-pairs-v1` feature family. Where the plan states intent, this
specification states exact layouts, formulas, and acceptance rules. Every
hardcoded constant named here has a single authoritative definition in
`src/koi/evaluation_features.hpp` or `src/koi/nnue.hpp`.

The architecture is experimental and opt-in. It does not make NNUE the default
evaluator, does not change the `uci` handshake, and makes no strength claims.

## 1. Motivation and scope

Container v4 (`halfka-king-bucket-v1`, 9216 inputs, cross-half pair products,
single affine head, side-to-move perspective only) is the current opt-in
network. It cannot see attack relations between pieces, and its head is a
single linear map. Version 5 adds:

- a second feature group, `threat-pairs-v1`, encoding every attack relation
  in the position (either colour attacking the other);
- dual-perspective input, pairing the side-to-move and opponent accumulators
  element-wise before the head;
- a two-layer quantized head (L1 with clipped-ReLU, per-bucket output).

Group A stays exactly the v4 encoder so its golden vectors, incremental
deltas, and audit history carry over unchanged.

Out of scope: making NNUE the default evaluator, removing the classical
evaluator, new UCI options, handshake changes, and any change to container
versions 2, 3, or 4 (they must stay loadable and serialize byte-identically).

## 2. Feature sets

### 2.1 Group A: `halfka-king-bucket-v1` (unchanged)

Exactly the existing encoder and constants: 12 own-king buckets, 12 piece
planes, 64 squares, 9216 inputs, `bucket * 768 + plane * 64 +
perspective_square(piece_square)`. Both kings contribute. Sparse capacity 64
(maximum 32 active).

### 2.2 Group B: `threat-pairs-v1` (new)

Perspective: side to move. For black to move every square is mirrored
vertically first (`sq ^ 56`) and colors are mapped into the mover's frame
exactly as in group A. A **threat relation** is one piece (the *attacker*)
whose attack set contains a square occupied by an opposing piece (the
*victim*). Every such relation in the position is encoded, in the requested
perspective's coordinates. Both kings participate.

Encoding every relation, rather than only the perspective's own attacks, keeps
the relation set identical across the two perspectives: bullet's dual-perspective
`map_features` emits `(stm_index, ntm_index)` pairs, so both views must see the
same set of relations. This also matches the symmetric threat feature sets used
by modern SF-class networks.

Indexing (all values perspective-relative):

- `b` = own-king bucket, `0..11`, using the same bucket function as group A.
- `vt` = victim type index, `0..5` for pawn, knight, bishop, rook, queen, king.
- `vs` = victim square, `0..63`.
- `as` = attacker square, `0..63`.
- `st` = slider attacker index, `0..2` for bishop, rook, queen.

Per-bucket offsets (0-based within the bucket block):

| family | formula | range |
| --- | --- | --- |
| pawn attacker | `vt * 64 + vs` | 0..383 |
| knight attacker | `384 + vt * 64 + vs` | 384..767 |
| slider attacker | `768 + st * 384 + as * 6 + vt` | 768..1919 |
| king attacker | `1920 + vt * 64 + vs` | 1920..2303 |

Group B block size per bucket is `2304`; total group B inputs are
`12 * 2304 = 27648`. Final input count for v5 is
`9216 + 27648 = 36864`, which still fits `std::uint16_t` indices.

Total input index: `9216 + b * 2304 + bucket_offset`. Sorted, strictly
increasing. Non-slider families can be produced by several attackers (for
example two pawns attacking the same victim), so the encoder builds a set:
collect, sort, and remove duplicates. Slider features include the attacker
square; two sliders of the same type attacking the same victim from the same
square is impossible, so slider features are unique by construction, but the
set rule applies uniformly.

Capacity constants:

- `kNnueSparseFeatureCapacityThreatV1 = 128` for group B;
- `kNnueSparseFeatureCapacityV5 = 160` for the combined v5 list
  (group A ≤ 32, group B ≤ 128).
- If, in a pathologically promoted position, the collected set exceeds the
  capacity, the sorted list is truncated to the capacity. The truncation rule
  is part of the contract and must be identical in C++, Python, and Rust; the
  fuzz test in §7 pins the observed maximum over random games and requires it
  to stay below capacity.

Feature-set string constant: `threat-pairs-v1`. Container feature-set string
for v5: `halfka-king-bucket-v1+threat-pairs-v1`.

### 2.3 Encoder API additions (`evaluation_features.hpp` / `.cpp`)

```cpp
[[nodiscard]] static NnueSparseFeaturesV5 encode_sparse_v5(
    const EvaluationFeatures&, Color perspective) noexcept;
```

`NnueSparseFeaturesV5` is `{std::array<std::uint16_t, kNnueSparseFeatureCapacityV5> indices; std::size_t count;}`.
The encoder concatenates the group A index set for `perspective` and the
group B index set for `perspective`, then sorts. A dense
`NnueFeatureVectorV5` (`std::array<std::int8_t, 36864>`) is provided for tests
and parity checks only; production code uses the sparse path.

Attack enumeration in C++ uses the existing `detail::attack_tables` helpers
(non-sliding attacks plus occupancy-aware slider attacks). Pawn attacks use
the attacker's forward diagonals, and both colours are enumerated so each
perspective mirrors the same relation set.

## 3. Network and quantization

### 3.1 Float model (training forward)

For the two perspectives `own` (side to move) and `opp`:

- `h_s = W1f x_s + b1f`; `a_s = clamp(h_s, 0, 1)` (clipped ReLU);
- cross pairs `p[j] = a_own[j] * a_opp[j]`, `j in [0, hidden)` (full width);
- `e = clamp(W1hf p + b1hf, 0, 1)` (L1, `l1_units` units, default 32);
- `score = b2of[b] + W2of[b] e`, in units of 100 centipawns
  (`target_scale = 100`), where `b = min(7, (32 - pieces) / 4)` counts all
  occupied squares including kings (unchanged from v4).

The score is side-to-move relative. `evaluate(features, perspective)` orders
the two accumulators (perspective first, opponent second); it is not
antisymmetric under perspective swap, and no negation property is claimed for
v5 (v2/v3/v4 keep their pinned negation behavior).

Widths: `hidden_units = 1536`; `l1_units = 32`; `output_buckets = 8`.

### 3.2 Integer quantization

With `S1 = 1 << s1`:

- `W1_q = clamp(round(W1f * S1), -32767, 32767)` int16;
- `b1_q = round(b1f * S1)` int32;
- `W1h_q = clamp(round(W1hf * (1 << s_l1) / 127), -127, 127)` int8;
- `b1h_q = round(b1hf * 127 * (1 << s_l1))` int32;
- `W2o_q = clamp(round(W2of * 100 * (1 << s_out) / 127), -127, 127)` int8;
- `b2o_q = round(b2of * 100 * (1 << s_out))` int32.

Shift grids for export selection: `s1 in {6,7,8}`, `s_l1 in {6,7,8}`,
`s_out in {12,14,16,18,20}`; the selected combination minimizes integer
round-trip MAE on the validation sample. `hidden_shift` remains trainer-side
metadata only (it is not applied at inference); `l1_shift` and `output_shift`
are applied.

### 3.3 Integer inference

```
acc_s[h] = b1_q[h] + sum W1_q[f][h]   (int64 accumulation, clamp to int32)
values_s[h] = clamp(acc_s[h], 0, 127)
p[j] = values_own[j] * values_opp[j]          # j < hidden, <= 16129
e[k] = clamp((sum_j W1h_q[k][j] * p[j] + b1h_q[k]) >> s_l1, 0, 127)
y    = (b2o_q[b] + sum_k W2o_q[b][k] * e[k]) >> s_out
score = clamp(y, INT_MIN, INT_MAX)
```

Accumulator reconstruction rebuilds one perspective at a time from that
perspective's feature list (group A + group B for that perspective).

AVX2 kernels must match scalar bit-for-bit and keep the established rule that
scalar is the correctness boundary:

- accumulator kernels are reused from v4;
- `compute_cross_pairs_avx2`: `values_own[j] * values_opp[j]` with
  `_mm256_mullo_epi16` (operands ≤ 127), scalar tail;
- `l1_dot_avx2`: int8 weights against int16 pair products; vectorize only when
  `(pair_count / 16 + 1) * 2 * 127 * 16129 <= INT32_MAX` (true for hidden 1536,
  false for wide hidden), else scalar int64; periodic lane reduction keeps
  per-lane partial sums in range;
- `l2_bucket_dot_avx2`: 32 int8 weights per bucket against `e`, int64 or
  overflow-checked int32.

## 4. Container version 5

Magic stays `KOI-NNUE`. Version is `5`. Loader accepts versions 2, 3, 4, 5.
Version 5 requires feature set `halfka-king-bucket-v1+threat-pairs-v1`.

Header (little-endian, 76 bytes):

| offset | size | field |
| --- | --- | --- |
| 0 | 8 | magic `KOI-NNUE` |
| 8 | 4 | version `5` |
| 12 | 4 | `input_units` (must equal 36864) |
| 16 | 4 | `hidden_units` (even, 32..8192) |
| 20 | 4 | `output_buckets` (must equal 8) |
| 24 | 4 | `l1_units` (8..128) |
| 28 | 1 | `hidden_shift` (`s1`, ≤ 20) |
| 29 | 1 | `output_shift` (`s_out`, ≤ 20) |
| 30 | 1 | `l1_shift` (`s_l1`, ≤ 20) |
| 31 | 1 | reserved, must be `0` (v4 requires offsets 30 and 31 to be `0`) |
| 32 | 2 | quantization string length |
| 34 | 2 | feature-set string length |
| 36 | 8 | payload length |
| 44 | 32 | SHA-256 of the payload |
| 76 | qlen | quantization string (`int16/int8`) |
| 76+qlen | flen | feature-set string |
| 76+qlen+flen | payload | payload |

Payload order and sizes:

1. `W1`: `input_units * hidden_units` int16, feature-major.
2. `b1`: `hidden_units` int32.
3. `W1h`: `l1_units * hidden_units` int8, unit-major.
4. `b1h`: `l1_units` int32.
5. `W2o`: `output_buckets * l1_units` int8, bucket-major.
6. `b2o`: `output_buckets` int32.

Payload size formula:
`input*hidden*2 + hidden*4 + l1*hidden + l1*4 + buckets*l1 + buckets*4`.

At the default shape (36864, 1536, 8, 32) the payload is `113,301,920` bytes
(about 108 MiB).

`synthetic_v5()` builds a deterministic network with
`{36864, 32, 8, 8}` (`hidden 32`, `l1 8`, `hidden_shift 7`, `l1_shift 6`,
`output_shift 12`) for container and inference tests.

Validation rules (existing `NnueErrorCode` values, no new codes):

- v2/v3/v4 rules unchanged, including v4's reserved bytes at offsets 30/31;
- v5 requires the combined feature set, `input_units == 36864`,
  `output_buckets == 8`, `hidden_units` even in `[32, 8192]`,
  `l1_units` in `[8, 128]`, reserved byte 31 zero, shifts ≤ 20;
- payload length equals both the formula and the remaining container bytes; no
  trailing bytes; every read bounds-checked; SHA-256 over the payload only;
  deterministic serialization for identical weights.

## 5. Incremental engine

`NnueWorker` keeps the v4 slot model (132 slots, two perspectives, cursor,
scratch, keys). Group A deltas remain exactly as they are. Group B updates
work by recomputation and set difference:

- each slot stores, per perspective, the sorted group B feature list used to
  build it (`std::array<std::vector<std::uint16_t>, 2>` indexed by slot, or an
  equivalent flat layout);
- on a make move, after group A updates, the child's group B list is computed
  from the child position (via the encoder) and diffed against the copied
  parent list; removed indices subtract their `W1_q` rows from the child
  accumulator, added indices add them;
- a king move that crosses a bucket boundary refreshes that perspective
  entirely (group A and group B) and stores the exact child list, reusing the
  existing refresh trigger;
- unmake rewinds the cursor; parent slots (accumulators and lists) are intact;
- null moves change neither group (threats are perspective-absolute);
- `on_make_move`/`prepare_child_slot` copy the parent list into the child slot
  before applying deltas.

The stateless `evaluate` path encodes both perspectives, computes both
accumulators, and runs the head. `NnueEvaluator` fallback, `make_evaluator`,
and concurrency rules are unchanged.

## 6. Training pipeline

### 6.1 `koi-dataset-v2`

Header: `KOI-DATA`, `u32 version = 2`, `u16 group_count = 2`, then per group
`{u16 name_length, utf8 feature_set}` (group A `halfka-king-bucket-v1`, group B
`threat-pairs-v1`), then `u64 record_count`.

Record: four `u16` counts in fixed order
`[A(stm), B(stm), A(opponent), B(opponent)]`, the concatenated `u16` indices
for each block in that order, then `i32 score_cp` (side-to-move relative).
Perspective-normalized encodings for both perspectives are stored because the
opponent list cannot be derived from the side-to-move list.

Resume state schema `koi-dataset-v2-state` with the same crash-safety
semantics as v1 (fsync before checkpoint, size-validated resume, final count
patch at EOF). The v1 reader is retained for `info` and regression tests; the
encoder writes v2.

Expected record size is roughly 210–230 bytes at typical active counts
(~30 group A + ~25 group B per perspective), so a 100M-position corpus is
about 21–23 GB before text corpora and sharding.

### 6.2 Trainers

- PyTorch (`tools/measurement/train_nnue_koi.py`): architecture selector
  (`--arch v4|v5`, v5 default for new runs); `KoiNet` v5 computes both
  perspective embeddings from one shared `EmbeddingBag(36864, hidden)`, clamps
  to `[0, 1]`, forms cross pairs, applies `Linear(hidden, l1)` with clipped
  ReLU, and a per-bucket `Linear(l1, 1)` head selected by piece count.
  `quantize_v5`, integer reference, container v5 writer, and metadata schema
  `koi-nnue-training-metadata-v3`
  (`architecture: {input, hidden, l1, output_buckets, groups: [...]}` plus
  `l1_shift`). Progress lines keep the existing contract (add `l1=` to the
  quantization/selected lines is additive and must remain parseable).
- Bullet crate (`tools/nnue/bullet_train`): `KoiHalfkaThreat` input type
  (36864 inputs, combined group A + threat mapping), output buckets unchanged;
  `train.rs` builds `l0` (36864 → hidden), cross pairs, `l1`
  (hidden → l1) with CReLU, `l2` (l1 → 8) with output-bucket select, and
  saves `[l0w, l0b, l1w, l1b, l2w, l2b]`.
- Exporter (`tools/measurement/export_bullet_v5.py`, or an arch-parameterized
  exporter): reads the six raw tensors, searches the shift grids, writes the
  v5 container and metadata v3, with `backend: "bullet"` and
  `checkpoint` recorded.
- Studio (`tools/nnue/studio_core.py`, backends, GUI): `koi_architecture`
  config key (default `v5`, `v4` selectable) and hidden width surfaced to both
  backends; gate, adoption, and verdict logic unchanged.

### 6.3 Corpus scale

The architecture is sized for 100M+ positions. Data generation reuses
`tools/measurement/gen_training_data.py` (Stockfish 19 binary present) with
sharded outputs; the trainers accept multiple dataset files. The recorded
single-process labeling rate (~233 positions/s) implies multi-day campaigns,
so sharding across processes or machines is expected. No campaign is run as
part of this program.

## 7. Tests and parity

- `evaluation_features_tests.cpp`: golden `threat-pairs-v1` index lists for
  startpos and at least two tactical/midgame FENs, from both perspectives;
  dedupe semantics; bucket probes; combined sparse list sorted and within
  capacity; dense == sparse; occupied-square attack guards.
- `nnue_boundary_tests.cpp`: v5 container round trip, golden payload SHA-256,
  rejection matrix, `synthetic_v5` inference golden vector, scalar vs
  independent int64 reference, scalar vs AVX2 full parity including wide
  weights and saturation, bucket selection, cross-language fixture emitter
  `--emit-v5-fixture`.
- Incremental tests: full-game walks (startpos, en passant, promotion,
  castling, nulls, king-bucket crossings, skipped hooks, wide deltas) requiring
  incremental == full recompute for score and accumulator values, and no
  fallback when hooks are complete.
- Random-game fuzz: observed maximum group B count stays below
  `kNnueSparseFeatureCapacityThreatV1`; both perspectives and both groups
  dimension-check.
- Python: `koi_dataset_test.py` v2 layout/resume/rejections; threat encoder
  parity against `--emit-v5-fixture`; `koi_trainer_test.py` container v5 parse,
  integer reference, metadata v3, byte determinism, `--float-in` reuse.
- Rust: `parity.rs` golden combined index lists for the same FENs; bullet
  input count and bucket table unchanged.
- Guards: v2/v3/v4 containers serialize byte-identically and still load; the
  `uci` handshake is untouched; classical evaluation and all existing
  evaluation/search tests are unchanged.

## 8. Acceptance

The program is accepted when every phase in the plan is complete, the Release
and Debug suites show no new failures beyond the known committed true-IID
failure, the v5 tiny-net end-to-end smoke (CPU trainer → container → `EvalFile`
→ benchmark gate run) succeeds, and the verification record states explicitly
that v5 is experimental, opt-in, and strength-unvalidated pending the user's
training campaign. No strength or Elo claim is made anywhere.
