# Koi NNUE Validated Opt-In Phase Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add backward-compatible piece-square-v1 loading plus a validated opt-in piece-square-king-pawn-v2 NNUE path and deterministic offline export metadata.

**Architecture:** Keep the current `KOI-NNUE` v2 container and dispatch dimensions/encoding from the manifest feature-set string. Centralize Koi-owned feature encoding in `evaluation_features`, keep immutable network storage separate from worker-local accumulators, and make scalar inference the reference for an optional AVX2-compatible fast path. Extend the existing Python tool with a small synthetic export boundary and optional GPU backend without introducing a runtime dependency.

**Tech Stack:** C++26/MSVC-compatible standard library, `std::expected`, fixed-width integer serialization, SHA-256, optional AVX2 intrinsics under compile guards, Python 3 standard library, optional PyTorch only for offline training.

**Spec:** `docs/superpowers/specs/2026-09-07-nnue-phase-design.md`

## Global Constraints

- Preserve `piece-square-v1`, its `768->128->32->1` shape, payload ordering, and current format-version-2 header.
- Add `piece-square-king-pawn-v2` with exactly 960 inputs and `960->256->32->1` layers.
- Encode pawn context as two colors x eight files x presence/doubled/isolated/passed flags.
- Use int16/int8 payloads and clipped ReLU `[0,127]` with wider intermediate accumulation.
- Keep scalar inference deterministic and make the AVX2-compatible path equal to scalar or safely fall back.
- Keep shared weights immutable, accumulators worker-local, and classical evaluation as the default/fallback.
- Do not edit search service, Position/GameState legality, UCI controller, CMakeLists.txt, or measurement tools.
- Do not spawn subagents; preserve all unrelated working-tree changes.

### Task 1: Red tests for the v2 feature contract

**Files:**
- Modify: `tests/nnue_boundary_tests.cpp`
- Modify: `tests/evaluation_boundary_tests.cpp`

**Interfaces:**
- Consumes: Existing v1 `NnueNetwork`, `NnueLoader`, and `EvaluationFeatureExtractor` APIs.
- Produces: Assertions for v1 compatibility, v2 feature dimensions/flags, golden vectors, and invalid manifests that drive the implementation.

- [ ] **Step 1: Add a failing v2 feature-index test.** Construct a FEN with kings, doubled/isolated/passed pawns, call the public v2 encoder, and assert the exact 960 binary positions for the four file flags.
- [ ] **Step 2: Add a failing v2 serialization test.** Require `synthetic_v2()` to serialize and round-trip with feature set `piece-square-king-pawn-v2`, layer sizes `{960,256,32,1}`, and a stable payload hash.
- [ ] **Step 3: Add failing invalid-network tests.** Mutate v2 feature name, dimensions, quantization, payload length, and payload bytes; assert the specific loader error codes and classical fallback where appropriate.
- [ ] **Step 4: Add failing golden inference tests.** Build a small deterministic network fixture, assert scalar output and clipped activations, and assert scalar/AVX2-compatible equality.
- [ ] **Step 5: Run the focused C++ test target.** Run `ctest --test-dir <configured-build> -R "nnue_boundary_tests|evaluation_boundary_tests" --output-on-failure`; confirm failure is due to missing v2 APIs/behavior rather than a test typo.

### Task 2: Implement Koi-owned feature encoders

**Files:**
- Modify: `src/koi/evaluation_features.hpp`
- Modify: `src/koi/evaluation_features.cpp`

**Interfaces:**
- Consumes: `GameState::position_features()` and existing `EvaluationFeatures`.
- Produces: Stable v1/v2 feature-count constants and `encode_piece_square_v1()`/`encode_piece_square_king_pawn_v2()` helpers returning binary int8 vectors.

- [ ] **Step 1: Add the public feature constants and encoder signatures used by Task 1.** Keep existing classical extraction fields unchanged.
- [ ] **Step 2: Implement the shared 768 non-king piece-square layout.** Skip empty/king pieces and preserve color/type/square ordering.
- [ ] **Step 3: Implement king context.** Set one bit per cached white/black king square and leave invalid squares all-zero.
- [ ] **Step 4: Implement the four pawn-file flags.** Scan pawns by color/file and determine presence, doubled, isolated, and passed status with board-only deterministic rules.
- [ ] **Step 5: Run the focused test target.** Confirm the feature-index tests pass while serialization/inference tests remain red.

### Task 3: Implement versioned loader, serializer, and synthetic networks

**Files:**
- Modify: `src/koi/nnue.hpp`
- Modify: `src/koi/nnue.cpp`

**Interfaces:**
- Consumes: Task 2 encoders and existing v1 container behavior.
- Produces: Feature-set-specific constants/spec validation, `NnueNetwork::synthetic_v2()`, deterministic serialize/load behavior, and manifest checksum validation.

- [ ] **Step 1: Add legacy and v2 constants without changing legacy names’ meaning.** Expose both layer shapes/counts and the exact feature-set strings.
- [ ] **Step 2: Refactor manifest validation around a feature-set spec.** Accept v1 and v2 under format version 2; reject unknown sets, shape mismatches, unsupported quantization, malformed lengths, and unsafe payload sizes.
- [ ] **Step 3: Preserve payload ordering and implement v2 array sizing.** Use the manifest-selected dimensions for all arrays and recompute payload checksum deterministically on serialization.
- [ ] **Step 4: Add deterministic v2 synthetic weights.** Keep the existing `synthetic()` v1 fixture unchanged and add a distinct v2 fixture with bounded int16/int8 values.
- [ ] **Step 5: Run round-trip, checksum, and invalid-network tests.** Verify v1 bytes/shape still pass and v2 serialization tests move green.

### Task 4: Implement scalar and AVX2-compatible worker inference

**Files:**
- Modify: `src/koi/nnue.hpp`
- Modify: `src/koi/nnue.cpp`
- Test: `tests/nnue_boundary_tests.cpp`

**Interfaces:**
- Consumes: Task 2 encoders and Task 3 validated network specs.
- Produces: `NnueInferencePath`, worker overloads for scalar/AVX2-compatible/automatic inference, clipped-ReLU accumulators, immutable shared weights, and classical fallback behavior.

- [ ] **Step 1: Add path-selection API and red path-equivalence assertions.** Keep existing two-argument `evaluate` calls source-compatible through an automatic default.
- [ ] **Step 2: Implement the scalar reference for both manifest feature sets.** Select the correct encoder and dimensions from the validated network, clip both hidden layers to `[0,127]`, and sign the final score by perspective.
- [ ] **Step 3: Implement the guarded AVX2-compatible first-layer path.** Vectorize contiguous int16 feature-major blocks only when compiled/runtime-supported; otherwise call scalar. Share the exact second-layer/output/clipping semantics.
- [ ] **Step 4: Verify accumulator ownership and concurrent worker use.** Require separate workers to retain independent hidden/bottleneck vectors while reading one const shared network.
- [ ] **Step 5: Run focused C++ tests.** Confirm golden scores, path equality, saturation, v1 compatibility, and classical fallback all pass.

### Task 5: Add deterministic offline NNUE export/training boundary

**Files:**
- Modify: `tools/tune_eval.py`
- Modify: `tests/tune_eval_test.py`
- Create: `tests/nnue_training_test.py`

**Interfaces:**
- Consumes: Existing corpus parser/canonical hashes and Task 3’s binary container contract.
- Produces: `tune_eval.py nnue` CLI with synthetic export, optional backend/device selection, network output, and stable JSON provenance sidecar.

- [ ] **Step 1: Add failing CLI tests.** Invoke the synthetic boundary with tiny train/validation/holdout files and assert deterministic network bytes plus required metadata keys/values.
- [ ] **Step 2: Add an explicit v2 serializer in Python.** Pack little-endian weights/biases, the exact C++ header strings, and SHA-256 payload bytes; use only the standard library for synthetic mode.
- [ ] **Step 3: Add minimal CLI validation.** Require all three splits, output path, and supported backend/device values; accept `--seed` and record the normalized command and hyperparameters.
- [ ] **Step 4: Add optional GPU/backend metadata.** `auto`/`torch` may inspect PyTorch and CUDA only when requested; missing optional libraries produce an actionable offline error and never affect engine runtime.
- [ ] **Step 5: Run Python tests and repeat export.** Verify no-GPU synthetic success, byte-identical repeated output, split/corpus hashes, seed, backend/device, command, and network hash.

### Task 6: Focused verification and scope audit

**Files:**
- No additional production files; inspect only the allowed diff.

- [ ] **Step 1: Run exact focused C++ tests** for `nnue_boundary_tests` and `evaluation_boundary_tests` with `--output-on-failure`.
- [ ] **Step 2: Run exact Python tests** for `tests/tune_eval_test.py` and `tests/nnue_training_test.py`.
- [ ] **Step 3: Run configured NNUE/evaluation CTest targets** and record unavailable build/toolchain prerequisites without changing CMake.
- [ ] **Step 4: Audit `git diff --name-only`** and confirm no search, legality, UCI, CMake, or measurement files were modified by this task.
- [ ] **Step 5: Report exact commands, counts, changed files, optional-library status, and any build prerequisites.**
