# Syzygy and endgame plan

Goal: improve endgame play and Syzygy tablebase handling in Koi. The endgame
evaluation work is the primary strength carrier; the Syzygy work hardens the
adapter, adds an opt-in interior WDL probe, and prepares 50-move-aware DTZ
selection. The plan was approved after reconnaissance established that this
machine has no `.rtbw`/`.rtbz` assets anywhere on `C:` or `D:`, so every
tablebase strength claim is explicitly blocked and the interior probe ships as
opt-in, experimental, and strength-unvalidated.

## Hard constraints

- Windows x64, MSVC only; builds go through the repository CMake/Ninja trees.
- Classical evaluation remains the engine default; NNUE stays opt-in.
- `SyzygyInteriorDepth = 0` (the default) must leave the entire existing suite
  byte-identical. The interior probe is inert unless enabled.
- The interior probe is only implemented and unit-tested through a probe hook;
  no real 3-4-5-man tablebase data is available to validate strength.
- The UCI handshake change adds exactly one option. `tests/data/uci/handshake.txt`,
  the controller tests, the process test, and both README option lists are
  updated together.
- Root probing keeps its existing gate and `SyzygyProbeDepth` semantics; the new
  interior option is independent.
- Never weaken or delete an assertion to obtain a green run. Pinned evaluation
  tests may change only with recorded evidence; the 64-position tactical gate,
  perft, and shadow-diff stay mandatory.
- Do not touch the pre-existing uncommitted true-IID work; do not commit
  `.opencode/`, builds, or artifacts.

## Ordering

`D -> A -> B -> C -> E`. Evaluation first because it is the only strength work
that can be validated on this machine; the Syzygy work follows so it can reuse
the same A/B harness without blocking on tablebase assets.

## Phase D - Endgame evaluation

- D1: bounded, deterministic, integer-only endgame scale factor applied to
  `total` before perspective negation. Patterns: pawnless minor-piece endings
  (`endgame_scale_minor_only`, 1/2) and same-material opposite-colored-bishop
  endings (`endgame_scale_opposite_bishops`, 1/2), active only at or below
  `endgame_scale_start_phase`.
- D2: continuous king-activity taper (no more `phase <= 2` cliff) and passed-pawn
  king proximity both ways: own-king support still adds, enemy-king control now
  subtracts (`passed_pawn_enemy_king_penalty_weight`).
- D3: new `ClassicalEvaluationParameters` fields, a version bump past
  `classical-eval-v7-opening-queen-discipline`, and regenerated metadata.
- D4: deterministic evaluation tests plus node-limited A/B evidence against the
  pre-change baseline before adoption.
- D5: endgame regression fixtures under `tests/data/endgames/` and an invariant
  test over them.

## Phase A - Syzygy adapter hardening (no search-tree change)

- A1: new UCI option `SyzygyInteriorDepth` (spin 0..100, default 0);
  `SyzygyProbeDepth` keeps gating only the root probe.
- A2: cheap `probe_eligible` pre-gate (piece count + castling rights) and
  `large_table_limit()` exposure.
- A3: allow concurrent WDL probes now that the vendored Fathom documents
  `tb_probe_wdl` as thread safe; root WDL/DTZ probing stays serialized because
  `tb_probe_root` is not thread safe.
- A4: surface accumulated `tbhits` through `SearchInfo` and report the loaded
  table size when a path is set.
- A5: extend `modules/tablebase.ixx` (`interior_depth`, `large_table_limit`) and
  bump `tablebase_boundary_version` to 2.
- A6: extend `syzygy_tablebase_tests.cpp` for the new seams.

## Phase B - Interior WDL probing (opt-in, hook-tested, strength-unvalidated)

- B0: `SearchOptions::TablebaseProbeHook` for tests and diagnostics; it fully
  overrides real probing and is only consulted when interior probing is enabled.
- B1: plumb a `TablebaseSearchBinding` (table, interior depth, fifty-move rule,
  hook) into `SearchContext` and all four construction sites (serial,
  root-parallel, both pools' workers).
- B2: node-entry probe after the TT probe and before pruning, when
  `depth >= interior_depth`, not a repetition-sensitive path, not an excluded
  search, and not a claimable draw. Real probing additionally requires
  `probe_eligible`.
- B3: provenance: a TB cutoff marks the path selective and records direction as
  a lower-bound certificate; it is never an exact nominal-depth root result.
- B4: TT policy: TB-derived scores stay unstorable (the store gate already
  refuses selective children without an authoritative lower-bound certificate).
- B5: the interior probe mirrors the root-gate restrictions: MultiPV > 1,
  analysis mode, ponder, `searchmoves`, and forced/claimable root draws disable
  it.
- B6: `SyzygyInteriorDepth = 0` keeps the default suite and node-count pins
  byte-identical.
- B7: hook-driven tests for cutoff, provenance, non-decisive results, the
  fifty-move gate, exception safety, and default-off equality.

## Phase C - 50-move and DTZ correctness

- C1: accept win/loss cutoffs only when `halfmove_clock == 0` under the
  fifty-move rule; cursed/blessed/draw results never cutoff.
- C2: root DTZ ranking already avoids 50-move shuffling; keep WDL fallback.
- C3: defer changing the `SyzygyProbeLimit` default (requires 6-man data that
  does not exist here).

## Phase E - Verification and documentation

- Full Release and Debug CTest; document the one pre-existing true-IID failure.
- Record that tablebase strength validation is blocked (no assets) and that
  interior probing is opt-in and unvalidated.
- Update `README.md` (Syzygy section and release defaults), `tests/README.md`
  (fixtures, known failures), and this plan/verification pair plus both
  `docs/superpowers/` index tables.
