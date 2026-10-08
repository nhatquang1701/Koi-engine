# Speed program final report (2026-09-25)

Baseline: commit `47d2cf3` binary
`artifacts/verification/speed-program/phase-0/koi-bench-phase0.exe`
(sha256 `7A9AB33B...73F7`). Final: `build/release/koi-bench.exe`
(sha256 `138A4595A395611194C8838558B497DBE675447D827DA8A5A0E259158721C313`).
Both were measured with `tools/build/speed_gate.ps1` (alternating runs,
medians, `-MinRowElapsedMs` default 20 ms; rows below the floor are reported
with `noise=true` and excluded from the row verdict only).

## Correctness contract

- Threads=1 fixed-depth rows stayed byte-identical through every phase:
  default stdout sha256
  `6F7D8FF537A61FF0B6F15D14E1C2AC5B0F5E7DC0FE205BE3374C1DC09FC4A7F8`, and
  identical `--timed` rows after removing `elapsed_ms`/`nps`.
- perft startpos/Kiwipete/endgame-pin, shadow-diff, rules, ordering, and the
  soak suites stayed green in every phase.
- Final full runs: Release CTest 67/67 and Debug CTest 67/67 (100% passed),
  ASan subset 26/26, `release_verify.ps1` PASS.

## Per-phase results (Threads=1, Phase 0 binary vs the phase binary)

| Phase | Gate | Total NPS | Median row |
| --- | --- | --- | --- |
| 1 TT hazard leases | T8 default, Runs=9 | -1.69% | -1.18% |
| 1 TT hazard leases | T1 cold default, Runs=9 | -1.88% | -0.96% |
| 1 TT hazard leases | T1 endgames 2..5, Runs=5 | +0.78% | 0.00% |
| 2 evaluation caching | T1 cold default, Runs=9 | +5.55% | +4.74% |
| 2 evaluation caching | T1 endgames 2..5, Runs=5 | +0.96% | +2.80% |
| 3 search bookkeeping | T1 cold default, Runs=9 | +5.37% | +6.66% |
| 3 search bookkeeping | T1 endgames 2..5, Runs=5 | +0.93% | +3.85% |
| 4 king masks + fast filter | T1 cold default, Runs=9 | +7.38% | +6.61% |
| 4 king masks + fast filter | T1 endgames 2..5, Runs=5 | +1.90% | +8.26% |
| 7 cumulative (vs Phase 0) | T1 cold default, Runs=9 | +7.30% | +7.02% |
| 7 cumulative (vs Phase 0) | T1 endgames 2..5, Runs=5 | +1.48% | +9.22% |

The per-phase rows are not additive: every phase was measured against the
same frozen Phase 0 binary, and the machine's absolute NPS drifts between
sessions. The Phase 7 rows are the end-to-end effect of Phases 1-4.

## Build-level experiment (Phase 5, PGO)

The `KOI_PGO` generate/use pipeline is wired and documented, and its binaries
are byte-identical in behavior. On this machine the use-mode link produced a
consistently slower binary than the plain Release build (-2% to -9% on the
same gates), while a control build of the same directory with `KOI_PGO=OFF`
matched Release. The option therefore stays `OFF` by default; see the Phase 5
record in the plan for the training recipe and numbers.

## Phase 6

No behavior-changing candidate surfaced: every optimization that landed was
value-preserving. The SPRT gate was not used and no campaign was recorded.

## Deferred

- Direct legal generation (capture-checker and block-ray predicates instead of
  the probed exceptions) and bitboard movegen.
- Incremental king-mask maintenance from move metadata.
- Pawn hash keyed by `pawn_key`, locked-pawn-wall cache, FeatureState
  lock/copy removal, and the per-candidate feature extractions at
  `search_context.cpp:1559,1591`.
- Picker emission-time `priority` recomputation removal, the constant
  `null_move_is_safe` re-derivations, and the `terminal_score` check probe.
- A PGO revisit with another compiler or training set.

## Evidence index

- `phase-0/`: baseline captures (cold/warm/node steady, three depth sweeps),
  pin, noise floor, and the fixed-depth performance gate.
- `phase-1/` .. `phase-4/`: speed-gate JSON and logs; the Phase 1 ASan log.
- `phase-5/`: the PGO gates (negative result) and the control gate.
- `phase-7/`: Release/Debug/ASan summaries, `release-verify/`, and the final
  cumulative gates.
