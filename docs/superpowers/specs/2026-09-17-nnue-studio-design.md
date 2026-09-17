# Koi NNUE Studio design

Status: implemented on 2026-09-17. The studio is a local convenience front end
over the existing training pipeline; it adds no new engine behavior.

## Problem

Training a Koi network required remembering the corpus path, the trainer
flags, the quantization format, the validation commands and the install step.
The goal is a one-click, always-available pipeline: open the GUI, press Train,
and let the studio run the existing `tools/measurement/train_nnue_sf.py`
trainer, watch progress, validate the result and offer to install it beside
the engine.

## Shape

Three layers, all under `tools/nnue/`:

1. `studio_core.py` — framework-free logic: run directories, detached process
   launch, log tailing, progress parsing, gate/AB validation, install.
2. `backends/` — one module per trainer backend implementing a small protocol
   (`name`, `available()`, `build_command()`, `net_path()`). The torch backend
   wraps the existing CPU trainer; a bullet backend is scaffolded for the
   future Rust trainer.
3. `koi_nnue_studio.py` — headless entry points plus the tkinter GUI, which is
   a thin event pump over the core.

Launchers: `Koi NNUE Studio.cmd` (double-click GUI), `tools/nnue/train.ps1`
(headless preset run), `tools/nnue/create-shortcut.ps1` (Desktop/Start Menu
shortcut), `tools/nnue/ab_match.ps1` (A/B validation match).

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
| `net.nnue`, `net.metadata.json`, `net.pt` | Backend artifacts. |

Runs are launched detached through a generated `run.cmd`, so closing the GUI
does not stop training. `--detach` on the CLI behaves the same; the default
headless mode streams to the console.

## Progress contract

`studio_core.parse_progress()` translates the trainer's stable log lines:
`epoch i/n train_loss .. val_loss .. val_mae_cp .. time ..s`,
`quantization v3 ...`, `selected v3 ...`, and
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

## Bullet backend (planned)

`backends/bullet_backend.py` documents the three pieces the Rust trainer
needs: a `to_binpack.py` corpus converter, a `bullet_train` Cargo crate with a
CLI compatible with the studio's config, and a log adapter matching the
progress contract. The backend reports `available() == False` with that
reason, and `--dry-run --backend bullet` exits 2 rather than pretending to
work.

## Constraints

- No engine code changes: the studio is tooling only.
- No network access required; runs are local and resumable by hand.
- The GUI must never block: file work happens in worker threads and the Tk
  thread only drains a queue.
