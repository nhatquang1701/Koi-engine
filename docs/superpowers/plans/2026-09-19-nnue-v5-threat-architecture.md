# NNUE v5 threat architecture plan

Goal: take the NNUE evaluation to its final architecture. Add a second feature
group (`threat-pairs-v1`, every attack relation in the position evaluated per
perspective), feed both perspectives into a two-layer quantized head, define
container version 5 and `koi-dataset-v2`, extend the PyTorch and bullet trainers
plus the exporter and Studio, and pin the whole thing with cross-language golden
vectors and incremental equivalence tests. The network is sized for 100M+
position campaigns (input 36864, hidden 1536, L1 32, 8 output buckets) but no
training campaign is run by the agent; the user trains later through the Studio.

**Spec:** `docs/superpowers/specs/2026-09-19-nnue-v5-threat-architecture-design.md`

## Hard constraints

- Windows x64, MSVC only; builds go through the repository CMake/Ninja trees.
- Classical evaluation remains the engine default; NNUE stays opt-in with the
  classical fallback.
- The `uci` handshake stays byte-identical; no new UCI options.
- Containers v2/v3/v4 stay loadable and serialize byte-identically; the loader
  gains v5 without touching earlier branches.
- Scalar inference is the correctness boundary; every AVX2 kernel keeps its
  overflow fallback and must match scalar bit-for-bit.
- Never weaken or delete an assertion to obtain a green run. Golden vectors,
  the 64-position gate, perft, and shadow-diff stay mandatory.
- No strength or Elo claims. v5 is experimental, opt-in, and
  strength-unvalidated pending the user's training campaign.
- Do not commit `.opencode/`, generated corpora, networks, run directories, or
  other artifacts.
- Do not disturb the pre-existing true-IID work already committed at `0d741a9`.

## Global decisions

- Dual-perspective input: the head consumes element-wise cross pairs of the
  side-to-move and opponent accumulators. No negation property is claimed for
  v5; v2/v3/v4 behavior is unchanged.
- Threat features are conditioned on the own-king bucket (12 buckets,
  2304 offsets each, 27648 inputs) and deduplicated as sorted sets.
- Group B updates use recompute-and-set-diff per make move; a true delta
  enumeration is deferred until the measured cost requires it.
- `koi-dataset-v2` stores all four index blocks (A/B for side to move and
  opponent); the v1 reader is retained.
- The PyTorch path is the reference trainer; the bullet path is the GPU
  campaign path. Both share the same quantization and container writer rules.

## Phase F0 — design freeze and documentation

- [x] Write `docs/superpowers/specs/2026-09-19-nnue-v5-threat-architecture-design.md`
  with every formula, count, offset, and acceptance rule; scaffold this plan
  and add both index rows. Landed; the threat semantics were later made
  symmetric (see F1).

## Phase F1 — feature encoders and dataset v2

- [x] `evaluation_features.hpp/.cpp`: `threat-pairs-v1` constants, capacity
  constants, `NnueSparseFeaturesV5`, dense vector, `encode_sparse_v5`, and
  `encode_sparse_threat_v1`; attack enumeration via `detail::attack_tables`.
  Threat enumeration now covers every attack relation (either colour) so both
  perspectives see the same set, matching bullet's dual-perspective pairing.
- [x] `evaluation_features_tests.cpp`: golden group-B lists (startpos plus two
  tactical FENs, both perspectives), dedupe, bucket probes, combined sorted
  list, dense == sparse, capacity fuzz.
- [x] `tools/measurement/koi_dataset.py`: `koi-dataset-v2` writer/reader with
  four index blocks, new state schema, `info` support, and tests.
- [x] `tools/nnue/bullet_train`: `KoiHalfkaThreat` input mapping and
  `parity.rs` golden lists.
- [x] `nnue_boundary_tests.cpp`: `--emit-v5-fixture` emitter; Python parity
  test consumes it.

## Phase F2 — container v5

- [x] `nnue.hpp/.cpp`: version constant, feature-set string, `NnueNetwork`
  L1 tensors, `payload_size_for`, `validate_manifest`, `arrays_match_manifest`,
  `serialize`, `load`, `synthetic_v5`.
- [x] `nnue_boundary_tests.cpp`: v5 round trip, golden payload SHA-256,
  rejection matrix (reserved bytes, shift caps, L1 range, input mismatch,
  truncation), v2/v3/v4 byte-identity regressions.

## Phase F3 — inference v5

- [x] Scalar kernels: cross pairs, L1 dot with int64 accumulation, per-bucket
  L2, dispatch in `NnueWorker::evaluate`; independent int64 reference in tests.
- [x] AVX2 kernels: `compute_cross_pairs_avx2`, `l1_dot_avx2`,
  `l2_bucket_dot_avx2` with pre-check fallbacks; scalar/AVX2 full parity
  including wide weights and saturation.
- [x] Golden score tests for `synthetic_v5` and piece-count bucket selection.

## Phase F4 — incremental v5

- [x] Slot threat lists, `apply_threat_deltas` set-difference updates,
  king-bucket refresh, stateless dual-perspective path.
- [x] Equivalence walks (startpos, en passant, promotion, castling, nulls,
  bucket crossings, skipped hooks, wide deltas); fallback counters stay put
  when hooks are complete.
- [x] Fuzz the observed maximum group-B count; record the number in the test.
  Observed maxima over 300 games x 160 plies: group B 15, merged v5 44.
- [ ] Measure v5 nps versus v4 with `koi-bench` on a tiny local network; record
  the ratio in the verification record (no threshold). Deferred: no
  representative v5 network exists yet; recorded in the verification record.

## Phase F5 — trainers and exporter

- [x] `train_nnue_koi.py`: `--arch v4|v5`, v5 `KoiNet`, `quantize_v5`, integer
  reference, container v5 writer, metadata v3.
- [x] `export_bullet_v5.py` (or arch-parameterized exporter): six tensors,
  shift search, v5 container, metadata v3; `run_bullet.py` passes the arch and
  hidden layout.
- [x] `bullet_train`: v5 model, save formats, training smoke on a tiny slice.
  The dual-perspective model compiles with CUDA, and a two-superbatch smoke ran
  on the local GTX 1060 (sm_61); `export_bullet_v5.py` wrote a v5 container that
  the engine loaded through `EvalFile`. A representative campaign remains the
  user's to run.
- [x] Python tests: v2 dataset, v5 container parse, integer reference, byte
  determinism, metadata v3, exporter raw layout.

## Phase F6 — Studio and documentation

- [x] `studio_core.py` + backends + GUI: `koi_architecture` (default v5,
  v4 selectable), hidden width surfaced, dry-run/selftest still work.
- [x] README, `tools/README.md`, `tests/README.md`: document v5, the dataset
  v2 layout, the trainer options, and the unvalidated-strength status.

## Phase F7 — verification

- [x] Tiny-net end-to-end smoke: CPU trainer (hidden 64) -> container v5 ->
  `EvalFile` load -> `koi-bench` gate run; no artifacts committed. Smoke used a
  32-hidden net: `info string NNUE enabled from ...`, depth 1-3 info lines,
  `bestmove a2a4`.
- [x] Full Release and Debug CTest; no new failures beyond the known committed
  true-IID failure. Release 58/59, Debug `-LE heavy` 51/51.
- [x] Verification record
  `docs/superpowers/verification/2026-09-19-nnue-v5-threat-architecture.md`
  with exact commands, results, and the explicit strength-unvalidated statement;
  index rows added. The v4/v5 nps ratio is deferred because no representative v5
  network exists yet.
