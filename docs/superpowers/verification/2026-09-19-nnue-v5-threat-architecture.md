# Koi NNUE v5 threat architecture verification

Plan: [2026-09-19-nnue-v5-threat-architecture.md](../plans/2026-09-19-nnue-v5-threat-architecture.md)
Spec: [2026-09-19-nnue-v5-threat-architecture-design.md](../specs/2026-09-19-nnue-v5-threat-architecture-design.md)

## Environment

- Windows x64, MSVC 14.44 (Visual Studio 2022 Community), CMake 3.31 + Ninja,
  Release and Debug trees under `build/`.
- Python 3.14 with numpy, python-chess, and CPU PyTorch; Rust 1.98.1 with the
  pinned `bullet_lib` revision; CUDA 12.9.1 for the bullet compile check and a
  tiny GPU run on the local GTX 1060 (sm_61).
- No network files, corpora, or training run directories are committed; all
  artifacts live under the git-ignored `artifacts/` tree.

## Scope

Version 5 of the Koi NNUE architecture adds a second feature group and a
dual-perspective head while keeping every earlier container loadable:

- Group A stays `halfka-king-bucket-v1` (9216 inputs: 12 king buckets, 12 piece
  planes, 64 squares, kings included).
- Group B is the new `threat-pairs-v1` (27648 inputs: 12 king buckets, 2304
  offsets per bucket for pawn, knight, and king attack families plus the three
  slider families).
- The combined input space is 36864 and the container version is 5.
- Both perspective accumulators feed the head: the pair products are
  `p[j] = a_own[j] * a_opp[j]` for every `j < hidden` (full width), followed by a
  32-unit CReLU layer and eight piece-count output buckets.
- Default hidden width is 1536 for v5; the container carries the L1 width in the
  fourth layer word and stores the hidden, output, and L1 shifts.
- Training support covers `koi-dataset-v2` (four index groups per record),
  the PyTorch trainer (`--arch v5`, metadata schema `koi-nnue-training-metadata-v3`),
  and the bullet Rust/CUDA trainer (dual-perspective graph, exporter
  `export_bullet_v5.py`).
- Version 2, 3, and 4 containers still load, serialize, and evaluate exactly as
  before; the classical evaluator remains the engine default and NNUE stays
  opt-in.

## Design deviation: symmetric threat features

The original plan described group B as "own attacks onto enemy pieces" for each
perspective. During implementation the definition was changed to **every attack
relation in the position, encoded in the requested perspective's coordinates**
(symmetric threats).

Rationale:

- The bullet `SparseInputType::map_features` callback emits `(stm_index,
  ntm_index)` pairs, so the side-to-move and non-side-to-move views must carry
  the same number of active features. An asymmetric "own attacks only" set has a
  different cardinality in the two views and cannot be paired exactly.
- Symmetric threat features match the SF-class design, where each perspective
  sees the complete threat picture rather than only its own attacks.
- Dedupe structure is view-independent (pawn, knight, and king families ignore
  the attacker square; slider families use the attacker square plus victim type,
  and all coordinates mirror), so pairing after per-view dedupe is exact.

The spec, the C++ and Python encoders, the Rust encoder, and every golden vector
now describe the symmetric definition.

## Phase F0 - Design and documentation

- Created `docs/superpowers/specs/2026-09-19-nnue-v5-threat-architecture-design.md`
  with the full contract: feature layouts, head math, quantization rules,
  container bytes, dataset v2 records, incremental design, and acceptance tests.
- Created `docs/superpowers/plans/2026-09-19-nnue-v5-threat-architecture.md`.
- Both documents are indexed in their README tables.

## Phase F1 - Encoders and dataset

- C++ `EvaluationFeatureExtractor` gained `encode_sparse_threat_v1`,
  `encode_sparse_v5`, and `encode_halfka_threat_v5` on top of the existing group
  A encoders; the threat encoder enumerates both colours and emits sorted,
  deduplicated indices.
- `tests/unit/evaluation/evaluation_features_tests.cpp` gained five v5 tests
  (startpos, golden vectors, dedupe, attack families, and the merged v5 view):
  `run=15 pass=15 fail=0`.
- `tools/measurement/koi_dataset.py` writes `koi-dataset-v2` records with the
  four index groups `A_stm`, `B_stm`, `A_opp`, `B_opp`; the legacy v1 header
  reader is retained.
- `tests/python/evaluation/koi_dataset_test.py` covers the v2 layout, golden
  threat lists, merge/capacity behavior, resume, and the v1 reader: 14 tests.
- Rust `KoiHalfkaThreat` in `tools/nnue/bullet_train/src/lib.rs` implements the
  same 36864-input encoder for both views; `tests/parity.rs` pins the startpos
  and midgame own/opp lists.

## Phase F2 - Container v5

- `NnueNetwork` gained `l1_weights`, `l1_bias`, and `l1_shift`; the loader
  accepts version 5 with input 36864, eight buckets, an L1 width in 8..128, and
  a payload of `input*hidden*2 + hidden*4 + l1*hidden + l1*4 + buckets*l1 +
  buckets*4` bytes (113,301,920 at hidden 1536 and L1 32).
- `tests/unit/evaluation/nnue_boundary_tests.cpp` covers the round trip, the
  golden payload digest
  `d9102b1c9e3011b22f9815091eff54b13769b81e3b2deaebbe582ab8570a68db`, the
  validation matrix, and the corruption cases:
  `run=39 pass=38 fail=0 skip=1` (the skip is the external-container test).
- Serialization for versions 2, 3, and 4 is unchanged.

## Phase F3 - Inference

- Scalar inference computes both perspective accumulators, full-width cross
  pair products, the L1 CReLU layer, and the bucket head with int64
  accumulation; AVX2 kernels mirror the scalar results with explicit overflow
  fallbacks, and the scalar path remains the correctness boundary.
- `reference_v5_score` in the boundary tests is an independent int64
  reimplementation; scalar, AVX2, and reference scores agree on the golden
  positions with equal accumulator contents.
- `nnue_boundary_tests.exe --emit-v5-fixture` emits a deterministic v5 container
  plus per-position own/opp sparse index lists and integer scores. The Python
  trainer parity test and the Rust parity tests consume the same fixture, so the
  C++, Python, and Rust encoders and integer references agree exactly.

## Phase F4 - Incremental updates

- v5 workers keep per-perspective group B sets per slot; a make-move recomputes
  the child's threat set and applies the sorted-list difference, while a king
  bucket crossing refreshes the whole perspective. Unmake rewinds the slot
  cursor and null moves need no feature delta.
- Five `NNUE v5 incremental ...` tests cover the random walk, king bucket
  crossings, skipped-hook recovery, scripted castling/en-passant/promotion
  moves, and wide saturating deltas; all pass with no hook-induced fallbacks.
- A 300-game capacity fuzz over both perspectives found a maximum of 15 group B
  entries and 44 merged entries, far below the 128 and 160 capacities.
- The nps comparison between v4 and v5 is deferred because no representative v5
  network exists yet.

## Phase F5 - Trainers and exporter

- `tools/measurement/train_nnue_koi.py` gained `--arch v5` (default), the
  four-group dataset reader, `KoiNetV5`, `quantize_v5`, the v5 container writer,
  and metadata schema `koi-nnue-training-metadata-v3`.
- `tests/python/nnue/koi_trainer_test.py` passes 7 tests, including the v5
  fixture parity test and a v5 pipeline test that checks the metadata schema,
  the container layout, and byte-for-byte determinism across two runs.
- `tools/nnue/bullet_train/src/bin/train.rs` gained the dual-perspective v5
  graph (one shared feature transformer for both views, full-width cross pairs,
  hidden->32 CReLU, 32->8 buckets, six saved tensors) and compiles with the CUDA
  feature enabled; `cargo test --test parity` passes 7 tests and the crate unit
  tests pass 2.
- `tools/measurement/export_bullet_v5.py` reads the six-tensor checkpoint
  layout, quantizes with the shift grids, and writes the v5 container plus
  metadata v3 with `backend: "bullet"`. The synthetic smoke produced a valid
  container with header `(KOI-NNUE, 5, 36864, 32, 8, 8, 7, 12, 6, 0, 10, 37,
  2359808)` and a payload matching the size formula exactly.
- `tests/python/nnue/bullet_data_test.py` covers the v5 raw layout, hidden
  inference, the exporter CLI/metadata, and the error paths: 22 tests.
- `tools/nnue/run_bullet.py` selects the v5 exporter and defaults for
  `--arch v5` (`--hidden 1536`, `--l1-units 32`, `--l1-shifts 6 7 8`) and keeps
  the v4 exporter for `--arch v4`; the dry-run output confirms both commands.
- A real tiny training run then exercised the whole bullet path on the local
  GPU: `bullet_train.exe` on a GTX 1060 (sm_61) trained two superbatches of a
  32-hidden, L1-32 model from the existing bullet `.data` and saved
  `koi-v5-smoke-2` (second superbatch loss 0.157118, `raw.bin` 4,724,000 bytes);
  `export_bullet_v5.py` wrote a 2,360,987-byte v5 container with header
  `(KOI-NNUE, 5, 36864, 32, 8, 32, 7, 12, 6, 0, 10, 37, 2360864)` and metadata v3
  (`backend: "bullet"`; 200 validation rows, round-trip MAE 454.79 cp). This is
  a pipeline smoke on a two-superbatch model, not a strength result.

## Phase F6 - Studio and documentation

- `studio_core.default_config()` now defaults to `arch: v5`, hidden 1536 for
  both trainers, `bullet_l1_units: 32`, and `bullet_l1_shifts: [6, 7, 8]`;
  the `koi` and `bullet` backends pass the architecture, hidden, L1, and shift
  flags through. The studio test suites pass (`studio_test`,
  `studio_ui_test`, and `bullet_data_test`: 79 tests).
- Updated `README.md` (architecture description, example commands, Studio
  paragraph, and the statement that no v5 network has been trained or
  strength-validated), `tools/README.md` (backend paragraph and exporter
  selection), and `tests/README.md` (v5 inventory notes).

## Phase F7 - Verification

- Release: `ctest --test-dir build\release -C Release -j 8 --output-on-failure`
  -> 58 of 59 passed in 263.84 s. The only failure is the pre-existing committed
  `koi_search_tests_1of4` true-IID case, which this program did not touch.
- Debug: `ctest --test-dir build\debug -C Debug -j 8 --output-on-failure
  -LE heavy` -> 51 of 51 passed.
- End-to-end smoke: a tiny PyTorch v5 network (hidden 32, L1 8, trained on eight
  rows) was loaded through `EvalFile` and searched to depth 3:
  `info string NNUE enabled from ...`, three `info depth ...` lines, and
  `bestmove a2a4`.
- The same engine then loaded the GPU-trained bullet v5 container through
  `EvalFile`: `info string NNUE enabled from ...`, depth 1 and 2 info lines
  (`score cp -3`, `pv a2a3 ...`), and `bestmove a2a3`.
- Cross-language parity totals: C++ `evaluation_features_tests` 15/15 and
  `nnue_boundary_tests` 38 pass/1 skip; Python 14 + 7 + 22 + 79 tests; Rust 7/7
  integration plus 2/2 library tests.

## Known limitations

- No representative version 5 network has been trained, so no v5 strength claim
  is made. The tiny smoke networks exist only to exercise the pipeline locally.
- The bullet v5 path runs end-to-end on the local GPU (training, export, and
  engine load through the two-superbatch smoke above), but no representative v5
  network exists and no v5 nps comparison was recorded.
- The `hidden_shift` field remains trainer metadata; only the L1 and output
  shifts apply at inference.
- Version 5 stays opt-in. The classical evaluator remains the default, the UCI
  surface is unchanged, and no Elo numbers are claimed.
