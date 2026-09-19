# Syzygy and endgame verification

Record for the program planned in
`docs/superpowers/plans/2026-09-19-syzygy-endgames.md`.

## Environment

- Windows x64, MSVC (VS Community 2022 toolchain 14.44, Ninja), C++26.
- Repository baseline HEAD `dab26d5` (`Add bounded static-eval correction
  history`), engine version 1.1.0.
- The working tree already contained uncommitted true-IID work
  (`src/koi/detail/search_constants.hpp`, `src/koi/detail/search_context.cpp`,
  `src/koi/search_types.hpp`, `tests/unit/search/koi_search_tests.cpp`); it was
  preserved untouched. Its WIP test
  `true internal iterative deepening: a deep search without a transposition move
  must probe at depth - 2` fails in `koi_search_tests_1of4` before and after
  this program and is the only Release CTest failure.
- No `.rtbw`/`.rtbz` files exist anywhere on this machine (verified by recursive
  scan of `C:` and `D:`), so no real tablebase strength validation was possible.
  All Syzygy work below is either neutral (default off) or explicitly
  experimental and strength-unvalidated.
- Build wrapper used for every tree:
  `cmd /c '"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 && cmake --build build\release --config Release'`
  (and the `build\debug` equivalent).

## Phase D - endgame evaluation

### Changes

- `src/koi/evaluation_parameters.hpp`: version bumped from
  `classical-eval-v7-opening-queen-discipline` to
  `classical-eval-v8-endgame-scaling`; new fields
  `king_activity_endgame_threshold = 12`, `endgame_scale_start_phase = 8`,
  `endgame_scale_minor_only = 32`, `endgame_scale_opposite_bishops = 32`, and
  `passed_pawn_enemy_king_penalty_weight = 2`.
- `src/koi/classical_evaluator.hpp`: `kEvaluationScaleOne = 64` and a new
  `EvaluationBreakdown::endgame_scale` magnitude field documented as unchanged by
  perspective negation (`total == sum(terms) * endgame_scale / 64` for live
  positions).
- `src/koi/classical_evaluator.cpp`: `king_activity_for` now ramps from
  `endgame_phase > king_activity_endgame_threshold` instead of the `phase <= 2`
  cliff; `endgame_scale_for` recognizes pawnless one-minor-each and
  opposite-colored-bishop (with pawns) endings below `endgame_scale_start_phase`;
  `apply_endgame_scale` rounds half away from zero; dead positions stay exactly
  zero; `passed_pawn_for` now subtracts enemy-king control
  (`max(0, 4 - enemy_king_distance) * passed_pawn_enemy_king_penalty_weight`).
- `src/koi/evaluation_parameters_generated.hpp`: `parameter_version` follows the
  v8 string.

### Tests and evidence

- `tests/unit/evaluation/classical_evaluator_tests.cpp`: new tests
  `test_endgame_scale_discounts_drawn_minor_endings`,
  `test_endgame_scale_halves_opposite_colored_bishops`,
  `test_king_activity_tapers_beyond_the_endgame_threshold`,
  `test_endgame_king_proximity_shapes_passed_pawn_value`, and
  `test_endgame_fixture_invariants`; `test_breakdown_terms_sum_to_total` now
  applies the same rounding helper. Registered names: "classical evaluator
  minor-piece draw scale", "classical evaluator opposite-bishop scale",
  "classical evaluator king activity taper", "classical evaluator passed pawn
  king proximity", "classical evaluator endgame fixture invariants".
- `tests/data/endgames/endgame-positions.txt`: 18 named endgame FENs covering
  passers, races, minor-piece draws, opposite/same-colored bishops, rook
  endings, and dead positions.
- A curated-gate constraint was found and is recorded: the `evasion_06` fixture
  `7k/7b/8/8/4K3/8/8/6N1 w - - 0 1` (also used by the serial/threaded
  root-in-check parity test) flips its expected move at any minor-only factor
  below 1/2, and only the rounding `(total*scale + 32) / 64` keeps it stable at
  1/2. Do not lower `endgame_scale_minor_only` below 32 or remove the rounding.
- Commands and results:
  - `ctest --test-dir build\release -C Release -j 8 --output-on-failure -L evaluation`
    => 100% passed, 5/5.
  - `build\release\koi_strength_tests.exe` => `run=7 pass=7 fail=0`
    (includes the 64-position tactical gate).
  - `ctest -R "koi_search_tests" -j 4` => shards 2of4/3of4/4of4 passed; 1of4
    failed only on the pre-existing true-IID entry.
- A/B evidence (baseline worktree at `HEAD` plus the same three IID files, so
  the only difference is the Phase D evaluation change):
  - `tools/stability/uci_match.ps1` on `tests/data/positions/evaluation-positions.txt`
    (10 endgame FENs), 2 games per position per color, 20,000 nodes, Threads 1,
    Hash 64, own book off.
  - Result: `+6 =28 -6 aborted=0` over 40 games (50%), recorded in
    `artifacts/matches/eval-endgame-ab-20260919-101314-475/eval-endgame-ab.json`
    (schema `koi-eval-endgame-ab-v1`). Verdict: neutral at this sample size; no
    regression observed.
  - A first A/B attempt without `-FenFile`/`-OpeningFile` was degenerate (the
    harness then replays one startpos game) and was discarded.

## Phase A - Syzygy adapter hardening

### Changes

- `src/koi/syzygy_tablebase.hpp/.cpp`: new `large_table_limit()` (largest table
  in the loaded directory, `0` when disabled) and cheap `probe_eligible()`
  (piece count + castling rights only, conservative). `probe_wdl` no longer
  takes the Fathom mutex (the vendored `tb_probe_wdl` is documented thread
  safe); root WDL/DTZ probing stays serialized because `tb_probe_root` is not
  thread safe.
- `src/koi/search_types.hpp`: `SearchOptions::syzygy_interior_depth`
  (`std::uint8_t`, default 0).
- `src/koi/uci_controller.cpp/.hpp`: new `SyzygyInteriorDepth` spin option
  (min 0, max 100, default 0, advertised and dynamic); the option is wired into
  `SearchOptions` without rebuilding the tablebase. `rebuild_syzygy()` now emits
  `info string Syzygy: <n>-man tables at <path>` (or a "no usable tables"
  notice) when a path is set.
- `tests/data/uci/handshake.txt`:
  `option name SyzygyInteriorDepth type spin default 0 min 0 max 100` between
  `SyzygyProbeLimit` and `Syzygy50MoveRule`; the controller test option count is
  now 26.
- `modules/tablebase.ixx`: `tablebase_boundary_version = 2`; contract gains
  `interior_depth` and `large_table_limit`.
- `tests/unit/runtime/syzygy_tablebase_tests.cpp`: three new tests
  ("large table limit reflects the loaded directory", "probe eligibility is a
  cheap conservative gate", "concurrent enabled WDL probes agree") using the
  existing 80-byte `KQvK.rtbw` dummy fixture. `uci_controller_tests.cpp` sets
  `SyzygyInteriorDepth` valid and invalid values in the Syzygy option test.

### Verification

- `ctest -R "syzygy_tablebase_tests|uci_controller_tests|koi_module_tests"`
  => 3/3 passed (0.03 s / 0.31 s / 2.04 s).

## Phase B - interior WDL probing

### Changes

- `src/koi/detail/search_constants.hpp`: `kTablebaseInteriorWinScore = 90'000`,
  `kTablebaseInteriorLossScore = -90'000` (below `kMateThreshold`, so a TB win
  is never advertised as mate), `kMinimumSyzygyInteriorDepth = 1`.
- `src/koi/search_types.hpp`: `TablebaseProbeResult{score_cp, mate}` and
  `SearchOptions::tablebase_probe_hook` (test/diagnostic seam; a hook result is
  decisive only when `mate` is set, and the hook fully overrides real probing).
- `src/koi/detail/search_context.hpp/.cpp`: `TablebaseSearchBinding{table,
  interior_depth, fifty_move_rule, probe_hook}` and
  `SearchContext::probe_tablebase`. The node-entry probe sits after the TT
  probe/`root_move_hint` and before null move:
  `interior_depth >= 1 && ply > 0 && !excluded_search && !claimable_draw &&
  !repetition_sensitive && depth >= interior_depth`; a decisive score
  increments `stats.tbhits`, sets `path_selective_bound = true`,
  `path_lower_bound = score > 0`, and returns.
- `src/koi/detail/search_runner.cpp`: one binding built at the top of
  `SearchRunner::run` under the same restrictions as the root probe (single PV,
  not analysis, not ponder, no `searchmoves`, no forced/claimable root draw,
  plus interior depth > 0 and a table or hook); passed to all four
  `SearchContext` construction sites through the two pools. `SearchInfo::tbhits`
  is now filled for serial iterations, root-parallel iterations/MultiPV, and the
  depth-zero fallback.
- Provenance/TT policy needed no code change: the TT store gate refuses
  selective children without an authoritative lower-bound certificate, so
  TB-derived scores are never stored as exact bounds, and the root stay
  non-authoritative for aspiration seeding. `CompletionGate` classifies a
  result with `tbhits != 0` as `CompletionSource::tablebase`; interior hits
  therefore carry that source, which is acceptable and documented.

### Tests

`tests/unit/runtime/search_service_tests.cpp` (SearchService end-to-end):
"interior tablebase hook cutoff" (pawn FEN
`8/8/8/4k3/8/8/4P3/4K3 w - - 0 1`, decisive loss hook, expects
`tbhits > 0` and `score_cp == 90000`), "interior tablebase hook non-decisive"
(a non-mate result is ignored), "interior tablebase hook zero mate" (draw-class
result is ignored), "interior tablebase hook fifty move" (pawnless FEN with a
nonzero clock never invokes the hook), "interior tablebase hook exception"
(hook exceptions are swallowed and the search completes), and "interior
tablebase default off" (hook installed but `syzygy_interior_depth = 0` gives
equal nodes/qnodes/score/best move versus no hook).

### Verification

- `build\release\search_service_tests.exe` => `run=14 pass=14 fail=0`.
- Full Release CTest `ctest --test-dir build\release -C Release -j 8
  --output-on-failure` => **58/59 passed**, with the only failure the
  pre-existing true-IID WIP test (`koi_search_tests_1of4`:
  `run=38 pass=34 fail=1 xfail=3`). All evaluation, runtime, search, process,
  UCI, Python, and packaging tests passed.
- Debug build succeeded; `ctest --test-dir build\debug -C Debug -j 8
  --output-on-failure -LE heavy` => 51/51 passed.

## Phase C - 50-move and DTZ correctness

- C1 is satisfied in code and pinned by the new tests: interior cutoffs require
  `halfmove_clock == 0` when the fifty-move rule is active (hook and real
  paths), and cursed/blessed/draw classes never cutoff because
  `syzygy_score` leaves `mate` unset for them and `decisive_score` rejects
  unset/mate-0 results.
- C2 already existed: `probe_root` uses clock-aware `tb_probe_root_dtz` with a
  WDL fallback, covered by the existing "50-move selection" test.
- C3 is deferred: changing the `SyzygyProbeLimit` default needs 6-man data,
  which does not exist on this machine.

## Phase E - verification and documentation

- `README.md`: the Syzygy section documents `SyzygyInteriorDepth` (default 0,
  experimental/unvalidated, decisive results only, fifty-move gated, inert
  unless a path or hook is present, root `SyzygyProbeDepth` gate unchanged), and
  the portable defaults list now ends with `SyzygyInteriorDepth=0`.
- `tests/README.md`: added the `data/endgames/` fixture entry and corrected the
  stale known-failure list (removed `threaded root-in-check parity`, which is
  not in the current source list).
- This record and the plan are indexed in `docs/superpowers/plans/README.md` and
  `docs/superpowers/verification/README.md`.
- Final scoped Release rerun after all edits:
  `ctest --test-dir build\release -C Release -j 8 --output-on-failure -E "koi_search_tests_1of4"`
  => **58/58 passed**. The excluded shard is the only holder of the pre-existing
  true-IID WIP failure, which is outside this program.

## Outcome

- Endgame evaluation is the shipped strength change: version
  `classical-eval-v8-endgame-scaling`, deterministic, and neutral-to-positive
  (no regression) in the node-limited endgame A/B.
- Syzygy interior probing is implemented, wired through UCI, and covered by
  hook-driven tests, but **strength-unvalidated** because no tablebase assets
  exist here. It is off by default and leaves every existing suite
  byte-identical.
- The single Release CTest failure is the pre-existing true-IID WIP test, which
  is outside this program's scope.
