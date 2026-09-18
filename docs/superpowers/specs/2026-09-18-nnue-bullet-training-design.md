# NNUE bullet training design

Implementation contract for `docs/superpowers/plans/2026-09-18-nnue-bullet-training.md`.

## 1. Environment and dependencies

- Target toolkit: CUDA 12.9 (`sm_61` supported; CUDA 13.x drops Pascal).
  `CUDA_PATH` must point at the toolkit root for `crates/gpu/build.rs`
  (links `cuda`, `cudart`, `nvrtc`, `cublas`; kernels are compiled at runtime
  through NVRTC for the detected compute capability).
- Install fallback chain, each step retried before falling through:
  1. `winget install Nvidia.CUDA --version 12.9 --silent --accept-package-agreements --accept-source-agreements --disable-interactivity`;
  2. direct NVIDIA archive installer (`cuda_12.9.*_windows.exe`), silent flags;
  3. pip wheels `nvidia-cuda-nvcc-cu12`, `nvidia-cuda-runtime-cu12`,
     `nvidia-cuda-nvrtc-cu12`, `nvidia-cuda-cublas-cu12` assembled into a
     synthetic `CUDA_PATH` tree (`include/`, `lib/x64/`, `bin/`, `nvvm/`).
- Rust: `%USERPROFILE%\.cargo\bin` is added to `PATH` per command; cargo 1.98.1.
- `bullet` dependency pin: git rev `2ea3d2d0f7e597b0d645f6e8040cf37818f51bce`
  (`bullet_lib` v1.0.0, features `["cuda"]`). Not vendored.
- Failure policy: after retries, the bullet path is compile-tested only and the
  CPU PyTorch trainer remains the working path; the blocker is recorded.

## 2. Feature parity (`halfka-king-bucket-v1`)

Exact C++ contract (`src/koi/evaluation_features.cpp:124-168`):

- Perspective is the side to move; black mirrors squares with `sq ^ 56`.
- Own-king square `k`: `file = k % 8`, `rank = k / 8`;
  `zone = rank <= 2 ? 0 : (rank <= 5 ? 1 : 2)`;
  `mirrored_file = file < 4 ? file + 4 : file`;
  `bucket = zone * 4 + (mirrored_file - 4)`.
- Index `bucket * 768 + plane * 64 + perspective_square(piece_square)`.
- Planes: own pawn..king = `type - 1`, opponent pawn..king = `type + 5`
  (types 1..6). Both kings contribute their own plane. Missing king → bucket 0
  and that king's plane skipped. Maximum 32 active inputs.
- Inputs: `12 * 12 * 64 = 9216`.

Bullet's `ChessBucketsMirrored` pairs files `{a,h}{b,g}{c,f}{d,e}` and cannot
express Koi's `{a,e}{b,f}{c,g}{d,h}` pairing, so the crate implements a local
`SparseInputType` (or `ChessBuckets` with an explicit 64-entry table) that
reproduces the mapping above. The Rust test module pins the three golden sparse
index lists from `tests/unit/evaluation/evaluation_features_tests.cpp`:

- startpos (white and black to move): `8,9,10,11,12,13,14,15,65,70,130,133,
  192,199,259,324,432,433,434,435,436,437,438,439,505,510,570,573,632,639,
  699,764`;
- midgame `r1bqkbnr/pppp1ppp/2n5/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R b KQkq - 4 4`:
  `8,9,10,11,13,14,15,28,70,82,130,133,192,199,259,324,420,432,433,434,435,
  437,438,439,493,505,546,570,632,639,699,764`;
- bucket probes: e4 white and e5 black → `{3420, 3836}`; a1 white → `{320, 764}`;
  a5 white → `{3424, 3836}`.

## 3. Data conversion

- Source rows: `FEN;cp;best_move`, `cp` is side-to-move relative and already
  filtered to `|cp| <= 4000`.
- White-relative score: negate when the side to move is black.
- bulletformat text row: `<FEN> | <white-relative cp> | <result>`, result
  written as `0.5` (ignored because the WDL blend is 0.0).
- Conversion runs through `bulletformat::convert_from_text` in the crate's
  `convert` bin, so no separate `bullet-utils` install is required.
- `to_bullet.py` supports `--input`, `--output`, `--limit`, `--resume`
  (skip already-converted rows via a state file), and prints progress every
  N rows. Malformed rows are skipped and counted.

## 4. Training

- Model: `EmbeddingBag`-equivalent sparse first layer (9,216 × hidden) + bias,
  CReLU `clamp(x, 0, 1)`, pair product `p[j] = a[j] * a[j + H/2]`, one linear
  head per output bucket (8 buckets × H/2), scalar output.
- Output bucket: `min(7, (32 - pieces) / 4)` counting all non-empty squares
  (kings included), implemented as a local `OutputBuckets<ChessBoard>`.
- Activation/pair product: prefer `Slice` + `CABinary::Mul`; if the public
  builder cannot express the slice, add a local `ModelOperation` via
  `ModelBuilder::add_op`.
- Targets: `eval_scale = 100`, `wdl::ConstantWDL { value: 0.0 }`, loss
  `|output, target| output.sigmoid().squared_error(target)`. The raw network
  output is therefore in cp/100 units.
- Optimizer AdamW; cosine decay from the configured initial LR to a final LR;
  schedule/batch sizes are CLI options with defaults hidden 1024, batch 8192
  (fallbacks 4096/2048 on VRAM pressure).
- Validation: a held-out `.data` file (`TestDataset { path, freq }`); the
  wrapper reports its loss. Optional weight decay, seed, and thread options
  mirror `train_nnue_koi.py` naming.
- Checkpoint layout: bullet writes `<out>/<net_id>/raw.bin` (f32 values in
  `SavedFormat` order) and `quantised.bin`. The raw format must be declared so
  the payload order is exactly: l0 weights feature-major (hidden × 9216,
  column-major affine = feature-major), l0 bias (hidden), l1 weights
  bucket-major (8 × H/2), l1 bias (8).

## 5. Export

- `tools/measurement/export_bullet_v4.py` reads `raw.bin`, reshapes to
  `W1 [9216][hidden]`, `b1 [hidden]`, `W2 [8][hidden/2]`, `b2 [8]`, and reuses
  the `train_nnue_koi.py` quantization formulas:
  `S1 = 1 << s1`; `W1_q = clamp(round(W1 * S1), ±32767)`; `b1_q = round(b1 * S1)`;
  `W2_q = clamp(round(W2 * 100 * 2^k3 / S1^2), ±127)`; `b2_q = round(b2 * 100 * 2^k3)`.
- Shift grid `s1 ∈ {6,7,8}`, `k3 ∈ {12,14,16,18,20}`; selection minimises
  integer round-trip MAE on a validation sample; saturation fractions reported.
- Output: standard `KOI-NNUE` v4 container (76-byte header, payload W1 int16
  feature-major, b1 int32, W2 int8 bucket-major, b2 int32) plus
  `koi-nnue-training-metadata-v2` metadata with a `backend` block
  (`name: bullet`, crate revision, CUDA version, batch/hidden/LR/seed) and the
  existing fields (val MAE, round-trip MAE, payload/network SHA-256, command).
- The container must load in the C++ engine and pass `koi-bench --nnue`.

## 6. Progress contract

- `run_bullet.py` tails the trainer output and emits studio lines:
  - `epoch <i>/<n> train_loss <f> val_loss <f> time <f>s` (no `val_mae_cp`;
    the parser's `val_mae_cp` becomes optional);
  - after export: `quantization v4 s1=<s> k3=<k> val_mae_cp <f>`,
    `selected v4 s1=<s> k3=<k> val_mae_cp <f>`,
    `wrote <net> (<bytes> bytes) and <meta>`.
- Bullet's `superbatch N | time Xs | running loss E` lines map to the epoch
  line; `TestDataset` loss maps to `val_loss`. The wrapper prints the exact
  command it runs so the studio `command.txt` stays truthful.

## 7. Studio backend

- `bullet_backend.py` availability requires: cargo resolvable, CUDA path
  present, `tools/nnue/bullet_train/Cargo.toml` present, dataset path exists.
  `unavailable_reason()` distinguishes each missing piece.
- `build_command` runs `python tools/nnue/run_bullet.py` with dataset, output
  directory, hidden/batch/LR/seed/thread options and `--net-out`/`--meta-out`
  under the run directory (network naming from `studio_core.network_file_name`).
- `planned_layout()` is removed or updated to the real paths; the legacy
  `train_nnue_sf.py` torch backend is untouched.

## 8. Bug hunt and documentation audit

- Phase 6 audits: loader error paths (`io_error`, `empty_container`,
  `malformed_manifest`, reserved bytes, shift cap), v4 payload sizing,
  quantization boundaries, incremental accumulator recovery and king-bucket
  refresh, AVX2 overflow bound, perspective/sign conventions, and the Python
  reference in `koi_dataset.py`/`train_nnue_koi.py`. Every defect gets a test
  that fails before the fix.
- Phase 7 sweeps `README.md`, `tools/README.md`, `tests/README.md`, the studio
  and overhaul specs/verification records, and source comments (for example the
  stale `src/koi/nnue.hpp` comment claiming no network-path UCI option) and
  records a mismatch table in the verification record.

## 9. CI refresh

- `.github/workflows/windows.yml` moves to `actions/checkout@v5`,
  `actions/setup-python@v6`, and `actions/upload-artifact@v5`; job names,
  commands, and the Debug ctest invocation stay otherwise unchanged.
- `tests/integration/packaging/ci_configuration_test.ps1` is updated in the same
  commit so the contract assertions keep passing.
