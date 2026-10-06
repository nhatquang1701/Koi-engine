# Koi NNUE Studio (removed)

Status: **removed on 2026-10-06**. This page records what the Koi NNUE Studio
was, what it did, and what replaced it, so the removal stays traceable.

## What it was

The Koi NNUE Studio was a local training front end for Koi's NNUE networks. It
consisted of:

- `Koi NNUE Studio.cmd` (Windows) and `koi-nnue-studio.sh` (Linux/macOS)
  launchers that opened a tkinter GUI.
- `tools/nnue/koi_nnue_studio.py` (1340 lines): the GUI with **Data**,
  **Train**, **Validate and install**, and **Runs** tabs, plus a headless CLI
  (`--list-backends`, `--dry-run`, `--run`, `--selftest`).
- `tools/nnue/studio_core.py` (1255 lines): run directories under
  `artifacts/training/runs/<stamp>-<kind>-<backend>/`, progress parsing,
  validation and install helpers, and the detached run launcher.
- `tools/nnue/backends/` (299 lines): a trainer backend registry with three
  adapters — `koi` (the in-tree `tools/measurement/train_nnue_koi.py`, v4/v5),
  `torch` (the legacy `train_nnue_sf.py` v3 trainer), and `bullet` (the pinned
  Rust/CUDA crate under `tools/nnue/bullet_train/` driven through
  `tools/nnue/run_bullet.py`).
- `tools/nnue/train.ps1` (52 lines): a headless wrapper around
  `koi_nnue_studio.py --run <preset>` for quick/standard/thorough presets.
- `tools/nnue/create-shortcut.ps1` (37 lines): Desktop/Start Menu shortcut
  helper for the GUI.
- Tests: `tests/python/nnue/studio_test.py` (181 lines; progress parser,
  backend CLI contract, GUI smoke, torch-gated tiny training selftest) and
  `tests/python/nnue/studio_ui_test.py` (605 lines; run-list and telemetry
  helpers, validation gating, network naming, numeric guards).

The GUI's Validate tab drove the 64-position `koi-bench --nnue` gate and
node-limited A/B matches, then offered to install the produced network as
`koi.nnue` beside the engine (backing up any previous file). Reports from that
era used the schema name `koi-nnue-studio-ab-match-v1`.

## Why it was removed

- The project's actual NNUE workflow ran through the command-line tools
  directly (`train_nnue_koi.py`, `run_bullet.py`, `to_bullet.py`,
  `export_bullet_v4.py`/`export_bullet_v5.py`, `net_match.ps1`,
  `ab_match.ps1`); the GUI was not part of any release artifact or CI strength
  path.
- It carried a disproportionate maintenance surface: a tkinter UI, three
  backend adapters, run-directory telemetry, and two dedicated Python test
  modules.
- The maintainer requested complete removal on 2026-10-06, including its
  documentation.

## What replaced it

- Training: `tools/measurement/train_nnue_koi.py` (v4 and v5 architectures) and
  the bullet pipeline (`tools/nnue/to_bullet.py`, `tools/nnue/run_bullet.py`,
  `tools/nnue/bullet_train/`, `tools/measurement/export_bullet_v4.py`,
  `tools/measurement/export_bullet_v5.py`).
- Validation: the `koi-bench --nnue` gate plus the node-limited, colour-split
  A/B matches in `tools/nnue/ab_match.ps1` (report schema now
  `koi-nnue-ab-match-v1`) and `tools/nnue/net_match.ps1` (schema
  `koi-nnue-net-match-v1`).
- The user guide and developer tool reference document the surviving
  command-line workflow.

## Removed paths

```
Koi NNUE Studio.cmd
koi-nnue-studio.sh
tools/nnue/koi_nnue_studio.py
tools/nnue/studio_core.py
tools/nnue/train.ps1
tools/nnue/create-shortcut.ps1
tools/nnue/backends/__init__.py
tools/nnue/backends/koi_backend.py
tools/nnue/backends/torch_backend.py
tools/nnue/backends/bullet_backend.py
tests/python/nnue/studio_test.py
tests/python/nnue/studio_ui_test.py
```

The CTest registrations `nnue_studio_python` and `nnue_studio_ui_python` were
removed with them. Run directories previously produced under
`artifacts/training/runs/` remain on disk as local, ignored artifacts; no
tooling reads them.
