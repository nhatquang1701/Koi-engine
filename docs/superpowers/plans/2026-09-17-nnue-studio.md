# NNUE Studio implementation plan

Status: complete (2026-09-17). One commit.

## Tasks

- [x] **Task 1 — Core.** `tools/nnue/studio_core.py`: run directories under
      `artifacts/training/runs/`, config/state/command persistence, detached
      `cmd.exe` launch with pid/lock/exit-code markers, log tailing, progress
      parser, corpus row counting, gate validation (`koi-bench --nnue`),
      A/B validation, install with backup, JSON helpers.
- [x] **Task 2 — Backends.** `tools/nnue/backends/` with the
      `TorchBackend` (existing CPU trainer, v3 shifts) and a scaffolded
      `BulletBackend` that reports why it is unavailable and documents the
      pieces a Rust trainer needs.
- [x] **Task 3 — CLI and GUI.** `tools/nnue/koi_nnue_studio.py`: headless
      `--list-backends`, `--dry-run`, `--run`, `--selftest`,
      `--gui-selftest`; tkinter GUI with Data / Train / Validate and install /
      Runs tabs, queue-based pumping, live log and val-MAE chart, automatic
      validation after a run and an install prompt, run list with attach /
      use-network / stop / open-folder.
- [x] **Task 4 — Match seam.** `tools/stability/uci_match.ps1` gained
      `-KoiOptions [hashtable]` which sends extra `setoption` lines sorted;
      `tools/nnue/ab_match.ps1` runs the NNUE-vs-classical two-leg match and
      aggregates W/D/L into a JSON report with a verdict.
- [x] **Task 5 — Launchers.** `Koi NNUE Studio.cmd` (GUI, pythonw when
      present), `tools/nnue/train.ps1` (headless preset wrapper),
      `tools/nnue/create-shortcut.ps1` (Desktop/Start-Menu shortcut).
- [x] **Task 6 — Tests.** `tests/python/nnue/studio_test.py` (progress parser,
      presets, `--list-backends`, `--dry-run`, bullet rejection, GUI smoke,
      torch-gated tiny selftest) plus the `nnue_studio_python` CTest
      registration with the `python` label.
- [x] **Task 7 — Docs.** This plan, the design spec, the verification record,
      index rows, README section and the `tests/README.md` inventory row.

## Verification

- `python tools/nnue/koi_nnue_studio.py --list-backends` / `--dry-run` /
  `--gui-selftest` / `--selftest --rows 2000 --epochs 1` all exit 0.
- `pwsh -NoProfile -File tools/nnue/train.ps1 -Preset quick -Rows 1000 -Epochs 1`
  exits 0 and writes a network plus metadata.
- `pwsh -NoProfile -File tools/nnue/ab_match.ps1 -Games 2 -Nodes 2000` produces
  the aggregate report and a verdict.
- `ctest -R nnue_studio_python` passes; the full Release suite stays green.

## Constraints

- Tooling only: no engine, search or UCI behavior changes.
- The existing torch trainer stays the default backend; classical evaluation
  remains the engine default.
- Do not commit `.opencode/`, training corpora or network artifacts (all under
  gitignored `artifacts/`).
