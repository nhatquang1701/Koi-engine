# NNUE and evaluation overhaul design (2026-09-17)

**Plan:** `docs/superpowers/plans/2026-09-17-nnue-evaluation-overhaul.md`

This document is the implementation contract for the overhaul. Where the
plan states intent, this specification states exact layouts, formulas, and
acceptance rules. Every hardcoded constant named here is either reused from
the existing code or introduced with a single authoritative definition.

## 1. Motivation and scope

The current opt-in network is `piece-square-king-pawn-v2`
(960→256→32→1, container v3). It recomputes every feature and the whole
first layer per evaluation, vectorizes only the first layer, has no
incremental state, and loses the equal-node A/B match against the classical
evaluator. The overhaul replaces the feature set, network shape, inference
engine, and training pipeline, and modernizes the classical evaluator's
internal duplication without changing its default behavior.

Out of scope: making NNUE the default evaluator, removing the classical
evaluator, new UCI options, and any change to the `uci` handshake.

## 2. Feature set `halfka-king-bucket-v1`

- String constant: `halfka-king-bucket-v1`.
- Perspective: side to move. For black to move, every square is mirrored
  vertically first: `perspective_square(sq) = sq ^ 56`. Piece colors are
  mapped into the mover's frame exactly as in the existing encoder
  (`mover color -> white`, `opponent -> black`).
- Input count: `12 * 12 * 64 = 9216`, indexed
  `bucket * 768 + plane * 64 + perspective_square(piece_square)`.
- Own-king bucket (`0..11`): let `k` be the perspective square of the
  side-to-move king, `file = k & 7`, `rank = k >> 3` (rank 0 is the mover's
  back rank). Mirror the file into the e–h group with
  `mirrored_file = file < 4 ? file + 4 : file`; rank zones are
  `rank <= 2 -> 0`, `rank <= 5 -> 1`, else `2`. Then
  `bucket = rank_zone * 4 + (mirrored_file - 4)`.
- Planes (`0..11`): own pawn, knight, bishop, rook, queen, king, then
  opponent pawn, knight, bishop, rook, queen, king. For a piece of type `t`
  (`PieceType` 1..6) and color `c` relative to the perspective,
  `plane = c == own ? t - 1 : t + 5`. Both kings contribute their own
  plane; the own-king plane is the bucket selector's finer context.
- Each piece contributes exactly one active input; maximum active count is
  32.
- Invalid or missing king (`Square::kInvalid`): bucket `0` and the
  own/opponent king plane feature is skipped. The encoder never reads
  outside the 9216 inputs.
- Sparse view: indices strictly increasing in `std::array<std::uint16_t,
  kNnueSparseFeatureCapacityV4>` with `kNnueSparseFeatureCapacityV4 = 64`;
  `encode_sparse_v4` must not build a dense 9216 vector per call (a dense
  encoder remains available for tests and parity checks).
- Golden vectors: the C++ tests pin literal index lists for at least three
  reference positions (white to move startpos, black to move startpos, one
  midgame FEN). The Python encoder in the training tools must reproduce
  those lists exactly (cross-language parity test).

## 3. Container version 4

Magic stays `KOI-NNUE`. Version is `4`. The loader accepts versions 2, 3,
and 4; v1/v2 serialization remains byte-identical and v3 keeps its current
semantics. Version 4 requires feature set `halfka-king-bucket-v1`.

Header (little-endian):

| offset | size | field |
| --- | --- | --- |
| 0 | 8 | magic `KOI-NNUE` |
| 8 | 4 | version `4` |
| 12 | 4 | `input_units` (must equal 9216 for v4) |
| 16 | 4 | `hidden_units` (even, 32..8192) |
| 20 | 4 | `output_buckets` (must equal 8) |
| 24 | 4 | reserved, must be `0` |
| 28 | 1 | `hidden_shift` (`s1`, ≤ 20) |
| 29 | 1 | `output_shift` (`k3`, ≤ 20) |
| 30 | 2 | reserved bytes, must be `0` |
| 32 | 2 | quantization string length |
| 34 | 2 | feature-set string length |
| 36 | 8 | payload length |
| 44 | 32 | SHA-256 of the payload |
| 76 | qlen | quantization string (`int16/int8`) |
| 76+qlen | flen | feature-set string |
| 76+qlen+flen | payload | payload |

Payload order and sizes:

1. `W1`: `input_units * hidden_units` int16, feature-major
   (`feature * hidden_units + h`).
2. `b1`: `hidden_units` int32.
3. `W2`: `output_buckets * (hidden_units / 2)` int8, bucket-major
   (`bucket * (hidden_units / 2) + j`).
4. `b2`: `output_buckets` int32.

Payload size formula:
`input*hidden*2 + hidden*4 + buckets*(hidden/2) + buckets*4`.

Validation rules (all must reject with the existing `NnueErrorCode`
values; no new codes are required):

- magic, version ∈ {2,3,4}, quantization exactly `int16/int8`;
- v2/v3 keep their current feature-set and shape rules; v3 still requires
  the v2 feature set;
- v4 requires `halfka-king-bucket-v1`, `input_units == 9216`,
  `output_buckets == 8`, `hidden_units` even in `[32, 8192]`, reserved
  header bytes zero, shifts ≤ 20;
- payload length equals both the computed payload size and the remaining
  container bytes; no trailing bytes; every read bounds-checked; SHA-256
  over the payload only; deterministic serialization for identical
  weights.

The shift cap literal must become a named constant used by v3 and v4
validation and serialization (removing the current duplicated `20`).

## 4. Quantization and integer inference

Float model (training forward):

- `h_pre = W1f x + b1f`; `a = clamp(h_pre, 0, 1)` (clipped ReLU);
- pair products `p[j] = a[j] * a[j + hidden/2]` for
  `j in [0, hidden/2)`;
- `score = b2f[b] + Σ_j W2f[b][j] * p[j]`, in units of 100 centipawns
  (`target_scale = 100`), where `b` is the piece-count bucket.

Piece-count bucket: `pieces` is the number of pieces on the board including
kings; `bucket = min(7, (32 - pieces) / 4)` (integer division, clamped to
`[0, 7]`).

Integer quantization with `S1 = 1 << s1`:

- `W1_q = clamp(round(W1f * S1), -32767, 32767)` int16;
- `b1_q = round(b1f * S1)` int32;
- `W2_q = clamp(round(W2f * 100 * 2^k3 / S1^2), -127, 127)` int8;
- `b2_q = round(b2f * 100 * 2^k3)` int32.

Integer inference (scalar reference):

- `acc[h] = b1_q[h] + Σ_{f active} W1_q[f][h]` (int64, clamped to int32);
- `a[h] = clamp(acc[h], 0, 127)` int16;
- `p[j] = a[j] * a[j + hidden/2]` (int32, ≤ 16129);
- `y = b2_q[b] + Σ_j W2_q[b][j] * p[j]` (int64);
- `score_cp = y >> k3` (arithmetic shift), then clamped to `int`.

Shift selection: the trainer grid-searches `s1 ∈ {6,7,8}` and
`k3 ∈ {12,14,16,18,20}` against integer validation MAE computed with the
reference Python implementation, and reports the quantization saturation
fraction for `W1_q`/`W2_q`.

SIMD (AVX2, optional fast path, Release only as today):

- accumulator adds/removes use int16/int32 lane operations with the same
  overflow guards as the current code; the scalar reference stays the
  correctness boundary;
- pair products and the bucket dot product use `maddubs`/`madd` patterns
  with an explicit worst-case int32 bound check; when the bound fails the
  path falls back to scalar;
- `hidden_units % 16 == 0` gates the vector path; otherwise scalar;
- property tests draw random weights (including extreme int16/int8 values)
  and random positions and require scalar == AVX2 == Python reference.

## 5. Incremental accumulators

- `NnueWorker` owns two int32 accumulator vectors (white perspective and
  black perspective) and a per-ply slot array sized by the search stack
  capacity plus the root, so an evaluation at history index `ply` reads
  the slot for `ply`.
- On make: copy the current slots to `ply+1` (memcpy of
  `2 * hidden * 4` bytes), apply the piece deltas from `MoveMetadata` to
  both perspectives, and fully refresh a perspective when its own king
  crossed a bucket boundary (`bucket_before != bucket_after`).
- Delta rules (perspective square applied first):
  - normal move: remove moving piece at `from`, add at `to`;
  - capture: additionally remove the captured piece at the captured
    square (for en passant the square is the ep target adjusted by rank);
  - promotion: remove the pawn at `from` and add the promoted piece at
    `to` (the captured-piece rule still applies);
  - castling: the king delta already covers the king; additionally move
    the rook from its origin to its destination square;
  - null move: no feature delta (features are perspective-independent).
- On unmake: restore the slot at `ply` (no delta inversion needed when the
  parent slot is still intact). If any consumer evaluated a position
  without going through the hooks, the worker detects the mismatch (ply or
  position key) and performs a full refresh before use.
- The evaluation cache and the fallback scanners keep working; they may
  force refreshes at any time.
- Equivalence test: for random games (including quiescence-style chains,
  null moves, promotions, en passant, and castling) the incrementally
  maintained accumulator and score must equal a fresh full-recompute worker
  at every ply.

## 6. Training pipeline

- Dataset source stays the text line format `FEN;cp;best_move`.
- New binary dataset `koi-dataset-v1`: magic `KOI-DATA` (8 bytes), version
  u32 = 1, feature-set length u16, feature-set string, position count u64,
  then per record `u16 count`, `u16 indices[count]` (sorted),
  `i32 score_cp`.
- Generator fixes (`gen_training_data.py`): the dedup set is seeded from an
  existing positions file when resuming; the GUI/README label-depth hint is
  reconciled with the code default; behavior otherwise unchanged.
- Trainer `tools/measurement/train_nnue_koi.py`:
  - model: EmbeddingBag(9216, hidden, sum) + bias, CReLU pair products,
    one linear head per piece-count bucket, float output in cp/100;
  - default hidden 1024, batch 8192, AdamW, learning-rate schedule,
    SmoothL1 against `cp/100`, validation split by seeded permutation,
    deterministic container bytes for identical weights (the metadata file
    records a timestamp and the command, so it may differ between runs);
  - `--float-out` writes the torch checkpoint, `--float-in` quantizes
    without training;
  - export writes the v4 container plus metadata schema
    `koi-nnue-training-metadata-v2` with fields: schema, created, corpus,
    rows_used, val_rows, feature_set, quantization, architecture
    (`input/hidden/output_buckets`), activation `crelu-pair`, shifts
    (`hidden_shift`, `output_shift`), epochs, batch_size, learning_rate,
    seed, target_scale, val_mae_cp, val_round_trip_mae_cp,
    payload_sha256, network_sha256, command.
- Cross-language parity: the Python encoder must produce the same sparse
  index lists as the C++ encoder for the golden positions, and the Python
  integer reference must match C++ integer scores on a fixture network.
  The C++ boundary executable path is wired through `KOI_NNUE_BOUNDARY_EXE`
  in CMake for the Python test.
- The legacy `train_nnue_sf.py` v2/v3 paths stay functional and tested.

## 7. Classical evaluator modernization

Behavior-preserving items (no score change expected; pinned by existing
tests and perft):

- single material source: the evaluator reads the shared
  `piece_values.hpp` table; the parameter fields become validated
  references (`static_assert` equality) instead of an independent copy;
- attack-table routing: `native_feature_attacks`, `sliding_mobility`,
  `piece_attacks_square`, and `king_ring_attack_units` use
  `detail::attack_tables` where bitwise-equivalent;
- insufficient-material/dead-position unification: the evaluator's override
  additionally recognizes the locked-pawn-wall dead positions already
  defined in `Position`, mirroring that logic exactly.

Behavioral candidates (adoption-gated, reverted if not proven):

- new trapped-piece terms for bishops/rooks;
- tuned scalar parameters from `tune_classical.py` (below).

Tuner pipeline:

- new `koi-eval-features` tool dumps the evaluator term breakdown per
  corpus row as CSV (`FEN;result;phase;term columns...`);
- `tune_classical.py` fits a documented subset of scalar terms (pawn
  structure, mobility, activity, development, center control, king safety,
  initiative weights, tempo) with a regularized least-squares/logistic
  objective against game outcomes or `cp` labels, and writes a candidate
  header plus a report;
- adopting fitted values requires: 64/64 classical tactical gate, full
  suite green, and a non-regression equal-node A/B report versus the
  current classical evaluator. Otherwise the tooling and report are kept
  and the canonical values are unchanged.
- PSQT auto-tuning is deferred; the 768 literals stay hand-maintained.

## 8. Compatibility and reporting policy

- No UCI option changes; `EvalFile` behavior and messages stay as
  documented. New information is additive: the benchmark profile gains an
  optional evaluator/network identity block, and `info string` output may
  name the loaded network.
- The classical evaluator stays the engine default; the overhaul does not
  flip it even if a candidate wins the A/B.
- All strength statements are local reports with recorded provenance; no
  Elo or CPL claim is made.
- v1/v2 container serialization is byte-identical; v2/v3 remain loadable;
  the handshake fixture and README option tables stay in sync.

## 9. Risks and mitigations

- New net may still lose to classical: the deliverable is the stronger
  architecture, faster inference, and a truthful report; the default does
  not change.
- CPU-only training limits capacity: hidden 1024 default, 1536 stretch;
  corpus scale is time-boxed.
- Incremental updates can desynchronize: full-refresh fallback plus
  equivalence tests are mandatory before any speed claim.
- Container or feature drift between C++ and Python: golden vectors and a
  wired cross-language parity test are mandatory.
- Long training runs: detached studio-style runs with logs under
  `artifacts/training/runs/`, and the program continues implementing while
  they run.
