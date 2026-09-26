# v1.0.0 release candidate working summary (2026-09-26)

## Source and workspace

- Isolated worktree: `C:\Users\ntATh\.codex\worktrees\v1-release-candidate\Koi engine`, branch `codex/v1-release-candidate` from `14e4b99`.
- Original checkout at `C:\Users\ntATh\AI test\Koi engine` has an untracked repository exploration summary; leave it intact.
- Accepted plan: `docs/superpowers/plans/2026-09-26-v1-release-candidate.md`. No public publication authorized.

## Architecture and release paths

- `src/main.cpp` boots the CPU-selected UCI engine; `src/koi/uci_controller.cpp` advertises options and manages search lifecycle; `SearchService` drives async search.
- `CMakeLists.txt` sets project version and targets. `tools/build/package_release.ps1` builds archives and manifest. Linux CI tarball job checks a versioned archive path. Windows/Linux CI and `tools/build/release_verify.ps1` provide existing gates.
- `tools/stability/uci_match.ps1` and `sprt_compare.ps1` implement game/strength harnesses. `tests/unit/search/koi_search_tests.cpp` contains 12 current known failures and 3 intermittent cases; `tests/README.md` list is stale (still names a removed IID failure).

## Evidence so far

- Baseline source commit: `14e4b990a4197871d73869e104f2fe48d8694930`.
- Original checkout `cmake --build build/release --config Release` reported `ninja: no work to do`. Existing baseline `build/release/koi-engine.exe` SHA-256 is `5C09DC10E01C3CB73C7442DA1E159DD8BE49F649E29721E591470BACCEC86761`.
- Frozen baseline binaries and hashes are under ignored `artifacts/baselines/14e4b99/`. Fresh baseline `ctest --test-dir build/release -C Release -j 8 --output-on-failure` passed 67/67 in 240.29 seconds (2026-09-26).
- GitHub Actions for baseline commit `14e4b99`: Windows run `36221411583` and Linux run `36221411605` both concluded success. This is baseline evidence, not candidate evidence.
- Previous speed program (2026-09-25) recorded Release 67/67, Debug 67/67, ASan subset 26/26 and release verification PASS, but no Elo claim or manual GUI gate. Those are historical, not fresh release results.

## Current work

- Packaging/version task and Elo-option task delegated on disjoint files; parent handles baseline, release gates, integration, and final evidence.
- No fresh candidate build/tests or games have run yet.
- `wsl --list` showed only `docker-desktop`. Docker Desktop was launched for possible local Linux verification; daemon readiness remains unconfirmed. No En Croissant installation found under common Program Files paths.

## Decisions and risks

- Ruling: Keep known strength XFAILs as documented limitations unless they reveal a reliability or legality blocker; fixing all search-quality items would turn this into a new strength program. Cost if wrong: first release has weaker tactical choices than desired.
- Ruling: A frozen baseline binary is valid only with an exact source commit, clean build result, and recorded hash; historical CTest evidence alone does not qualify as candidate evidence. Cost if wrong: a comparison may use an unproven baseline binary.

## 2026-09-26 release gate checkpoint

- The candidate Windows Release CTest passed 69/69 after the benchmark version expectation was corrected. The 64 untimed single-thread tactical position records are byte-equivalent between frozen baseline and candidate profiles; only the top-level build identity differs (1.1.0 versus 1.0.0).
- The Linux game workflow and cross-platform artifact validator are now in the worktree. Focused validator tests passed 9/9, and a real 32-game Cute Chess pilot passed the validator with all 32 curated openings covered.
- A real two-game Cute Chess pilot exposed that the existing harness alternated Koi colors despite `-KoiColor white`. The fixed-color harness now sends `-noswap` and checks actual white/black counts; the real fixed-white pilot completed two white games. Regression tests were red before the fix and green after.
- Cute Chess `-maxmoves` counts **full moves after the EPD opening**. A real one-game opening pilot with `-MaxMoves 1` ended at two plies. The later 320-game Linux matrix exposed that Cute Chess 1.5.1 can delay its cap check by one ply when a move has no reported search depth; the corrected validator permits at most 61/13 plies for `-maxmoves 30/6` and rejects longer play.
- The package test first failed because the project's root MIT `LICENSE` was missing. Packaging now includes it, and the focused extracted-archive package test passed.
- The full `release_verify.ps1` gate is running in a Visual Studio developer environment. Debug, ASan, native/shadow differential, Linux candidate CTest, and long game/strength campaigns are still pending.
- Local WSL2 virtualization is disabled. Fresh Linux candidate execution needs the remote GitHub runner. The approved plan excluded pushing; no branch has been pushed or release published.

## 2026-09-26 continuation checkpoint

- The paired 32-game Windows baseline pilots completed for each fixed Koi color at 1,000 nodes, Threads 1, and an 80-ply cap. Each color run has 7 white wins, 1 black win, 3 rule draws, and 21 ply-cap games. From the candidate's perspective this is 8 wins, 8 losses, and 48 draws across 64 games. The pilot does not establish the planned 95% strength bound.
- A Linux workflow audit found the candidate game job passed `-OwnBook:$false` through Bash without quoting. A focused CI-configuration regression failed first; the workflow now quotes that argument and the test passes. No other shell quoting blocker was found in the focused audit.
- The latest Windows `release_verify.ps1` session is still active in session 13766; Debug CTest has not yet produced its final log. Rerun the gate after the strength-analysis test is registered and all source changes are settled.
- `docs/releases/v1.0.0.md` was drafted as candidate release notes. Exact archive hashes and final evidence belong in the ignored artifact report after a clean source commit, so package provenance can report `source_dirty=false`.
- The remote does not contain this candidate branch or workflow. Local WSL2 virtualization is disabled, so fresh Linux execution on GitHub Actions requires an authorized branch push after the clean source commit. No push has occurred.

## Strength gate and validator checkpoint

- The initial 64-game pilot is valid reliability evidence: the artifact validator now accepts the harness's `rule draw` termination, skips UCI `bestmove` only for named injected opening plies, and uses the recorded opening name to disambiguate overlapping curated prefixes. Those fixes were driven by failing focused tests; both real 32-game pilot reports now pass legal replay, process, color, cap, and opening-coverage validation.
- The pilot is **not** valid strength-bound evidence: it set `OwnBook=false` on the candidate while the baseline retained `OwnBook=true` (no packaged book was present, but the effective settings differ). The strength analyzer detects and rejects this difference. New matches will explicitly set equal options on both engines.
- `tools/stability/strength_bound.py` computes the predeclared one-sided exact adverse-pair bound. A pair is adverse when the candidate's average score over the same opening with reversed colors is below 0.5; the test conservatively lower-bounds mean score by `0.5 * (1 - p_upper)` using an exact Clopper-Pearson upper bound on adverse probability. The separate Hoeffding result is diagnostic only. Ply-cap games count as draws. This requires independent identically distributed opening draws and allows dependence within each color pair.
- `tools/stability/sample_release_openings.py` generated `tests/data/openings/openings-release-strength-160.txt` *before new matches*: 160 independent seeded draws with replacement, each choosing a curated root uniformly and then a legal one-ply continuation uniformly. Seed 20260926; sample file SHA-256 `D4E7335FF7D69C86941AB1C8354903E89B17AA8FB13FDB569E76656C047E5E1F`. Each draw will be played once as candidate White and once as candidate Black. A zero-adverse 160-pair result has a one-sided 95% lower Elo bound above -10; actual outcomes remain unmeasured.
- `release_verify.ps1` Debug CTest completed 69/69 in 2280.61 seconds, but its binary predates the final test edits. Release configure/build has begun. After the script finishes, rebuild and rerun both configurations with the newly registered Python tests.

## 2026-09-26 final validation checkpoint

- A final review reproduced three analyzer gaps: empty JSON move arrays, PGN movetext inconsistent with JSON, and a forged `replay_legal=true` flag on an illegal move. Focused tests failed for all three before the fix. A fourth RED test showed that an opening-only game could still pass. `strength_bound.py` now requires a played engine move, replays every move with the independent `koi_chess` board, checks the declared opening prefix, and compares each adjacent PGN game's full UCI movetext and result with JSON. The focused suite passed 12/12 and the stability Python suite passed 26/26. A real equal-options two-game dry-run artifact still analyzes as `inconclusive` with the new checks, which is expected for one pair.
- Ruling: the 320-game Windows candidate-versus-baseline campaign will complement the 320-game Linux Stockfish workflow to satisfy the plan's 600-game cross-platform minimum. Both campaigns must use the exact package hashes. Cost if wrong: this mix of opponents and clocks may underrepresent some engine failures.
- Ruling: the 64-game earlier Windows pilot remains reliability-only because baseline `OwnBook` differed. Cost if wrong: excluding it reduces sample size, but avoids a biased strength claim.
- Fresh source checks require a Visual Studio developer environment. An attempted direct `cmake --build build/release` in an ordinary shell damaged that build cache's compiler/linker detection; subsequent configure used the Visual Studio 3.31.6 CMake and explicit MSVC `cl`, `link`, and `lib` paths. This is a local build environment issue, not an engine failure. The fresh Release rebuild and 71/71 CTest passed; Debug CTest is running. The rebuilt candidate launcher hash is `133832CFB5B642F49958A303AC97847B5EA663299E7C2501BE8486EABF9AAAF1` and replaces the earlier `81C914...` hash for every subsequent match and package gate.

## Final review repair checkpoint

- A read-only review found that `validate_release_candidate_games.py` accepted result-only Cute Chess or UCI match artifacts and incomplete UCI process-status objects. Four new focused tests failed first, then passed after the validator required at least one engine ply after the opening and both named `koi`/`opponent` clean shutdown statuses. Both real 32-game Windows pilot bundles still pass the stricter validator. The stability Python suite now passes 30/30.
- The review also found that the optional `sprt_compare.ps1` wrapper gave candidate and baseline different seed/book settings. Its command-construction test failed first. The wrapper now sets candidate seed 0 and disables both books. `uci_match.ps1` gained an explicit optional `-OpponentOwnBook` parameter, recorded in match configuration; a real harness integration test failed on the missing parameter first and passed after implementation. The README's description of the combined SPRT rule was corrected to match its aggregate LLR implementation and labels that utility exploratory. The v1 strength decision still uses the fixed paired-opening exact bound.
- Ruling: the optional SPRT wrapper is repaired even though the release strength campaign uses `run-strength-matches.ps1` directly. Cost if wrong: one more harness option and test to maintain; leaving unequal settings would make future Koi-to-Koi comparison evidence misleading.
- The first Debug 71-test CTest run was already underway when the final review repairs landed. A fresh complete Debug/Release rerun on the settled source remains required before the clean source commit.

## Settled-source checkpoint

- The settled-source Windows Release CTest passed 71/71 without retry in 219.73 s; the settled-source Debug CTest passed 71/71 without retry in 882.69 s. Full logs are in ignored `artifacts/verification/v1-release-candidate/ctest-release-settled.log` and `ctest-debug-settled.log`.
- The final rebuilt Windows candidate launcher SHA-256 is `133832CFB5B642F49958A303AC97847B5EA663299E7C2501BE8486EABF9AAAF1`. Its fresh 64-position tactical benchmark profile is identical, position for position, to the frozen baseline; `artifacts/verification/v1-release-candidate/tactical-parity-final.json` records the exact binary and profile hashes.
- Local Docker Desktop's Linux daemon is unavailable (`docker version` cannot connect to `dockerDesktopLinuxEngine`); WSL2 virtualization was already found disabled. Fresh Linux candidate evidence still requires the remote runner and a branch push outside this plan's authorized scope.

## GitHub pre-release continuation

- The user authorized a GitHub pre-release titled `1.0.0` and then requested a push to the main branch. This repository has no `main` branch: `koi-engine` is its GitHub default branch. Commit `b71b35d` was pushed there by fast-forward; the original checkout's untracked exploration summary remains untouched.
- GitHub Actions run `36232853578` built and packaged Linux successfully at `b71b35d`, but all four game cells failed while installing the `cutechess` apt package, which Ubuntu 24.04 does not provide. No games ran in that attempt.
- Ruling: Obtain Cute Chess 1.5.1 from its upstream x86-64 AppImage and verify the upstream SHA-256 `d9448693e45bd57f1aeb32c46e94466894cd7cc5b6937effd285a02e871387b5`; invoke its bundled CLI through the AppImage's documented `cli` dispatch in extract mode. This retains a pinned binary and avoids FUSE. Cost if wrong: the runner may still lack a runtime dependency, so the workflow's CLI smoke must prove launch before games run.
- The focused CI-configuration regression failed on the old apt install command, then passed after the pinned AppImage workflow change. Fresh Linux package, game, and general CI evidence is required for the eventual release commit.

## Linux game artifact cap correction

- Run `36233389140` at `a276c18` built the Linux archive and launched all four game cells with the pinned Cute Chess 1.5.1 CLI. Both 32-game longer-clock cells passed on GitHub. Both 128-game short-clock cells completed play but the artifact validator rejected a few 61-ply games under its 60-ply assumption. Downloaded reports show clean Cute Chess exits and no failure lines.
- Root cause from upstream Cute Chess 1.5.1: `GameAdjudicator::addEval` returns before checking `m_maxGameLength` when `eval.depth() <= 0`. The 61-ply observations occurred from both White-to-move and Black-to-move EPD roots, so a starting-side-only explanation was incorrect.
- Ruling: Permit one delayed cap-check ply (up to `2 * -maxmoves + 1`) and reject two or more extra plies. This keeps the bounded-game gate strict while accepting observed legal, completed games. Cost if wrong: repeated depthless moves could delay Cute Chess further; such a future game remains a gate failure to investigate, not an automatic waiver.
- Focused synthetic PGN regressions were RED before the final correction and GREEN after it: Black/White roots accept 61 plies at `-maxmoves 30` and reject 62. The focused suite passed 19/19; the broader stability Python suite passed 32/32. All four downloaded real game bundles (128+128+32+32) pass independent local validation with the corrected rule. The workflow metadata and CLI summary now state the one-ply allowance. A new final-source CI run is required before publication.
- The `a276c18` general Linux CTest run `36233382132` passed all five jobs (GCC, Clang, modules-off, no-retry flake, tarball), and Windows CTest run `36233382239` passed all four jobs (Release, Debug smoke, ASan, shadow differential). These are evidence for the prior source commit; a new final-commit run will confirm the validator correction.
