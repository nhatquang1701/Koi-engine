# NNUE Studio UI improvement plan

Goal: make the local NNUE Studio trustworthy and informative. The pass fixes
the correctness defects found in the first studio version (a "gate only" button
that fired a broken zero-game A/B match, a `net_name` field no backend honored,
install offered for a network whose gate never ran, unguarded numeric input, and
unsynchronized run-state writes), then persists and displays the training
telemetry the trainer already prints (loss histories, ETA, throughput), and
turns the run list into a filterable, sortable view with failure details.

## Hard constraints

- Tooling only: no engine, UCI, or trainer command-line semantics change; the
  classical evaluator stays the default evaluator.
- The GUI must never block the Tk thread on file or process work; the existing
  worker-thread plus queue model is preserved.
- No new GUI automation: headless tests cover the pure logic, and
  `--gui-selftest` remains the widget construction smoke test.
- Existing runs keep working: a run whose configuration says `koi.nnue` but
  whose directory only holds `net.nnue` still resolves to `net.nnue`.
- Never weaken or delete an assertion; the full release and debug CTest suites
  stay green.
- No new runtime dependencies; the Windows x64 / CPython toolchain is unchanged.
- Do not commit `artifacts/` or `.opencode/`.

## Global decisions

- A new headless test module `tests/python/nnue/studio_ui_test.py` is registered
  as `nnue_studio_ui_python` with the `python` label; it tests only pure logic
  and skips nothing that is available in the default environment.
- `state.json` changes are additive; the existing schema string is unchanged and
  old state files keep loading.
- `net_name` means what the GUI and CLI always claimed: the configured network
  file name inside the run directory. `net.nnue` remains the fallback name for
  older runs and whenever the configured file does not exist yet.
- "Run 64-position gate" runs only `koi-bench --nnue`; the A/B match is a
  separate action. Install is offered only after a gate that ran, produced
  positions, and did not reject the network.
- Reports are local; nothing here claims Elo or changes the engine default.

## Phases

- [x] **Phase 0 — Documentation scaffolding.** Plan, design specification, and
  verification record, plus one index row in each of the three
  `docs/superpowers/*/README.md` tables. Commit.
- [x] **Phase 1 — Correctness and configuration plumbing.** Gate-only
  validation; `net_name` threaded through backends and run path resolution;
  install gated on a successful gate; shared numeric input guards; a lock around
  run-state writes; headless tests for each; commit.
- [ ] **Phase 2 — Training telemetry.** Persist `train_loss`, `val_loss`,
  `seconds`, rows loaded and throughput; ETA and summary helpers; a multi-series
  validation chart with axes and a legend; log filtering with pause-on-scroll;
  tests; commit.
- [ ] **Phase 3 — Run list UX and failure surfacing.** Run filtering, sortable
  columns, duration, a detail pane, an auto-refresh timer, and a failed-run
  report that includes the exit code and the tail of `train.err`; tests; commit.
- [ ] **Phase 4 — Verification.** Full release and debug CTest runs, the final
  verification section, index and README synchronization, and the closing
  commit.

### Deferred

Data-generation attach/progress and in-GUI dataset encoding; per-backend
advanced configuration; preset save/load; validation cancellation and report
links; net-vs-net validation controls; run deletion and continue-from-checkpoint;
CLI/JSON parity flags; GUI automation beyond construction.
