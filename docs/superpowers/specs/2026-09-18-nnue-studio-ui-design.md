# Koi NNUE Studio UI improvement design

Status: planned on 2026-09-18. The studio stays a local convenience front end
over the existing training pipeline; it adds no engine behavior.

## Problem

The first studio version ships several correctness defects and loses
information the pipeline already produces:

- The "Run 64-position gate" button calls the full validation path with
  `games=0`, and `ab_match.ps1` rejects a zero game count, so the button neither
  stays gate-only nor succeeds.
- `net_name` flows through the CLI, the configuration and the GUI field, but
  every backend hard-codes `net.nnue`; the setting is a silent no-op.
- Install is enabled whenever the gate did not explicitly reject the network,
  including when the gate failed to run at all.
- Numeric text fields are parsed with bare `int()`/`float()` inside Tk
  callbacks; bad input raises a traceback instead of a dialog.
- The tail thread and the Tk thread write `state.json` concurrently.
- `train_loss`, `val_loss` and the epoch duration are parsed but discarded, the
  chart shows validation MAE alone, and there is no ETA or throughput.
- Failed runs report only a status string; the exit code and `train.err` are
  never shown.
- The run list has no filter, no sorting, no duration and no detail view.

## Contracts

### Validation

- `studio_core.validate_net(net, run=None, games=20, nodes=20_000,
  report_path=None, gate_only=False)` keeps its current result schema. With
  `gate_only=True` it runs only `run_gate` and omits the `ab_match` key.
- `studio_core.gate_allows_install(gate)` is `True` only when the gate ran,
  recorded more than zero positions, has no `error` and no `rejected` flag. The
  GUI enables the install button and offers installation only when it is true.

### Network naming

- `studio_core.network_file_name(config)` returns the configured `net_name`
  (falling back to `net.nnue` for an empty or missing value).
- `studio_core.network_path(directory, config)` returns the configured path
  when it exists, otherwise the legacy `net.nnue` when that exists, otherwise
  the configured path (the not-yet-written target).
- `studio_core.metadata_file_name(config)` and `metadata_path(...)` derive the
  metadata name from the resolved network stem (`<stem>.metadata.json`).
- The backend protocol becomes `net_path(run_dir, config=None)` and
  `metadata_path(run_dir, config=None)`; both delegate to `studio_core`. The koi
  and torch backends write `--net-out`/`--meta-out` under the configured name.
- `Run.net_path`/`Run.metadata_path` use the run's own configuration, so old
  runs that only contain `net.nnue` keep resolving.

### Input validation

- `studio_core.parse_int(text, name, minimum=None, maximum=None)` and
  `parse_float(...)` raise `ValueError` with a field-name message. The GUI uses
  them for the run configuration and the A/B fields and reports failures with a
  dialog instead of a traceback.

### State consistency

- `studio_core` serializes `Run.write_state` with a module-level
  `threading.Lock`, so the tail thread and the Tk thread cannot interleave a
  read-modify-write of `state.json`.

### Telemetry

- `parse_progress` additionally recognizes the loader line
  `loaded <n> rows ... in <t>s` as a `loaded` event with `rows` and `seconds`
  (the legacy trainer prints the same prefix without the parenthesized detail).
- `Run.update_from_log_events` persists, in addition to the existing `history`:
  `train_loss_history`, `val_loss_history`, `seconds_history`, `rows_loaded`
  and `rows_per_second`. The merged `progress` mapping stays additive.
- Pure helpers: `estimate_eta(progress)` (mean epoch seconds times remaining
  epochs, or `None`), `progress_summary(progress)` (one display line),
  `chart_series(progress)` (named series with colors for train loss, validation
  loss and validation MAE), and `chart_bounds(series)` (value range or `None`).
- The GUI chart draws every non-empty series with epoch ticks, axis labels and a
  legend; the progress label shows epoch, validation MAE, ETA and throughput.
- `log_line_matches(line, needle, errors_only)` powers the log filter; the log
  view follows new output only while it was already scrolled to the bottom.

### Run list

- Pure helpers: `filter_runs(runs, text)` (case-insensitive match over run name,
  kind and status), `sort_runs(runs, key, descending)`, `run_duration_seconds`
  (from persisted `created`/`finished` timestamps), `run_detail_lines(run)` and
  `failure_lines(run, limit)` (tail of `train.err` for failed runs).
- The Runs tab gains a filter entry, clickable sort headings, a duration column,
  a five-second auto-refresh, and a detail pane; existing actions are unchanged.
- When a run finishes as `failed`, the studio appends the exit code and the
  failure tail to the log and shows a dialog offering to open the run folder.

## Out of scope

Data-run attach/progress and dataset encoding; advanced per-backend
configuration; preset persistence; validation cancellation and report links;
net-vs-net controls; run deletion and checkpoint continuation; CLI/JSON parity.
Those remain candidates for a later pass.

## Constraints

- No engine code changes: the studio is tooling only.
- No network access; runs stay local.
- The GUI never blocks: existing worker threads and the queue pump are kept, and
  only cheap state reads happen on the Tk thread.
