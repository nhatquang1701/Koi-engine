# NNUE and evaluation overhaul plan

Goal: replace the current 960→256→32→1 full-recompute NNUE with a
king-bucketed feature-transformer network (`halfka-king-bucket-v1`, 9216
inputs, CReLU pair products, eight output buckets, incremental dual
perspective accumulators, container version 4, scalar reference plus AVX2
fast paths), train it on a larger Stockfish-labeled corpus, modernize the
classical evaluator's internal duplication without changing its default
behavior, and prove the whole program with the repository verification
gates — all while keeping the classical evaluator the engine default, the
UCI handshake byte-identical, and v1/v2/v3 containers loadable.

**Spec:** `docs/superpowers/specs/2026-09-17-nnue-evaluation-overhaul-design.md`

## Hard constraints

- Windows x64, MSVC only; build through the repository CMake/Ninja trees.
- stdout stays protocol clean with exactly one `bestmove` per search.
- The 64-position tactical gate must stay 64/64 for the classical
  evaluator; NNUE candidates are reported by match count.
- perft and shadow-diff remain the rules oracle.
- Never weaken or delete an assertion to obtain a green run; XFAIL entries
  are removed only when the behavior is genuinely fixed (XPASS is fatal).
- Classical evaluation remains the engine default; NNUE stays opt-in with
  a safe fallback.
- `KOI-NNUE` v1/v2 serialization stays byte-identical; v2/v3 stay loadable.
- The `uci` handshake stays byte-identical (no new options, stable order).
- The engine stays Python-independent at runtime.
- Elo, CPL, and NPS numbers are local reports, not CI thresholds and not
  Elo claims.
- Do not commit `.opencode/`, generated networks, or corpus artifacts.
- Commit at every phase boundary with a descriptive message.

## Global decisions

- New feature set: `halfka-king-bucket-v1`, 12 own-king buckets × 12 planes
  × 64 squares = 9216 binary inputs, sparse active indices capped at 64.
- New architecture: 9216 → hidden (default 1024, even, ≥32) → CReLU pair
  products (hidden/2) → 8 piece-count output buckets × (hidden/2 → 1 int8)
  → centipawns with a stored output shift.
- Container v4 keeps the `KOI-NNUE` magic and adds explicit shift bytes;
  the loader accepts versions 2, 3, and 4.
- Incremental accumulators are dual-perspective, indexed by plies of the
  game history, with a full refresh on king-bucket change or any doubt;
  scalar full refresh stays the correctness reference.
- Training is CPU-only on this host (modern CUDA wheels dropped Pascal);
  corpus expansion runs detached while implementation phases proceed.
- No UCI surface changes: `EvalFile` semantics stay exactly as documented;
  new identity information is additive (`info string`, benchmark profile).
- Ambiguity resolves toward the documented constraints above and the
  decision is recorded in the verification record.

## Phases

- [x] **Phase 0 — Baseline and scaffolding.** Fresh Release and Debug
  builds; full Release CTest; Debug smoke (`-LE heavy`); hard-suite
  benchmark rows at Threads 1/2/4; timed Threads=1 NPS for classical and
  the existing `koi-sf-v1` network; NNUE tactical-gate match count; record
  the pre-existing `release_verify.ps1` thread-parity gap; write this plan,
  the design spec, the verification record, and the three README index
  rows. Commit.
- [x] **Phase 1 — Dataset pipeline v2.** Fix generator defects (position
  dedup seeded from an existing file, label-depth hint mismatch), add a
  resumable `koi-dataset-v1` binary encoder with sparse feature indices,
  add pure-Python unit tests for the generator/encoder, and launch
  detached corpus expansion (three Stockfish workers) in the background.
  Commit.
- [ ] **Phase 2 — Feature set and container v4 (C++).** Implement
  `halfka-king-bucket-v1` in `evaluation_features.*` with exact golden
  vectors, and container v4 in `nnue.hpp`/`nnue.cpp` (manifest, payload,
  validation, deterministic serialization) with v2/v3 back-compat and
  complete loader error-path tests. Commit.
- [ ] **Phase 3 — Inference and SIMD.** Scalar reference inference for v4
  (CReLU pairs, output buckets, output shift) with golden integer scores;
  AVX2 accumulator and second-layer fast paths behind overflow guards;
  scalar↔AVX2 parity property tests; NPS measurement against the Phase 0
  baseline. Commit.
- [ ] **Phase 4 — Incremental accumulators.** Dual-perspective accumulator
  slots per ply, restart-free updates on make/unmake with king-bucket
  refresh, hooks from `SearchContext`, and parity tests that compare
  incremental results against full refresh over random game trees
  (including quiescence, null moves, and fallback paths). Commit.
- [ ] **Phase 5 — Trainer overhaul.** New `train_nnue_koi.py` (encoder
  parity with C++, binary dataset loader, AdamW with schedule, integer
  quantization with shift search, v4 export, metadata schema v2,
  determinism tests), legacy v2/v3 training kept green, studio backend and
  presets wired to the new trainer. Commit.
- [ ] **Phase 6 — Training campaign and strength gates.** Train candidates
  on the existing corpus first and the expanded corpus when ready; gates:
  64-position match count, equal-node color-balanced A/B versus classical
  and versus `koi-sf-v1`, NPS, and fixed-depth classical non-regression.
  Record nets and reports under `artifacts/`. Commit.
- [ ] **Phase 7 — Classical evaluation modernization.** Behavior-preserving
  deduplication (single material source, attack-table routing for feature
  generation and mobility, unified insufficient-material/dead-position
  handling) plus `koi-eval-features` and `tune_classical.py` producing a
  real generated parameter header. Tuned values are adopted only through
  the tactical/A-B/adoption gates, otherwise reverted with evidence.
  Commit.
- [ ] **Phase 8 — Verification and documentation.** Run
  `tools/build/release_verify.ps1` end to end, resolve the documented
  harness gap with evidence, update README/tests README/tools README and
  the verification record, and confirm all index rows. Commit.

## Deferred

- GPU training (host/GPU/toolkit mismatch), the Rust bullet backend, Lazy
  SMP and persistent workers, bitboard move generation, TT prefetching,
  threat and HalfKA_hm feature families beyond the chosen design, PSQT
  auto-tuning, and any change that would make NNUE the default evaluator.

## Verification

See `docs/superpowers/verification/2026-09-17-nnue-evaluation-overhaul.md`.
