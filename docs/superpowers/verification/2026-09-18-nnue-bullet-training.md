# NNUE bullet training verification

Verification record for `docs/superpowers/plans/2026-09-18-nnue-bullet-training.md`.

## Environment

- Windows x64, MSVC 14.44.35207 (VS Community), CMake 3.31.6-msvc6, Ninja.
- Python 3.14.5, torch 2.14.0+cpu, python-chess 1.11.2, numpy.
- Rust 1.98.1 at `%USERPROFILE%\.cargo\bin` (added to PATH per command).
- GPU: NVIDIA GeForce GTX 1060 6GB, compute capability 6.1, driver 582.66.
- Branch `koi-engine-v1`; evidence root
  `artifacts/verification/nnue-bullet-training/` (gitignored).

## Phase 0 — baseline and documentation

Baseline HEAD: `57c3d37` ("Record the NNUE Studio UI verification"), in sync
with `origin/koi-engine-v1`. The suite state at this commit was recorded by the
NNUE Studio UI pass:

| Gate | Result | Evidence |
| --- | --- | --- |
| Release CTest (full) | 58/58 passed, 1180.91 s | `artifacts/verification/nnue-studio-ui/ctest-release.log` |
| Debug CTest (`-LE heavy`) | 50/50 passed, 73.91 s | `artifacts/verification/nnue-studio-ui/ctest-debug.log` |
| Python studio suites | 21/21 UI + 12/12 studio | direct runs, 5.296 s |
| GUI smoke | `PASS gui construction` | `--gui-selftest` |

Scaffolding committed with this phase: the plan, this design spec
(`docs/superpowers/specs/2026-09-18-nnue-bullet-training-design.md`), this
record, and one index row in each of the three
`docs/superpowers/{plans,specs,verification}/README.md` tables.

## Phase 1 — dependencies

The winget step of the fallback chain succeeded on the first attempt, so the
NVIDIA-archive and pip-wheel fallbacks were not needed:

- `winget install Nvidia.CUDA --version 12.9 --silent --accept-package-agreements
  --accept-source-agreements --disable-interactivity` installed CUDA 12.9.1
  (installer `cuda_12.9.1_576.57_windows.exe`); log
  `artifacts/verification/nnue-bullet-training/cuda-install-winget.out.log`.
- `nvcc --version` reports release 12.9, V12.9.86 at
  `C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.9`; `cuda.lib`,
  `cudart.lib`, `nvrtc.lib`, `cublas.lib`, and `cublasLt.lib` are present under
  `lib\x64`; `CUDA_PATH` is set at machine scope. Command wrappers set it
  explicitly because the long-lived shell predates the install.
- Rust 1.98.1 (`cargo`/`rustc`) lives at `%USERPROFILE%\.cargo\bin`; wrappers
  prepend it to `PATH`.
- The pinned bullet revision
  (`2ea3d2d0f7e597b0d645f6e8040cf37818f51bce`) is consumed as a git dependency.
  `cargo check --lib --bin convert` compiled `bullet_lib` without the `cuda`
  feature in 28.50 s (`cargo-check.log.err`), and the CUDA release build of the
  training binary finished later in this phase
  (`cargo-build-cuda.log`). The runtime needs the CUDA `bin` directory on
  `PATH`: without it the binary exits `0xC0000135` (DLL not found).

## Phase 2 — dataset conversion

`tools/nnue/to_bullet.py` converts the `FEN;cp;best_move` label corpus into
bulletformat data:

- Labels are side-to-move relative, so the converter flips the score to
  white-relative and appends the pseudo-result `0.5` (ignored with the zero WDL
  blend) as `FEN | cp | 0.5`.
- A deterministic stride selects the validation split (`--val-fraction`,
  default 0.05); `--limit` caps the rows, malformed rows are skipped and
  counted, and `--text-only` stops after the text stage.
- The bulletformat conversion shells out to the crate's `convert` binary
  (`bulletformat::convert_from_text::<ChessBoard>`), which is auto-discovered in
  the crate's `target/release`/`target/debug` trees.
- Tests: `tests/python/nnue/bullet_data_test.py` (registered as
  `bullet_data_python` in CMake with the `python` label) covers the
  white-relative flip, split/limit/malformed handling, the missing-input error,
  the text-only path, and (when the binary exists) the `.data` record size.
- Smoke: `python tools/nnue/to_bullet.py --limit 500` wrote 475 train rows and
  25 validation rows; `train.data` was 15,200 bytes.

## Phase 3 — training crate, exporter, progress wrapper

`tools/nnue/bullet_train/` is a Cargo crate pinned to the bullet revision:

- `KoiHalfkaKingBucket` implements `SparseInputType` with Koi's exact formula
  (`bucket = zone*4 + (mirrored_file - 4)` from the side-to-move king, index
  `bucket*768 + (color*6 + kind)*64 + square`, 9,216 inputs). Rust tests pin the
  two C++ golden sparse index lists (startpos, midgame) plus range/capacity and
  the king-bucket table; all four pass.
- `KoiOutputBuckets` implements `OutputBuckets<ChessBoard>` with
  `min(7, (32 - pieces)/4)`; the model is
  `l0.crelu()` halves multiplied pairwise (`p[j] = a[j] * a[j + H/2]`) into a
  per-bucket linear head, matching the C++ pair-product inference.
- `src/bin/train.rs` trains with `eval_scale = 100`, `ConstantWDL 0.0`, AdamW,
  cosine decay, and saves raw f32 weights in `l0w, l0b, l1w, l1b` order.
- Weight layout was proven experimentally: `save_unquantised` writes the
  internal column-major buffers without applying `SavedFormat` transforms, and
  comparing the `quantised.bin` (which does apply transforms) against the
  internal buffer showed `l0w` is feature-major. `read_raw_weights` therefore
  reads `l0w` as `(9216, hidden)`; the earlier permuted reading is invalid and
  its smoke exports are stale.
- `tools/measurement/export_bullet_v4.py` parses `raw.bin`, runs the v4 shift
  grid on the validation text, and writes the `KOI-NNUE` v4 container plus
  `koi-nnue-training-metadata-v2` metadata (with a `bullet` backend block).
- `tools/nnue/run_bullet.py` chains convert → train → per-checkpoint validation
  MAE → export and emits the studio progress contract. Bullet's superbatch
  summary arrives after a cursor-up escape, so the parser strips ANSI codes and
  matches without anchors; `--test`/`TestDataset` is a no-op in the pinned
  revision (`Validation data not currently implemented!`), so validation MAE is
  measured by the exporter instead.
- Tests: `bullet_data_test.py` grew to 13 cases (parser incl. cursor-up prefix
  and ANSI, epoch formatting, command builders, raw-weight layout round trip,
  hidden-unit inference); all pass.
- Smoke on the GTX 1060 (`sm_61`): a 2-superbatch run printed genuine epoch
  lines (`train_loss 0.18612` / `0.25063`), the exporter selected
  `s1=6 k3=12` and wrote a 1,180,299-byte v4 container, and
  `koi-bench --nnue` loaded it and produced all 64 rows.

## Phase 4 — studio backend

- `tools/nnue/backends/bullet_backend.py` is a live backend instead of a
  placeholder. It is available when `tools/nnue/run_bullet.py` exists, a CUDA
  12.x toolkit directory is present, and either the release `bullet_train.exe`
  is built or cargo is on the machine; otherwise `unavailable_reason()` names
  the missing piece. `build_command` drives the wrapper (corpus, network name,
  hidden units, batch size, superbatches, learning rates, seed, threads, save
  rate, validation fraction, data directory, shift grids), and `net_path` /
  `metadata_path` resolve through the same `studio_core` naming helpers as the
  other backends.
- `studio_core.default_config()` carries the bullet tuning keys
  (`bullet_hidden_units`, `bullet_superbatches`, `bullet_save_rate`,
  `bullet_final_learning_rate`), so presets and the GUI inherit them.
- Tests: `tests/python/nnue/studio_test.py` gained an availability-aware
  `--dry-run --backend bullet` contract test and a `BulletBackendTests` command
  test; the old "unimplemented backend" expectation was replaced. Combined run
  `python -m unittest tests/python/nnue/studio_test.py
  tests/python/nnue/bullet_data_test.py` → 26 tests, OK in 6.4 s.

## Phase 5 — training campaign and gates

Pending.

## Phase 6 — NNUE bug hunt

An independent audit of the v4 loader, serializer, inference, incremental
accumulators, and the Python tooling against the design spec and the Python
reference found the following defects, each fixed with a regression test:

- **v1/v2 serialization silently dropped nonzero shifts.** Inference applies
  `bottleneck_shift`/`output_shift` for every container version, but
  `NnueLoader::serialize` only validated and wrote shift bytes for v3/v4, so a
  hand-built v2 network with `output_shift = 5` scored differently after a
  serialize/load round trip. Serialize now rejects nonzero shifts for
  `version < kKoiNnuePerspectiveV3FormatVersion` with `malformed_manifest`;
  valid v2 containers still round trip byte-identically.
- **`NnueEvaluator` returned 0 for a non-null invalid network instead of the
  classical fallback.** A shared `network_is_usable()` helper (manifest plus
  array validation) now gates `evaluate` and `create_worker`, matching the
  load path's safe-fallback contract.
- **One-shot `NnueEvaluator::evaluate` allocated ~1 MB of incremental slot
  storage per call.** The temporary worker now uses the stateless
  `EvaluationFeatureExtractor` overload, which is what the search fallback
  scanners need.
- **`run_bullet.py` passed `--hidden` to an exporter that had no such option**
  (argparse's abbreviation rule silently bound it to `--hidden-shifts`). The
  exporter now has a real `--hidden` and rejects a mismatch with the width
  inferred from `raw.bin` (exit 2).
- **Python error paths:** `train_nnue_koi.load_binary_dataset` guards the
  record-count offset (truncated `koi-dataset-v1` now raises `TrainerError`
  instead of `struct.error`), and `export_bullet_v4.load_validation` catches
  `ValueError`, `OverflowError`, and `TypeError` so `inf`/`nan` rows are
  skipped instead of aborting an export.
- **Scripted incremental coverage** for the riskiest deltas: castling for both
  colors and sides, en passant, and a promotion capture now assert accumulator
  equality against a fresh recompute with an unchanged fallback count. (The
  castles use separate side-to-move fixtures because a castled white rook on
  f1/d1 attacks black's castling squares.)

Everything else audited clean: v4 container validation, reserved bytes, shift
cap, payload and SHA-256 parity, pair-product overflow bounds, AVX2 fallbacks,
clamping order, perspective signs, bucket formulas across C++/Python/Rust, and
the advisory hook wiring.

Verification: `build\release\nnue_boundary_tests.exe` and
`build\debug\nnue_boundary_tests.exe` both report `run=27 pass=26 fail=0
skip=1` (the skip is the external-container environment case), and
`python -m unittest tests/python/nnue/bullet_data_test.py` runs 17 tests OK.

## Phase 7 — documentation mismatch audit

Pending.

## Phase 8 — verification and CI refresh

Pending.

## Limitations

Pending.
