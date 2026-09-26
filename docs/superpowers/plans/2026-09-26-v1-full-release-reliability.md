# Koi Engine 1.0.0 Full-Release Reliability Implementation Plan

> **For agentic workers:** Use superpowers:subagent-driven-development or superpowers:executing-plans task by task. Checkboxes track work; preserve the dated working summary across compaction.

**Goal:** Deliver a stable 1.0.0 release with no known search exceptions, an opt-in opening book, all available evaluator modes tested, and exact-hash Docker/CI/game evidence.

**Architecture:** Keep the existing UCI and search boundaries. Fix search mechanisms in focused groups, generate test-only NNUE fixtures, extend match evidence, then verify packaged binaries before controlled promotion of the existing public tag.

**Tech Stack:** C++26/CMake/Ninja, PowerShell, Python, Docker Desktop with Ubuntu 22.04, GitHub Actions.

**Spec:** `docs/superpowers/specs/2026-09-26-v1-full-release-reliability-design.md`.

## Global constraints

- Commit only on `koi-engine`, with detailed commit messages; create no new branch. Do not stage or delete the user's untracked exploration summary. Revert a pushed regression rather than resetting history.
- Preserve protocol-clean stdout and exactly one `bestmove`; classical CPU stays default. Change only `OwnBook` to default `false` and keep explicit opt-in functional.
- No network, book, tablebase, generated game, or evidence artifact is committed or included in release archives. No completion evidence is posted in the GitHub release.
- The local Docker Ubuntu 22.04 package smoke is mandatory. All final gates use the same clean source commit and exact archive/binary hashes.

## Tasks

1. [ ] **Freeze the public pre-release and establish the Docker gate.** Record old tag commit `84dd8d5`, four asset hashes/body locally; verify Docker Linux daemon and `hello-world`. Preserve the original untracked summary. Acceptance: baseline backup is readable and Docker reports Linux x86-64.
2. [ ] **Make books opt-in.** First change tests/handshake expectations and observe failure; then change UCI, harness, package, and docs defaults to `OwnBook=false`. Verify explicit `true` still selects a legal fixture book move. Acceptance: focused UCI/book/process/package tests and updated handshake pass.
3. [ ] **Create evaluator fixtures and mode attestation.** Add `koi-nnue-fixture --arch v4|v5 --output <path>` around existing synthetic networks and serializer; test deterministic bytes, loader acceptance, and bad args. Extend match records/validation with evaluator mode and network SHA-256; require NNUE acceptance and GPU activation evidence. Acceptance: generated fixtures hash identically across Windows/Linux, no generated network is packaged, and a forged mode or fallback is rejected.
4. [ ] **Fix search groups incrementally.** For each of root forcing/fallback, king safety/quiescence, pruning/verification, threaded MultiPV/TT, and tactical fixtures: write a meaningful failing deterministic regression, fix the shared cause, run focused search/runtime/UCI tests, and make a detailed main-branch commit. Correct timing/SMP oracles only with replacement coverage. Acceptance: all 15 original behaviors pass without `known_failures` or `intermittent` exemptions; no XPASS/XFAIL/SKIP/UNSEEN.
5. [ ] **Make flake checks strict.** Add an independent Windows no-retry job and preserve the Linux no-retry job. Run each formerly intermittent case 50 times in fresh Windows Release/Debug and Linux Release processes with `KOI_TEST_RETRIES=1`, followed by complete search shards and time-safety/process suites. Acceptance: every run passes without retry or protocol defect.
6. [ ] **Build and test all available modes.** Run complete Windows Release/Debug, ASan, shadow-diff, perft, rules, GPU probe/parity (no skips), CPU NNUE v4/v5 load/soak, and book/Syzygy fixtures. Run the local Docker Ubuntu 22.04 build/package/extracted auto/generic UCI smoke and final-commit Linux GCC/Clang/modules-off/tarball CI. Acceptance: every required job passes and package manifests show `source_dirty=false` and the final commit.
7. [ ] **Run and validate the 1,320-game matrix.** Generate a new 320-opening sample with seed `20260927` before play; run 640 paired Windows classical, 320 Linux classical, and the specified 360 NNUE CPU/GPU games. Validate all JSON/PGN, hashes, options, color, clock, threads, legal moves, completion, and process status. Acceptance: zero reliability failures, ≥1,320 games, and exact one-sided 95% lower bound > −10 Elo against the frozen pre-release binary.
8. [ ] **Promote the existing release.** Update stable-release wording and rebuild four archive/checksum assets from the final clean commit. Temporarily draft the current release, force-move its existing bare tag `1.0.0` to the final commit, replace four assets, and publish title `Koi Engine v1.0.0` with `prerelease=false`. Keep evidence local. Acceptance: remote tag/default branch/source manifest match, only four assets exist, downloaded hashes match, and release body contains no completion evidence. Restore the saved pre-release state if publication fails.

## Review focus

- Short-clock cancellation or replacement must not emit duplicate, stale, or absent `bestmove`.
- SMP ties and warmed transposition-table state must preserve legal, unique, coherent MultiPV results without assuming schedule-independent deep PVs.
- NNUE/GPU tests must detect rejected networks and silent fallback rather than merely a clean process exit.
- Docker and CI packages must use exact clean-source provenance while preserving the original checkout's untracked summary.
- The public tag transition must never expose mismatched source and archive assets; keep the release drafted during replacement.
