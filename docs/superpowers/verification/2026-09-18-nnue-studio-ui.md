# NNUE Studio UI improvement verification

Scope: the correctness, telemetry and run-list work described in
`docs/superpowers/plans/2026-09-18-nnue-studio-ui.md`.

## Environment

- Windows x64, MSVC 14.44.35207, CMake 3.31.6 (MSVC build tools), Ninja
  generator, `build/release` and `build/debug` trees.
- Python 3.14.5 with torch 2.14.0+cpu, python-chess 1.11.2, numpy, tkinter.
- CTest default configuration: 57 tests (`KOI_BUILD_SHADOW_DIFF=OFF`); this pass
  adds one Python test target.

## Phase 0 — documentation scaffolding

Plan, design specification, verification record and the three index rows.
Recorded in commit history; no behavior change.

## Phase 1 — correctness and configuration plumbing

### Gate-only validation

`studio_core.validate_net` gained a `gate_only` keyword. The gate
(`koi-bench --nnue`) always runs; when `gate_only` is true the
`ab_match.ps1` call is skipped entirely and the result has no `ab_match` key.
The Studio's "Run 64-position gate" button now uses this path instead of
calling the full validation with `games=0`, which `ab_match.ps1` rejects
(its `Games` parameter is validated to 2..2000).

### Network file naming

`studio_core` now owns the naming helpers:

- `network_file_name(config)` returns the sanitized `net_name` (basename only)
  or `net.nnue` when unset.
- `network_path(directory, config)` prefers the configured file when it exists,
  falls back to a legacy `net.nnue` in the same directory, and otherwise returns
  the configured target path.
- `metadata_file_name(config)` is the network stem plus `.metadata.json`;
  `metadata_path(directory, config)` follows the resolved network.

`Run.net_path` and `Run.metadata_path` use these helpers, and every backend
(`koi`, `torch`, `bullet`) delegates its `net_path`/`metadata_path` methods to
them. The `koi` and `torch` `build_command` implementations pass
`run_dir / network_file_name(config)` to `--net-out` and
`run_dir / metadata_file_name(config)` to `--meta-out`, so the GUI "Network
name" field and CLI `--net-name` now control the produced file. Older runs
that only hold `net.nnue` keep resolving to it.

### Install gating and input guards

- `gate_allows_install(gate)` returns false for a missing gate, an error
  result, a rejected network, or zero checked positions. The Studio enables the
  install button and offers installation only when it returns true; otherwise it
  reports "install stays disabled until a 64-position gate passes".
- `parse_int`/`parse_float` in `studio_core` validate and bound the numeric
  fields. `_collect_config` and `_start_validation` use them, so non-numeric
  A/B games/nodes or bad training fields show a message dialog instead of a Tk
  callback traceback.

### State write lock

`Run.write_state` is serialized by a module-level `threading.Lock`, removing
the interleaving between the tail thread, the Tk pump and the runs refresh.

### Tests and results

New module `tests/python/nnue/studio_ui_test.py`, registered in CTest as
`nnue_studio_ui_python` (label `python`), 12 cases:

- `NetworkNamingTests` — default and sanitized names, configured-name
  preference, legacy `net.nnue` fallback, unwritten target, `Run` properties.
- `ValidationGatingTests` — the `gate_allows_install` matrix; `gate_only`
  never calls `run_ab_match` and omits the key; full validation calls both.
- `InputGuardTests` — bounds and field-named error messages.
- `StateLockTests` — eight threads writing distinct state keys concurrently
  leave a parseable JSON file containing every key.
- `BackendCommandTests` — `--dry-run --preset quick --net-name custom.nnue`
  prints `custom.nnue` and `custom.metadata.json`.

Results: `python -m unittest tests/python/nnue/studio_ui_test.py -v` → 12/12
pass in 0.176 s. Existing `tests/python/nnue/studio_test.py` → 11/11 pass in
6.379 s. `python tools/nnue/koi_nnue_studio.py --gui-selftest` → `PASS gui
construction`.

## Phase 2 — training telemetry

### Parsing and persistence

`parse_progress` recognizes the trainer's dataset-loading line
(`loaded <n> rows ... in <t>s`, both the v4 and legacy trainers' wording) as a
`loaded` event carrying the row count and rows per second. Epoch events already
carried `train_loss`, `val_loss`, `val_mae_cp` and the optional per-epoch
seconds; `Run.update_from_log_events` now persists them as
`train_loss_history`, `val_loss_history`, `seconds_history`, plus `rows_loaded`
and `rows_per_second`. The existing `history` (validation MAE per epoch) and
`val_mae_cp` fields are unchanged, and the state schema string is untouched, so
older state files keep loading.

`stream_command` (headless runs and the selftest) now routes every parsed event
through the same persistence path instead of keeping a local MAE list.

### Display helpers

Pure helpers in `studio_core`:

- `format_duration` — `45s`, `4m 32s`, `1h 05m`.
- `estimate_eta` — mean epoch seconds times remaining epochs; `None` when no
  timings exist, `0.0` once every epoch is done.
- `progress_summary` — one line with epoch, validation MAE, ETA and throughput.
- `chart_series` — named, colored series (train loss, val loss, validation MAE),
  each tagged with its axis (`loss` or `mae`).
- `chart_bounds` — low/high bounds for one axis or all series, padded when flat.
- `log_line_matches` — Train-tab log filter (substring plus errors-only).

### Train tab

- The chart is now a multi-series plot with left (loss) and right (validation
  MAE) scales, epoch ticks, a legend and gridlines. The old single val-MAE
  polyline is replaced.
- The progress label uses `progress_summary`, so a running epoch shows
  `epoch 2/10 - val MAE 142.1 cp - ETA 14m 00s - 15,000 rows/s`.
- The log has a substring filter entry and an "Errors only" checkbox; the buffer
  keeps 5000 lines, re-renders on filter change, and auto-scrolls only while the
  view is already at the bottom.
- Attaching to a run restores the persisted progress into the chart and
  progress line instead of starting empty.

### Tests and results

`tests/python/nnue/studio_ui_test.py` gained `TelemetryTests` (5 cases): loaded
line parsing for both trainers, history persistence through
`update_from_log_events`, ETA/summary/duration formatting, chart series/axis
bounds, and log filtering. The older `studio_test.py` case that asserted
`loaded ...` was ignored now asserts it parses as throughput (the line is
telemetry by design).

Results: `python -m unittest tests/python/nnue/studio_ui_test.py
tests/python/nnue/studio_test.py` → 29/29 pass in 5.336 s (17 UI + 12 studio).
`--gui-selftest` → `PASS gui construction`.

## Phase 3 — run list UX and failure surfacing

Pending.

## Phase 4 — verification

Pending.

## Limitations

Pending.
