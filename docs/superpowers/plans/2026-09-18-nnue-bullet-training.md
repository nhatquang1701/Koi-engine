# NNUE bullet training plan

Goal: replace the CPU-only PyTorch training bottleneck with a GPU bullet
(Rust/CUDA) pipeline that trains the existing `halfka-king-bucket-v1` v4
architecture with exact feature parity, exports `KOI-NNUE` v4 containers
through the existing Python quantizer, exposes the trainer through the Studio
`bullet` backend, and proves candidates through the repository strength gates.
Install the required CUDA/Rust dependencies with retries; afterwards hunt NNUE
bugs and documentation mismatches; refresh the CI workflow to current action
versions; commit at every phase boundary and push when the suites pass.

## Hard constraints

- Windows x64, MSVC only; builds go through the repository CMake/Ninja trees.
- The studio is tooling: no engine/UCI surface changes, no protocol output
  changes, engine stays Python-independent at runtime.
- Never weaken or delete an assertion to obtain a green run; new behavior gets
  focused tests that fail before the fix.
- Classical evaluation remains the engine default; NNUE remains opt-in with the
  safe classical fallback.
- `KOI-NNUE` v1/v2 serialization stays byte-identical; v2/v3/v4 stay loadable.
- Elo, CPL, and NPS are reports, never CI thresholds or claims.
- Do not commit `.opencode/`, generated corpora, networks, or run directories.
- Commit at every phase boundary with a descriptive imperative message.

## Global decisions

- CUDA 12.9 is the target toolkit (Pascal `sm_61` is dropped in CUDA 13.x).
  Install order: `winget install Nvidia.CUDA --version 12.9` → NVIDIA archive
  installer → pip `nvidia-*-cu12` wheels wired into a synthetic `CUDA_PATH`.
  Each step is retried before falling through; failures are recorded.
- Rust/cargo 1.98.1 is already installed at `%USERPROFILE%\.cargo\bin` (not on
  PATH); the session adds it explicitly. `bullet` is consumed as a pinned git
  dependency (rev `2ea3d2d0f7e597b0d645f6e8040cf37818f51bce`) rather than
  vendored, so the repository stays clean.
- The bullet crate implements Koi's exact feature set locally: `ChessBuckets`
  cannot express the `{a,e}{b,f}{c,g}{d,h}` king-file pairing, so a local
  `SparseInputType` reproduces the C++ `halfka-king-bucket-v1` index formula
  (9,216 inputs). Golden sparse index lists pinned in the C++ tests are the
  parity oracle.
- Output buckets use a local `OutputBuckets<ChessBoard>` implementation with
  Koi's `min(7, (32 - pieces) / 4)` formula (bullet's built-in
  `MaterialCount` uses `(occ - 2) / ceil(32/N)` and must not be used).
- Training target: `eval_scale = 100` (Koi cp/100 units), `ConstantWDL 0.0`
  (pure evaluation targets from cp labels), sigmoid squared error; the raw
  network output then equals cp/100. The converter flips side-to-move labels
  to white-relative and writes a pseudo-result of `0.5` (ignored with a zero
  WDL blend).
- Export: bullet saves raw f32 weights; `export_bullet_v4.py` quantizes them
  with the existing `train_nnue_koi.py` formulas and writes a standard v4
  container + `koi-nnue-training-metadata-v2` metadata (with a `bullet`
  backend block).
- Progress: `run_bullet.py` parses bullet's logger and emits the studio's
  line contract (`epoch ...`, `quantization v4 ...`, `selected v4 ...`,
  `wrote ...`). Because the pinned bullet revision does not implement a
  validation pass, the wrapper measures validation MAE from each saved
  checkpoint through the exporter instead of fabricating one.
- If CUDA/bullet cannot run after the retry chain, keep the integration
  compile-tested and continue with the CPU PyTorch trainer; record the blocker
  and do not claim bullet training results.

## Phase 0 — baseline and documentation

- [x] Record the baseline at `57c3d37` (Release 58/58, Debug non-heavy 50/50
  from the NNUE Studio UI pass) and scaffold
  `docs/superpowers/plans/2026-09-18-nnue-bullet-training.md`,
  `docs/superpowers/specs/2026-09-18-nnue-bullet-training-design.md`,
  `docs/superpowers/verification/2026-09-18-nnue-bullet-training.md`, and the
  three index rows. Commit.

## Phase 1 — dependencies

- [x] Install CUDA 12.9 with the fallback chain; verify `nvcc --version` and
  `CUDA_PATH`; add cargo to the session PATH; fetch the pinned bullet revision
  and compile-check it (`cargo build --release` on a trivial crate).
  Record logs under `artifacts/verification/nnue-bullet-training/`. Commit the
  dependency notes in the verification record.

## Phase 2 — dataset conversion

- [x] `tools/nnue/to_bullet.py`: read `FEN;cp;best_move` labels, flip to
  white-relative, write `FEN | cp | 0.5` text, and convert to bulletformat
  `.data` through the crate's `convert` bin (bulletformat
  `convert_from_text`). `tests/python/nnue/bullet_data_test.py` covers the
  side-to-move flip, malformed rows, resume/limits, and converter invocation
  with a stub. Commit.

## Phase 3 — training crate, exporter, progress wrapper

- [x] `tools/nnue/bullet_train/` Cargo crate: local `SparseInputType` with the
  exact 9,216-index mapping, CReLU activation and the
  `p[j] = a[j] * a[j + H/2]` pair product, `KoiOutputBuckets`, AdamW + cosine
  decay, raw-f32 save format in KOI payload order. Rust tests pin the three
  C++ golden sparse index lists and the output-bucket formula.
- [x] `tools/measurement/export_bullet_v4.py`: quantize raw weights with the
  `train_nnue_koi.py` formulas and shift grid, write the v4 container and
  metadata, report round-trip MAE on a validation sample.
- [x] `tools/nnue/run_bullet.py`: invoke the crate, tail its logger, emit the
  studio progress contract; tests cover log translation. Commit.

## Phase 4 — studio backend
- [x] `tools/nnue/backends/bullet_backend.py` becomes
  available when cargo,
  CUDA, the crate, and the dataset are present; `build_command` targets
  `run_bullet.py`; `planned_layout()` is replaced by
  real paths. Extend
  `tests/python/nnue/studio_test.py`/`studio_ui_test.py`. Commit.
## Phase 5 — training campaign and gates

- [ ] Train with hidden 1024 (batch 8192, falling back to 4096/2048 on VRAM
  pressure) on the bullet dataset; export a v4 candidate; run the 64-position
  tactical gate, timed NPS, equal-node color-balanced A/B versus classical and
  versus `koi-v4-1024`, and classical fixed-depth non-regression. Record nets,
  reports, and logs under `artifacts/`; adopt only if the gates pass. Commit
  the evidence summary in the verification record.

## Phase 6 — NNUE bug hunt

- [ ] Audit the v4 loader/inference/accumulator paths against the design spec
  and the Python reference; add regression tests for any defect (incremental
  accumulator recovery, quantization boundaries, loader error paths, AVX2
  parity, stale comments). Fix each with a test that fails first. Commit.

## Phase 7 — documentation mismatch audit

- [ ] Sweep `README.md`, `tools/README.md`, `tests/README.md`, the studio and
  overhaul specs/verification records, and source comments for drift; produce
  a table of mismatches and fix them. Commit.

## Phase 8 — verification and CI refresh

- [ ] Full Release CTest and Debug non-heavy CTest at the final HEAD; refresh
  `.github/workflows/windows.yml` to current action versions
  (`actions/checkout@v5`, `actions/setup-python@v6`,
  `actions/upload-artifact@v5`), keep
  `tests/integration/packaging/ci_configuration_test.ps1` in sync, and run it.
  Commit and push.

## Deferred

- HalfKA threat features, larger corpora, ROCm/Metal backends, kernel tuning,
  bullet binpack loaders, and making NNUE the default evaluator.

## Verification

See `docs/superpowers/verification/2026-09-18-nnue-bullet-training.md`.
