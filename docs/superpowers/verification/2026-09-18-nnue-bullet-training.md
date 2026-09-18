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

The full label corpus (2,249,171 rows) was converted with
`tools/nnue/to_bullet.py --val-fraction 0.05`: 2,138,346 training rows and
112,545 validation rows, written as `train.data` (68,427,072 bytes) and
`validation.data` (3,601,440 bytes) beside `train.txt` / `validation.txt`. The
conversion and the training runs are recorded under `artifacts/training/bullet/`
and `artifacts/verification/nnue-bullet-training/`.

Two campaigns were run on the GTX 1060 (6 GB) through
`tools/nnue/run_bullet.py` (hidden 1024, batch 8192, 261 batches per
superbatch, AdamW with cosine decay, `ConstantWDL 0.0`, `eval_scale 100`):

| Run | Superbatches | Validation MAE (last epochs) | Export (float / selected) | Container |
| --- | --- | --- | --- | --- |
| `koi-v4-bullet` | 10 | 290.4 → 210.1 cp | 212.2 / 199.4 cp (`s1=6`, `k3=12`) | 18,882,699 bytes |
| `koi-v4-bullet-long` | 200 | plateau ≈ 193 cp | 199.1 / 198.7 cp (`s1=7`, `k3=14`) | 18,882,699 bytes |

The long run was selected as the candidate. Its validation MAE is higher than
the PyTorch v4 candidate (≈ 142 cp) partly because bullet trains
`sigmoid(output).squared_error(sigmoid(cp/100))` in probability space while the
PyTorch trainer minimizes SmoothL1 on `cp/100`; MAE is not the strength gate.

Gates (all local reports; no Elo or CPL claim):

| Gate | Result | Evidence |
| --- | --- | --- |
| Classical fixed-depth non-regression | 64 rows byte-identical to the Phase 0 baseline | `bench-classical-phase5.log` |
| Tactical 64-position suite, bullet candidate | 62 match / 64 | `bench-bullet-hard.log` |
| Tactical 64-position suite, classical | 64 match / 64 (unchanged) | `bench-classical-phase7.txt` |
| Equal-node A/B vs classical (20 games, 20k nodes, 1 thread, hash 64, own book off) | `+0 =10 -10` (25%) → `classical-stronger` | `ab-bullet-classical-2/ab-match.json` |
| Equal-node A/B vs the PyTorch v4 candidate `koi-v4-1024` (20 games, 20k nodes) | `+0 =0 -20` (0%) → `candidate-weaker` | `net-bullet-pytorch-2/et-match.json` |
| Timed single-thread throughput | ≈ 3,960 nps aggregate | `bench-bullet-timed.log` |

The first A/B attempt timed out two games at the 20-second move limit while the
full build was competing for the machine; it was retried with a 60-second
timeout and finished with zero aborted games. The throughput figure is not
comparable to the historical 76.5k / 144k nps numbers: the machine was carrying
heavy external load during the timed runs (a game process and several editors),
and back-to-back reruns of the same binaries took 99 s and 194 s where they
previously took 6.4 s. Node-limited matches are unaffected by wall-clock load,
which is why only those are used as evidence.

Interpretation: the bullet path is functional end to end (convert → GPU
training → per-checkpoint validation MAE → v4 export → engine load), and it is
the fastest iteration loop for this feature set on this machine. The first
candidate is weaker than both the classical evaluator and the PyTorch v4
candidate, so no network is installed and the classical evaluator remains the
engine default. Deferred: longer campaigns with LR restarts, WDL blending,
bullet-side validation support, wider hidden sizes, and NPS measurements on an
idle host.

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

The audit compared the READMEs, the bullet/studio/overhaul specs, and source
comments against the shipped code. Every mismatch below was fixed in this
phase; the two implementation gaps (benchmark evaluator identity, exact option
parsing) were closed in code and covered by tests rather than reworded.

| # | Mismatch | Fix |
| --- | --- | --- |
| F1 | `README.md` called the bullet backend an unavailable scaffold. | Rewrote the Studio paragraph: `--backend bullet` drives the pinned Rust/CUDA trainer through `tools/nnue/run_bullet.py` when cargo and a CUDA 12.x toolkit are present. |
| F2 | `src/koi/nnue.hpp` claimed no network-path UCI option exists. | Replaced the stale comment with the actual `KOI_NNUE_PATH`/`koi.nnue`/`EvalFile` selection seam. |
| F3 | `README.md` repeated the same "library-only seam" claim. | Same correction in the architecture paragraph. |
| F4 | `tests/README.md` listed two names in `known_failures` that live in `intermittent` (now three entries). | Updated both lists to the code state. |
| F5 | `tests/README.md` omitted `nnue_studio_ui_python` and `bullet_data_python`. | Added both to the Python inventory. |
| F6 | `KOI_NNUE_BOUNDARY_EXE` was attributed only to `nnue_training_test.py`. | Now says it is wired by CMake to `koi_trainer_test.py` and optional for the older test. |
| F7 | Bullet design described `to_bullet.py --output/--resume` with a state file. | Documented the real flags (`--output-dir`, `--val-fraction`, `--limit`, `--convert-exe`, `--text-only`) and the deterministic validation stride. |
| F8 | Bullet design claimed a `TestDataset` validation pass. | Documented per-checkpoint MAE measurement; the pinned revision has no validation pass. |
| F9 | Bullet design said the epoch line omits `val_mae_cp`. | Documented the wrapper's line, which the studio parser requires. |
| F10 | Bullet design promised a metadata `backend` block with crate/CUDA/batch fields. | Documented the actual `backend: "bullet"` marker plus checkpoint/architecture/shift/MAE/hash fields. |
| F11 | Bullet design described cargo/Cargo.toml/dataset availability checks and `--net-out` passing. | Documented runner + CUDA `bin` + trainer-or-cargo availability and the wrapper-derived outputs. |
| F12 | Bullet design said the wrapper always prints its inner commands. | Documented that only `--dry-run` prints them; `command.txt` records the wrapper. |
| F13 | Bullet design claimed "three golden sparse index lists" in Rust. | Two sparse lists plus range/capacity and the king-bucket table; the probe lists stay in the C++ encoder tests. |
| F14 | Overhaul design promised benchmark evaluator/network identity. | Implemented: `koi-bench --profile-json` now writes `"evaluator": "classical|nnue"` and, when `--nnue` is given, an `"nnue": {"path", "bytes"}` block; `koi_bench_process_test.ps1` asserts the classical case. |
| F15 | `train.ps1` presets implied bullet epoch counts. | README and design state bullet run length is controlled by `bullet_superbatches`. |
| F16 | Checkpoint path was documented as `<out>/<net_id>/raw.bin`. | Corrected to flat `<checkpoint-dir>/<net_id>-N/raw.bin`. |
| F17 | Overhaul design promised metadata "byte-for-byte" determinism. | Clarified: container bytes are deterministic; metadata records a timestamp and command. |
| F18 | `tools/README.md` had no `tools/nnue/`/`tools/test/` rows and no Studio section. | Added both rows and a new "NNUE training and the Studio" section. |
| F19 | Studio design still described the v3 trainer default, `net.nnue` names, v3 progress lines, and a planned bullet backend. | Updated the status line, trainer/backend descriptions, network naming, progress contract, launcher list, and replaced the "planned" bullet section with the live pipeline. |
| F20 | README listed three Studio tabs. | Added the Data tab. |
| F21 | `koi-bench` usage string omitted `--nnue`. | Added `[--nnue path]`. |

Stylistic fixes: `studio_core.py` and `backends/__init__.py` docstrings no
longer call the bullet backend "future"; `tests/README.md` no longer implies
`koi_uci_match_clock` is heavy.

Evidence: rebuilt `koi-bench` and ran it with and without a network. The
classical profile records `"evaluator": "classical"` (64/64 matches), and the
bullet net profile records `"evaluator": "nnue"` with
`"nnue": {"path": "...koi-v4-bullet-long\\koi.nnue", "bytes": 18882699}` (62/64
matches, unchanged from Phase 5). `ctest -R koi_benchmark_process` passes with
the new identity assertion. Logs:
`artifacts/verification/nnue-bullet-training/profile-classical-check.json`,
`profile-nnue-check.json`, `bench-classical-phase7.txt`, `bench-nnue-phase7.txt`.

## Phase 8 — verification and CI refresh

Both build trees were reconfigured so the new tests register, then the full
suites were rerun at the final HEAD:

| Configuration | Command | Result | Log |
| --- | --- | --- | --- |
| Release full | `ctest --test-dir build/release -j 4 --output-on-failure` | `100% tests passed, 0 tests failed out of 59` (323.90 s) | `ctest-release-phase8.log` |
| Debug non-heavy | `ctest --test-dir build/debug -LE heavy -j 4 --output-on-failure` | `100% tests passed, 0 tests failed out of 51` (113.12 s) | `ctest-debug-phase8.log` |

The four heavy `koi_search_tests` shards are excluded from the Debug smoke run,
which is why it registers eight fewer tests. With cutechess-cli absent the
counts are 58 and 50; the extra `cutechess_stability_smoke` test is labeled
heavy and only appears when cutechess-cli is installed (it is on this machine).

Focused evidence for this program's changes:

- `build/release/nnue_boundary_tests.exe` and the Debug build both report
  `koi-test-summary run=27 pass=26 fail=0 xfail=0 xpass=0 skip=1` (the skip is
  the external-container case, which needs a container path argument).
- `python -m unittest tests/python/nnue/bullet_data_test.py` passes 17 tests
  (converter, exporter layout, wrapper parsing, parser round trip, hidden
  mismatch, inf/nan rows, truncated dataset header).
- `ctest -R koi_benchmark_process` passes (450.91 s real) with the new
  classical-evaluator identity assertion in the profile JSON.

### CI refresh

`.github/workflows/windows.yml` was updated to the current action versions:
`actions/checkout@v5`, `actions/setup-python@v6`, and
`actions/upload-artifact@v5` (three checkout sites, two upload sites). The
Release job's dependency line now installs `python-chess` and `numpy`; the
trainer imports guard torch, so CI stays on the CPU-only Python surface and the
bullet tooling tests skip when numpy is missing. Build and test commands are
unchanged (VsDevCmd + Ninja + `cl`, Release `ctest -j 4 --output-junit`, Debug
`-LE heavy`, shadow-diff with `-DKOI_BUILD_SHADOW_DIFF=ON`).

`tests/integration/packaging/ci_configuration_test.ps1` was kept in sync: it now
asserts `actions/checkout@v5`, `actions/setup-python@v6`,
`actions/upload-artifact@v5`, and the `numpy` install line in addition to the
existing job-name and command regexes. Running it directly exits 0, and it also
runs inside the Release suite.

The shadow-diff job is unchanged and remains opt-in; local trees keep
`KOI_BUILD_SHADOW_DIFF=OFF`.


## Limitations

- The pinned bullet revision has no validation pass (`TestDataset` output is a
  no-op), so validation MAE is measured by re-reading each saved checkpoint's
  `raw.bin` through the exporter. Long runs therefore validate only at save
  points.
- Only the last saved checkpoint of each run is exported; intermediate
  checkpoints stay on disk but are not turned into containers.
- Absolute NPS numbers from this campaign are unusable: the host was running
  heavy external load during the timed benchmarks, and repeated runs of the same
  binaries varied by more than an order of magnitude. Node-limited matches are
  the only timing-independent evidence recorded here.
- The candidate is a local artifact (`artifacts/training/bullet/`) and is not
  committed or installed. The classical evaluator remains the engine default,
  and no Elo or CPL claim is made from any of the matches.
- The training corpus was built from Stockfish depth-10 labels with synthesized
  pseudo-results (`0.5`) because the A/B objective uses a pure evaluation blend;
  result-based blending would need real game outcomes.
- CI installs only `python-chess` and `numpy`; the bullet tooling tests skip when
  their dependencies are missing, and the Rust crate itself is not built in CI
  (no CUDA runner).
