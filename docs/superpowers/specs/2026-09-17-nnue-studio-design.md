# Koi NNUE Studio design

Status: implemented on 2026-09-17, extended on 2026-09-18 by the UI pass
(`2026-09-18-nnue-studio-ui-design.md`) and the bullet training program
(`2026-09-18-nnue-bullet-training-design.md`). The studio is a local
convenience front end over the existing training pipeline; it adds no new
engine behavior.

## Problem

Training a Koi network required remembering the corpus path, the trainer
flags, the quantization format, the validation commands and the install step.
The goal is a one-click, always-available pipeline: open the GUI, press Train,
and let the studio run the configured trainer backend, watch progress, validate
the result and offer to install it beside the engine. The default `koi` backend
drives `tools/measurement/train_nnue_koi.py`; `--backend torch` still drives the
legacy `tools/measurement/train_nnue_sf.py`.

## Shape

Three layers, all under `tools/nnue/`:

1. `studio_core.py` — framework-free logic: run directories, detached process
   launch, log tailing, progress parsing, gate/AB validation, install.
2. `backends/` — one module per trainer backend implementing a small protocol
   (`name`, `available()`, `build_command()`, `net_path()`). The default `koi`
   backend wraps the CPU v4 trainer, `torch` wraps the legacy v3 trainer, and
   `bullet` drives the Rust/CUDA trainer.
3. `koi_nnue_studio.py` — headless entry points plus the tkinter GUI, which is
   a thin event pump over the core.

Launchers: `Koi NNUE Studio.cmd` (double-click GUI), `tools/nnue/train.ps1`
(headless preset run), `tools/nnue/create-shortcut.ps1` (Desktop/Start Menu
shortcut), `tools/nnue/ab_match.ps1` (network vs classical match) and
`tools/nnue/net_match.ps1` (network vs network match).

## Run directory contract

Every run lives in `artifacts/training/runs/<stamp>-<kind>-<backend>/` and
contains:

| File | Purpose |
| --- | --- |
| `config.json` | Requested configuration snapshot. |
| `state.json` | Mutable state: status, pid, progress history, timestamps. |
| `command.txt` / `command.json` | Exact command line (reproducibility). |
| `train.log` / `train.err` | Combined output and errors. |
| `exit_code.txt`, `running.lock`, `pid.txt` | Liveness and completion markers. |
| `net.nnue`, `net.metadata.json`, `net.pt` | Backend artifacts. The network and metadata names follow the configured `net_name` (default `koi.nnue` / `koi.metadata.json`), with a legacy `net.nnue` fallback for older runs. |

Runs are launched detached through a generated `run.cmd`, so closing the GUI
does not stop training. `--detach` on the CLI behaves the same; the default
headless mode streams to the console.

## Progress contract

`studio_core.parse_progress()` translates the trainer's stable log lines:
`loaded N rows ... in T s`, `epoch i/n train_loss .. val_loss .. val_mae_cp ..
time ..s`, `quantization v4 ...`, `selected v4 ...`, and
`wrote <net> (<bytes> bytes) and <metadata>`. If the trainer's wording changes,
only this parser needs to change; the GUI and tests consume events.

## Validation contract

After a successful run the studio can validate a network two ways:

- **Gate**: `koi-bench --nnue <net>` over the 64-position suite; the report
  records matches/positions and rejects nets the loader cannot open.
- **A/B match**: `tools/nnue/ab_match.ps1` runs two node-limited matches
  through `tools/stability/uci_match.ps1`, with the NNUE side loaded via the
  new `-KoiOptions @{EvalFile=...}` seam and the classical side unmodified.
  Colors are split so the network plays both white and black; the aggregate
  report (schema `koi-nnue-studio-ab-match-v1`) carries W/D/L, score,
  percentage and a verdict.

Validation output is a report, not a CI gate: small node-limited matches are
noisy, exactly like the historical manual A/B runs.

## Install contract

"Install" copies the validated net to `koi.nnue` beside the engine executable,
backing up any existing network to `koi.nnue.<stamp>.bak` first. The engine
picks up `koi.nnue` on the next start (`KOI_NNUE_PATH` overrides), and the UCI
`EvalFile` option can switch networks without a restart. Classical evaluation
remains the default whenever no network is installed.

## Bullet backend

`backends/bullet_backend.py` drives `tools/nnue/run_bullet.py`, which converts
the label corpus with `tools/nnue/to_bullet.py`, trains the pinned Rust/CUDA
`bullet_train` crate, measures validation MAE per saved checkpoint and exports
the v4 container through `tools/measurement/export_bullet_v4.py`. The backend
reports `available()` only when the wrapper, a CUDA 12.x `bin` directory, and
the release trainer (or cargo) are present; otherwise `--dry-run --backend
bullet` exits 2 with the named missing piece.

## Constraints

- No engine code changes: the studio is tooling only.
- No network access required; runs are local and resumable by hand.
- The GUI must never block: file work happens in worker threads and the Tk
  thread only drains a queue.
