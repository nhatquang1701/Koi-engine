# Koi Engine v2 design

Companion contract for `docs/superpowers/plans/2026-09-19-engine-v2.md`.
Each section fixes the interfaces and gates a phase must implement.

## 1. SPRT harness (`tools/stability/uci_match.ps1`)

- New optional parameter set: `-Sprt` (switch), `-SprtElo0` (default `0.0`),
  `-SprtElo1` (default `5.0`), `-SprtAlpha` (default `0.05`),
  `-SprtBeta` (default `0.05`), `-SprtMinGames` (default `20`),
  `-SprtMaxGames` (default `2000`).
- Model: standard trinomial LLR for `H0: elo <= elo0` vs `H1: elo >= elo1`
  with the logistic Elo model, `elo_per_point = 400 / ln(10)`, variance from
  the game-pair score (`wins + draws/2` per game). Bounds:
  `lower = ln(beta/(1-alpha))`, `upper = ln((1-beta)/alpha)`.
- Stop conditions: after `SprtMinGames`, stop accepting when
  `LLR >= upper`, stop rejecting when `LLR <= lower`; stop inconclusive at
  `SprtMaxGames`. Without `-Sprt` the script keeps its current fixed
  `-Games` behavior byte-identical.
- Report: the leg and aggregate JSON gain an `sprt` object
  (`enabled`, `elo0`, `elo1`, `alpha`, `beta`, `llr`, `lower`, `upper`,
  `decision` in `accept|reject|inconclusive|running`, `games`, `elo`).
  Existing schema keys keep their names and meanings; adding `sprt` is
  additive.
- Classical A/B: matching a candidate binary against a baseline binary uses
  the existing engine-vs-engine path; no engine-side parameter loading is
  required for Phase 1.
- Test: `tests/integration/tools/uci_match_fixture.cpp` already fabricates
  scripted UCI engines; add fixture scripts that always win and always lose,
  then assert the decision at the first legal stop and at the game cap. The
  SPRT math itself is also covered by a pure function unit test inside the
  PowerShell test script where practical.

## 2. Studio adoption (`tools/nnue/studio_core.py`)

- Settings: `artifacts/studio/settings.json`, schema
  `koi-studio-settings-v1`, keys `engine_directory`, `auto_adopt` (bool),
  `theme` (`light|dark`), `geometry`, `ab_games`, `ab_nodes`, `gate_games`
  (net-match games), `net_match_nodes`. Missing keys fall back to defaults;
  malformed files fall back to defaults and are rewritten on next save.
- Registry: `artifacts/studio/adoptions.json`, schema
  `koi-nnue-adoption-registry-v1`, list of records
  `{installed_utc, source_run, source_net, installed_path, backup_path,
  bytes, gate, ab_verdict, net_match_verdict, reverted_utc}`. `revert_adoption`
  copies the backup back over the installed path (backing up the current file
  first) and marks the record reverted.
- Policy: `adoption_decision(gate, net_match, installed)` returns
  `adopt|skip` plus a reason string. `adopt` requires: gate ran, no `error`,
  no `rejected`, `positions > 0`; and when `installed` is set, `net_match`
  must report `candidate-stronger` or `inconclusive` (never
  `candidate-weaker`). When `installed` is unset the comparison is skipped.
- Engine pickup: install copies to `<engine_directory>/koi.nnue` with a
  timestamped `.bak`. The engine discovers it on the next start
  (`KOI_NNUE_PATH` wins) or via `setoption name EvalFile <abs path>` at
  runtime. `running_engines()` lists `koi-engine` processes; when non-empty
  the Studio shows the restart/`EvalFile` notice after an install and records
  it in the registry.
- GUI wiring: the "Validate when finished" flow keeps running the gate and
  A/B; with "Auto-adopt" enabled the post-validation branch calls the policy,
  installs, and reports evidence instead of prompting. The Runs tab gains
  "Adopt best run" (best val MAE among completed runs with a network).

## 3. Studio presentation

- Themes: one palette dictionary per theme with keys `bg`, `surface`,
  `border`, `text`, `muted`, `accent`, `danger`, `chart_bg`, `chart_grid`,
  `series` (list). A `configure_styles(style, palette)` helper assigns named
  ttk styles (`TFrame`, `TLabel`, `Muted.TLabel`, `Accent.TButton`,
  `Danger.TButton`, `Treeview`, `TNotebook`, `TEntry`, `TCheckbutton`,
  `TProgressbar`); no widget keeps a hard-coded hex color. Dark mode also
  configures the `tk.Text` log colors.
- DPI: call `SetProcessDpiAwareness(1)` via `ctypes` when available before
  creating the root, and set `root.tk.call("tk", "scaling", ...)` from the
  detected DPI; failures are ignored.
- Responsiveness: `_count_rows`, `_refresh_runs`, and the install copy run on
  worker threads and post results through the existing queue; the pump stays
  the only widget mutator. `Esc` cancels a running validation by terminating
  its process tree; the validation worker checks a cancel event between
  steps.
- Shortcuts: `Ctrl+R` refresh runs, `Ctrl+T` start training, `F5` validate,
  `Esc` stop training or cancel validation. Shortcuts are registered on the
  root and documented in the README.
- Chart: dual-axis loss/MAE series (already present) plus hover readout of
  the nearest epoch, axis labels, and a "Export PNG" action that renders the
  canvas to a file.
- Settings persist theme, geometry, engine directory, and the adoption policy.

## 4. NNUE hot path (`src/koi/nnue.cpp`)

- `apply_move_deltas` must stop calling
  `EvaluationFeatureExtractor::extract(child)`; the child features needed for
  deltas are derived from `MoveMetadata` plus the child's king square and
  piece count (bucket). King-bucket refresh still rebuilds a perspective when
  its own king crosses a bucket boundary.
- `apply_feature_delta` gains an AVX2 path (16 lanes) with the scalar path as
  the reference; overflow semantics stay int64-accumulate then int32 clamp.
- Slot copies are avoided when the child slot can be written from the parent
  in place; the per-evaluation sparse encode is reused from the slot state
  rather than rebuilt.
- Parity requirement: the existing incremental-vs-full-recompute tests and
  the scripted special-move tests must pass unchanged; new scripted tests pin
  the delta derivation for castling, en passant, promotions, and king
  crossings. `performance_gate.ps1` must stay within its 5% budget and the
  Phase 0 NNUE NPS baseline is the reference report.

## 5. Transposition table and per-node cost

- Indexing changes from `key % cluster_count` to a power-of-two mask with
  clusters still 4 entries / 128 bytes; the table size is rounded down to a
  power of two and reported via `hashfull`/resize unchanged.
- Probe prefetch uses `_mm_prefetch` under MSVC with a portable fallback.
- Per-node cost work removes unnecessary zero-initialization of move lists and
  failed-move arrays and avoids snapshot copies where a reference suffices;
  no search-decision semantics change. Pinned node-count tests move only with
  the Phase 5 evidence bundle.

## 6. Lazy SMP

- `Threads > 1` starts `Threads - 1` helper threads that run the same
  iterative deepening with per-thread jitter (depth offsets and history-table
  perturbation) and share the transposition table, the root line publication,
  and the time budget. The main thread owns result publication and stop
  signaling.
- `Threads == 1` keeps today's serial path exactly; the deterministic
  guarantee is scoped to that configuration and documented.
- Test/harness policy: per-thread determinism (same thread count, same
  result) and legality/coverage checks replace cross-thread equality in
  `release_verify.ps1`; threaded XFAIL entries are re-evaluated and the
  README threading note is updated.

## 7. Search modernization

- Static-eval correction history: correction tables keyed by pawn structure,
  material signature, and king position; corrections are applied to the static
  evaluation only (bounded), never to TT scores.
- True IID: when no TT move exists and depth is sufficient, re-search at
  `depth - 2` with a null window to seed ordering.
- LMP: late-move pruning tables for quiet moves at low depth with an
  improving bonus.
- Qsearch TT cutoffs: exact-bound TT score cutoffs inside quiescence with a
  depth guard, keeping the current move-hint behavior.
- MultiPV per-line aspiration: each PV line keeps its own window instead of
  disabling aspiration.
- Time-manager refinements: use the existing iteration observations to scale
  soft time on instability, with no change to flag protection.
- Each feature ships alone, gated by 64/64, perft, shadow-diff, the full
  suites, and a recorded SPRT against the previous commit.

## 8. Move generation and legality

- Add incremental `checkers` and `pinned` state maintained by make/unmake, and
  generate legal moves directly (evasions when in check, pin-restricted
  targets otherwise). The current apply/undo legality probe is removed from
  the hot path.
- perft and shadow-diff parity are the acceptance oracle; all perft suites
  stay byte-identical and the native/shadow comparison stays exact.
- NPS report plus SPRT gate; pinned node-count tests move only with that
  evidence.

## 9. Evaluation pipelines

- Classical: the tuner emits a candidate parameter header; a candidate build
  (separate executable) is matched against the baseline with the Phase 1
  harness. Adoption requires a recorded SPRT decision and the 64/64 gate;
  otherwise the canonical weights stay and the report is recorded. PSQT
  tuning follows the same gate.
- NNUE: the threat/HalfKA_hm family is designed as a new container version
  (feature-set string, index formula, payload layout, quantization) with
  encoders, golden tests, and trainer configuration. No training and no
  strength claim in this program; the Studio exposes the trainer option so
  the user's later campaign can produce the network.

## 10. Optional UCI batch

- `go mate <n>`, `go perft <n>`, and `lowerbound`/`upperbound` info flags are
  additive. Any option or output change updates the handshake fixture, the
  controller tests, the process test, and the README together. Syzygy interior
  probing is a study unless a gated implementation is ready.

## 11. Evidence policy

- Every phase records: the exact commands, the log paths under
  `artifacts/verification/engine-v2/`, the test counts, and either an SPRT
  report or an explicit statement that the change is behavior-preserving with
  the evidence that proves it (byte-identical bench rows, parity tests,
  perft/shadow-diff).
- The 64/64 tactical gate is mandatory for every search- or
  evaluation-affecting change. NPS changes report the Phase 0 baseline
  comparison.
