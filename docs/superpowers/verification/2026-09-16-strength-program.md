# Strength program verification (2026-09-16)

Record for the strength program planned in
`docs/superpowers/plans/2026-09-16-strength-program.md`. Everything here was
measured on this checkout; nothing is projected.

## Environment

- Windows x64, Intel i3-10100F (4 cores / 8 threads, 3.6 GHz), 34.2 GB RAM,
  GTX 1060 6 GB (unused).
- Visual Studio 2022 Community, MSVC 14.44.35207, CMake + Ninja, Release
  (`/O2`, `/arch:AVX2`, LTO).
- Python 3.14.5 with `python-chess` 1.11.2 and `torch` 2.14.0+cpu; Stockfish 19
  oracle; cutechess-cli 1.5.1 (driven through a space-free `C:\koi` junction).

## Changed surface

| Area | Files | Commit |
| --- | --- | --- |
| Per-node copies | `src/koi/position.cpp`, `src/koi/game_state.cpp`, `src/koi/detail/search_runner.cpp` | `7e9d96c` |
| NNUE pipeline | `src/koi/evaluation_features.{hpp,cpp}`, `src/koi/nnue.{hpp,cpp}`, `src/koi/search_service.{hpp,cpp}`, `src/koi/uci_controller.{hpp,cpp}`, `src/main.cpp`, `tools/measurement/gen_training_data.py`, `tools/measurement/train_nnue_sf.py` | `16c8ce7` |
| EvalFile handshake tests | `tests/unit/runtime/uci_controller_tests.cpp` | `e46e4c0` |
| Attack tables | `src/koi/detail/attack_tables.{hpp,cpp}`, `src/koi/position.cpp` | `d21da4d` |
| NNUE v3 container | `src/koi/nnue.{hpp,cpp}`, `tools/measurement/train_nnue_sf.py`, `tests/unit/evaluation/nnue_boundary_tests.cpp`, `tools/engine/koi_bench.cpp` | `03db674` |
| O(1) check flags | `src/koi/game_state.cpp` | `0b7cdb4` |
| Lightweight SEE context | `src/koi/game_state.{hpp,cpp}`, `tests/unit/search/static_exchange_tests.cpp` | `5258065` |

## Speed

- `koi-perft.exe 5` (4,865,609 nodes) completes in 12.21 s, about 398k
  movegen+make/unmake nodes/s.
- Raw stdin/stdout probe, startpos, `Threads=1`, `Hash=256`, `go depth 6`:
  - before the hot-path series: 406,067 nodes in 6,918 ms, 58,697 nps;
  - after the attack tables and the O(1) check flags: 406,067 nodes in
    4,668 ms, 86,989 nps (about +48% throughput, a third less time);
  - after the lightweight SEE context: 406,067 nodes in 4,578 ms,
    88,699 nps.
  The identical node count proves the search tree did not change.
- NNUE probe (same command, `KOI_NNUE_PATH` set): depth 6 covers
  1,175,585 nodes at 64,624 nps. The bigger tree is an eval/ordering effect,
  not a slowdown of inference; throughput is roughly three quarters of
  classical on the same probe.

## NNUE pipeline

- Corpus: 1,461,259 positions, 1,200,002 Stockfish-19 depth-10 labels
  (`FEN;cp;bestmove`), |cp| > 4000 dropped.
- Trainer: 960-256-32-1 embedding-bag network, float checkpoint
  `artifacts/training/koi-sf-v1.pt`, validation MAE about 123 cp.
- Export: version 3 container with explicit shifts (`s1=7`, `s2=7`, `k3=4`),
  integer validation MAE 122.9 cp, 501,011 bytes. The earlier version 2
  single-scale container collapsed the same model to about 405 cp MAE, which
  is why v3 exists.
- C++ side: `NnueLoader` round trip and boundary vectors pass
  (`nnue_boundary_tests`, 11 cases including the v3 shift case);
  `koi_strength_tests` is 64/64 with the net loaded; `koi-bench --nnue`
  completes the 64-position gate.
- Playing test: equal-node cutechess A/B, 20,000 nodes per move, 1 thread,
  both sides `OwnBook=false`, `Hash=64`. The match reached 9 finished games
  (`0 - 4 - 5`) and was aborted; the net is weaker than the classical
  evaluator at equal nodes and stays opt-in.

## Tests

Full Release CTest: 45 registered tests. The first full run finished 44/45 in
439.00 s with `koi_engine_process` failing because its handshake expectation
predated the advertised `EvalFile` option. The expectation in
`tests/integration/uci/uci_process_test.ps1` was updated and the follow-up full
run finished **45/45 in 435.33 s**.

| Test | First run | Final run |
| --- | --- | --- |
| `koi_search_tests` | 236.98 s | 239.14 s |
| `koi_uci_match_clock` | 67.98 s | 68.22 s |
| `koi_benchmark_process` | 49.35 s | 47.35 s |
| `cutechess_stability_smoke` | 30.08 s | 27.69 s |
| `koi_engine_time_safety_process` | 11.26 s | 11.10 s |
| `koi_uci_match_process` | 10.05 s | 8.59 s |
| `uci_controller_tests` | 1.81 s | 1.75 s |
| `koi_engine_process` | 0.60 s (failed) | 1.58 s |

Targeted Release runs before the full suite: `koi_core_tests`,
`koi_rules_tests`, `native_rule_state_tests`, `perft_tests`,
`koi_shadow_diff_tests`, `koi_strength_tests`, `search_ordering_tests`,
`search_policy_tests`, `search_runtime_tests`, `static_exchange_tests`,
`evaluation_boundary_tests`, `evaluation_architecture_tests`,
`time_manager_tests`, `completion_gate_tests`, `uci_controller_tests`, and
`koi_search_tests` all exited 0. The `koi_core_tests` quiet-check differential
is the direct oracle for the O(1) rewrite; `native_rule_state_tests` caught and
then confirmed the fix for the en-passant occupancy bug in the attack-table
commit.

## Limitations

- The trained network is kept as a local artifact; no `.nnue` file is
  committed and no Elo/CPL claim is made.
- Elo anchors manifest, SPRT/LLR, and a licensed `book.bin` remain absent, so
  no rating campaign was run (repository policy: Elo/NPS are reports).
- The deferred list in the plan (bitboard movegen, incremental legality, TT
  power-of-two indexing, lazy SMP, correction history, interior Syzygy
  probes) has no implementation and therefore no verification.
