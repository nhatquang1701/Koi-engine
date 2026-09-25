# Implementation plans

Chronological record of Koi implementation plans. Paths are intentionally
stable because plans and verification records link to each other.

| Document | Purpose |
| --- | --- |
| `koi-engine-v1.md` | v1 Lucas Chess UCI random-move baseline. |
| `2026-08-31-future-architecture.md` | Layered architecture and native position ownership. |
| `2026-08-31-next-strength-roadmap.md` | Strength and compatibility roadmap. |
| `2026-08-31-search-optimization.md` | Performance and UCI speed controls. |
| `2026-08-31-uci-lucas-compatibility.md` | Lucas Chess UCI compatibility. |
| `2026-09-04-elo-intelligence-sprint.md` | Data-driven Elo intelligence sprint. |
| `2026-09-04-elo-opening-book.md` | Elo improvement and external opening book. |
| `2026-09-05-en-croissant-intelligence.md` | En Croissant intelligence roadmap. |
| `2026-09-05-long-horizon-elo-roadmap-task-1.md` | Long-horizon Elo roadmap, task 1. |
| `2026-09-05-long-horizon-elo-roadmap-task-4.md` | Long-horizon Elo roadmap, Syzygy task 4. |
| `2026-09-06-koi-measurement-forensics.md` | Measurement and forensic corpus tooling. |
| `2026-09-07-nnue-phase.md` | Opt-in NNUE phase. |
| `2026-09-09-strength-improvement.md` | Strength improvement pass. |
| `2026-09-10-strength-push.md` | Strength push. |
| `2026-09-11-koi-architecture-stage1.md` | Architecture stage 1: state ownership seam. |
| `2026-09-11-koi-architecture-stage2.md` | Architecture stage 2: search lifecycle and stack. |
| `2026-09-11-koi-architecture-stage3.md` | Architecture stage 3: ordering and policy seams. |
| `2026-09-11-strength-continuation.md` | Strength continuation. |
| `2026-09-12-koi-architecture-stage4.md` | Architecture stage 4: evaluation/NNUE seam. |
| `2026-09-12-koi-architecture-stage5.md` | Architecture stage 5: TT, budget, parallel runtime. |
| `2026-09-12-search-strength-pass.md` | Search strength pass (staged picker and selective pruning). |
| `2026-09-16-uci-controller-improvement.md` | UCI controller robustness, option registry, and ponder continuation. |
| `2026-09-16-strength-program.md` | Strength program: hot-path speedups and an end-to-end NNUE pipeline. |
| `2026-09-17-test-suite-hardening.md` | Test-suite reliability, determinism, parallel CTest, and CI hardening. |
| `2026-09-17-nnue-studio.md` | NNUE Studio: GUI, headless training, validation, and install. |
| `2026-09-17-nnue-evaluation-overhaul.md` | King-bucketed NNUE, incremental inference, container v4, and classical evaluation modernization. |
| `2026-09-18-nnue-studio-ui.md` | NNUE Studio UI: correctness fixes, training telemetry, and run-list UX. |
| `2026-09-18-nnue-bullet-training.md` | Bullet GPU trainer: dependencies, dataset conversion, exact feature parity, campaign, bug and docs audits. |
| `2026-09-19-engine-v2.md` | Koi Engine v2: SPRT harness, Studio adoption and redesign, NNUE/TT/SMP/search/movegen modernization. |
| `2026-09-19-syzygy-endgames.md` | Endgame evaluation scaling and opt-in Syzygy interior WDL probing. |
| `2026-09-19-nnue-v5-threat-architecture.md` | NNUE v5: threat features, dual-perspective head, container v5, dataset v2, trainers, and Studio. |
| `2026-09-20-gpu-nnue-inference.md` | GPU NNUE inference (sm_61 PTX pipeline, driver API, batched evaluator, verification). |
| `2026-09-23-linux-compatibility.md` | Linux x86-64 compatibility: portable build, engine platform layer, GPU, tooling, and CI. |
| `2026-09-24-engine-hardening.md` | Engine hardening: stability, correctness, robustness/scale, protocol surface, and strength foundations. |
| `2026-09-25-speed-program.md` | Speed program: measurement, TT/movegen/eval/search hot paths, and Windows MSVC PGO. |
