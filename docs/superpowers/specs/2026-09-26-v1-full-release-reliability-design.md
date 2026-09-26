# Koi Engine 1.0.0 Full-Release Reliability Design

## Intent and release identity

Promote the published `v1.0.0` pre-release to a full release after clearing every current search XFAIL/intermittent case and running broader reliability gates. The current tag points to `84dd8d57f62ba96315e6634be45148f5bd39fc29`; the user explicitly chose to move this public tag to the final commit instead of issuing 1.0.1. Save the old tag SHA and four release assets locally before changing it. Commit directly on the GitHub default `koi-engine` branch with detailed messages, create no new branch, and revert a pushed regression instead of resetting public history.

Change the runtime and advertised UCI default `OwnBook` to `false`. An explicit `setoption name OwnBook value true` remains supported. The classical CPU evaluator remains the default and the UCI option surface stays otherwise stable.

## Search behavior

Clear all 12 `known_failures` and 3 `intermittent` entries in `koi_search_tests`. Work in five causal groups: root forcing/short-search fallback; king safety/quiescence; selective pruning/verification; threaded MultiPV/TT behavior; tactical/PGN fixtures. Write or correct deterministic oracles before production changes. Timing-dependent exact-move tests may become fixed-node/depth behavior tests plus separate clock/cancellation invariants; SMP tests may assert legal, complete, coherent rank ordering instead of impossible schedule-independent deeper PV identity. Every original behavior must retain a meaningful test, and the exemptions must be empty at completion. Do not delete a case simply to obtain green output.

## Available modes and evidence

Hard-gate classical CPU, CPU NNUE v4/v5, and actual GPU NNUE v5 on the local GTX 1060. Generate deterministic synthetic v4/v5 networks in CI with a test-only tool; record SHA-256 and do not commit or package the generated networks. Matches must prove `EvalFile` acceptance; GPU matches must prove service activation. Require local `koi_gpu_probe` success and GPU unit parity/concurrency cases without skips. Real book and Syzygy assets are absent, so gate those optional data paths with their existing fixtures and extracted-package behavior.

Start Docker Desktop if needed. A local Docker Linux daemon, `hello-world`, and an Ubuntu 22.04 build/package/extracted-archive smoke are hard release gates. Use the existing clean managed worktree as package staging while committing only on `koi-engine`; preserve the original checkout's untracked exploration summary. Require Windows Release/Debug, ASan, native/shadow differential, Linux GCC/Clang/modules-off/tarball, and Windows/Linux no-retry flake checks on the exact final source.

## Game and strength decision

Run at least 1,320 newly recorded games against exact final packaged binaries with zero crash, illegal move, protocol timeout, missing/duplicate `bestmove`, or unclean process exit:

- 640 Windows classical candidate-versus-frozen-pre-release games: 320 new IID sampled openings, each played in both colors at 1,000 nodes, Threads 1, 80-ply cap, and equal seed/book/hash/speed settings. Generate and freeze the sample with seed `20260927` before matches. Use the one-sided exact 95% adverse-pair bound; require lower Elo bound greater than −10. At 320 pairs, at most four adverse pairs can pass. Do not selectively discard or adaptively resample results.
- 320 Linux classical games: the existing 128/128 White/Black 1+0 Threads 1 and 32/32 White/Black 5+3 Threads 4 matrix, with curated-opening coverage.
- 360 optional-mode games: Windows CPU NNUE v4 60, Windows CPU NNUE v5 60, Windows GPU NNUE v5 120, Linux CPU NNUE v4 60, Linux CPU NNUE v5 60. Split each 60-game cell into 40 short 1+0 and 20 longer 5+3, evenly by color; GPU's 120 games split 80/40, evenly by color, at Threads 4. CPU short games use Threads 1 and longer games Threads 4. Use fixed, hash-recorded opening schedules and bounded games.

Every game artifact records executable, package, evaluator-network, configuration, and source hashes and is independently replayed. A failed or inconclusive strength bound blocks release and requires a code decision plus an entirely new predeclared sample after any fix.

## Publication and rollback

Only after all gates pass, update candidate wording to full-release wording, package from a clean source commit, and keep the evidence locally. The GitHub release body contains product/install information only; its assets are exactly Windows/Linux archives and matching `.sha256` files, with no completion-evidence files or verification claims. Temporarily draft the existing release, move `v1.0.0` to the final default-branch commit, replace the four assets, set `prerelease=false`, and publish. Read back title, tag target, flags, asset names, and downloaded hashes. If promotion fails, restore the saved old tag, assets, body, and pre-release flags before making the release public again.
