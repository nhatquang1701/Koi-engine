# Koi Engine 1.0.0 full-release working summary (2026-09-26)

## Source and decisions

- Default branch `koi-engine` at start: `84dd8d57f62ba96315e6634be45148f5bd39fc29`; public pre-release `v1.0.0` points there. User approved force-moving that public tag only after all full-release gates. The release currently has only Windows/Linux archives and their `.sha256` files; evidence must remain local, never in its body/assets.
- User requires: zero of 12 known search XFAILs and 3 intermittent exemptions; review invalid timing/SMP assertions but preserve behavior coverage; hard-gate classical CPU, CPU NNUE v4/v5, and actual GPU NNUE v5; use fixtures for unavailable real book/Syzygy assets; ≥1,320 fresh games; one-sided 95% strength lower bound > −10 Elo; mandatory local Docker package smoke; `OwnBook=false` default.
- Commit only on `koi-engine` with detailed messages and no new branch; revert a pushed regression. Preserve untracked `docs/superpowers/verification/2026-09-26-repository-exploration-working-summary.md`. Existing managed worktree may be fast-forwarded for clean packaging but receives no commits.
- Spec: `docs/superpowers/specs/2026-09-26-v1-full-release-reliability-design.md`; plan: `docs/superpowers/plans/2026-09-26-v1-full-release-reliability.md`.

## Architecture and existing evidence

- Search work centers on `src/koi/detail/search_runner.cpp`, `search_context.cpp`, policy/TT code, and `tests/unit/search/koi_search_tests.cpp`. The 15 exemptions fall into root forcing/fallback, king safety/quiescence, pruning, threaded MultiPV, and tactical fixtures.
- Existing pre-release at `84dd8d5` passed Windows and Linux CI, 640 games, and the prior paired −10 Elo gate. Those are baseline evidence; source changes require fresh full-release binary/hash/game evidence.
- Host has NVIDIA GTX 1060 sm_61, CUDA 12.9 `nvcc`, GPU-enabled Release build. `koi_gpu_probe.exe` passed and `gpu_nnue_tests.exe` reported 5/5 with zero skips. Local ignored v4 and v5 network artifacts exist, but the full gate will generate deterministic test fixtures instead.
- Docker Desktop initially was stopped in this shell. Root launched it hidden, confirmed `docker info` reports `linux x86_64 29.8.0`, and `docker run --rm hello-world` passed. Full Ubuntu 22.04 package smoke awaits a settled clean source.

## Active work

- Three independent subagents are editing disjoint areas without committing: OwnBook defaults/docs/tests; deterministic NNUE fixture generator; Windows no-retry CI job. Root owns search mechanisms, Docker/package/game integration, review, and all main-branch commits.
- No full-release source code has been committed yet. Do not rely on test results reported by agents without inspecting diffs and running focused/full verification.
