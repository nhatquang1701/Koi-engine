# NNUE Studio verification

Date: 2026-09-17. Scope: the `tools/nnue/` studio (GUI + headless pipeline),
the `-KoiOptions` match seam, the new Python test, and the docs. No engine
behavior changed.

## Environment

- Windows x64, Intel i3-10100F (4C/8T), 34 GB RAM.
- Python 3.14.5 with `torch` 2.14.0 (CPU), `numpy`, `python-chess`; tkinter
  present; `cargo` 1.98.1 available for the future bullet backend.
- Release tree `build\release` (MSVC 14.44.35207, Ninja), `koi-engine.exe`,
  `koi-bench.exe`, `koi-replay.exe` present.
- Existing artifacts used by the selftest: `artifacts/training/labels.txt`
  (1.2M SF19 depth-10 rows), `artifacts/training/koi-sf-v1.nnue`.

## Changed surface

| File | Change |
| --- | --- |
| `tools/nnue/studio_core.py` | New: run directories, detached launch, tailing, progress parser, gate + A/B validation, install with backup. |
| `tools/nnue/backends/torch_backend.py` | New: wraps the existing CPU trainer. |
| `tools/nnue/backends/bullet_backend.py` | New: documented Rust scaffold; reports unavailable with the exact missing pieces. |
| `tools/nnue/koi_nnue_studio.py` | New: headless CLI (`--list-backends`, `--dry-run`, `--run`, `--selftest`, `--gui-selftest`) plus the tkinter GUI. |
| `tools/nnue/ab_match.ps1` | New: NNUE-vs-classical two-leg match and aggregate report. |
| `tools/nnue/train.ps1` | New: headless preset wrapper. |
| `tools/nnue/create-shortcut.ps1` | New: Desktop/Start-Menu shortcut. |
| `Koi NNUE Studio.cmd` | New: double-click GUI launcher (pythonw when present). |
| `tools/stability/uci_match.ps1` | `-KoiOptions [hashtable]` sends extra sorted `setoption` lines. |
| `tests/python/nnue/studio_test.py` | New: 10 unittest cases. |
| `CMakeLists.txt` | Registers `nnue_studio_python` with the `python` label. |
| Docs | Design spec, plan, this record, index rows, README studio section, `tests/README.md` row. |

## Evidence

Headless contract:

- `python tools/nnue/koi_nnue_studio.py --list-backends` exited 0 and printed
  `torch available` plus `bullet unavailable - not implemented yet: needs
  tools/nnue/to_binpack.py and a bullet_train Cargo crate (cargo is available)`.
- `--dry-run --preset quick` printed the full v3 trainer command
  (`train_nnue_sf.py ... --format v3 --hidden-shift 7 --bottleneck-shift 7
  --output-shifts 3 4 5 6 --rows 200000 --float-out ...`); `--backend bullet`
  exited 2 with the reason instead of pretending to work.
- `--gui-selftest` printed `PASS gui construction` (window builds, then is
  destroyed after 400 ms).

End-to-end training:

- `python tools/nnue/koi_nnue_studio.py --selftest --rows 2000 --epochs 1`
  trained (`epoch 1/1 ... val_mae_cp 482.2`), quantized (selected `k3=6`),
  wrote a 501,011-byte v3 net, loaded it through the C++ loader and ran the
  64-position gate: `engine loaded the network; gate matches 47/64` (a
  2000-row, 1-epoch net; the check proves the pipeline and loader, not
  strength).
- `pwsh -NoProfile -File tools/nnue/train.ps1 -Preset quick -Rows 1000 -Epochs 1`
  exited 0 and wrote the network and metadata into a timestamped run directory.

Validation:

- `pwsh -NoProfile -File tools/nnue/ab_match.ps1 -NnueNet artifacts/training/koi-sf-v1.nnue
  -Games 2 -Nodes 2000 -OutputDirectory artifacts/matches/nnue-ab-smoke2` ran
  both legs through `uci_match.ps1`, wrote `ab-match.json`
  (`koi-nnue-studio-ab-match-v1`) and reported `+0 =0 -2` with verdict
  `classical-stronger`. This matches the earlier 20k-node A/B result (0-4-5)
  and is reported as a small-sample local observation, not an Elo claim.

Tests:

- `python -m unittest tests/python/nnue/studio_test.py -v` -> 10 tests OK
  (progress parser, presets, backends CLI, bullet rejection, GUI smoke,
  torch-gated tiny selftest).
- `ctest --test-dir build\release -C Release -R nnue_studio_python
  --output-on-failure` -> passed in 7.71 s, registered as test 54.
- Full parallel Release suite
  (`tools/test/run_tests.ps1 -NoBuild -Parallel 8`) ->
  **100% tests passed, 0 failed out of 54, Total Test time (real) 163.20 s**
  (`artifacts/verification/2026-09-17-nnue-studio/suite`).

## Limitations

- The torch backend is CPU-only; the bullet (Rust) backend is a documented
  scaffold, not an implementation.
- The GUI was verified by construction smoke and the CLI tests; its interactive
  flows (charts, install prompt) were exercised by hand during development but
  are not part of CTest.
- The A/B validation is node-limited and small by default and is not a CI
  gate; the studio treats the aggregate as a local report.
- Installing a network overwrites `koi.nnue` beside the engine, keeping one
  timestamped `.bak` copy; the studio does not manage multiple installed nets.
- Network files, corpora and run directories live under gitignored
  `artifacts/` and are not committed.
