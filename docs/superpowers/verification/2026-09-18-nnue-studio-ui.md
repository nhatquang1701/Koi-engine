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

Pending.

## Phase 3 — run list UX and failure surfacing

Pending.

## Phase 4 — verification

Pending.

## Limitations

Pending.
