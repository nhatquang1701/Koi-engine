# NNUE bullet training verification

Verification record for `docs/superpowers/plans/2026-09-18-nnue-bullet-training.md`.

## Environment

- Windows x64, MSVC 14.44.35207 (VS Community), CMake 3.31.6-msvc6, Ninja.
- Python 3.14.5, torch 2.14.0+cpu, python-chess 1.11.2, numpy.
- Rust 1.98.1 at `%USERPROFILE%\.cargo\bin` (added to PATH per command).
- GPU: NVIDIA GeForce GTX 1060 6GB, compute capability 6.1, driver 582.66.
- Branch `koi-engine-v1`; evidence root
  `artifacts/verification/nnue-bullet-training/` (gitignored).

## Phase 0 — baseline and documentation

Baseline HEAD: `57c3d37` ("Record the NNUE Studio UI verification"), in sync
with `origin/koi-engine-v1`. The suite state at this commit was recorded by the
NNUE Studio UI pass:

| Gate | Result | Evidence |
| --- | --- | --- |
| Release CTest (full) | 58/58 passed, 1180.91 s | `artifacts/verification/nnue-studio-ui/ctest-release.log` |
| Debug CTest (`-LE heavy`) | 50/50 passed, 73.91 s | `artifacts/verification/nnue-studio-ui/ctest-debug.log` |
| Python studio suites | 21/21 UI + 12/12 studio | direct runs, 5.296 s |
| GUI smoke | `PASS gui construction` | `--gui-selftest` |

Scaffolding committed with this phase: the plan, this design spec
(`docs/superpowers/specs/2026-09-18-nnue-bullet-training-design.md`), this
record, and one index row in each of the three
`docs/superpowers/{plans,specs,verification}/README.md` tables.

## Phase 1 — dependencies

Pending.

## Phase 2 — dataset conversion

Pending.

## Phase 3 — training crate, exporter, progress wrapper

Pending.

## Phase 4 — studio backend

Pending.

## Phase 5 — training campaign and gates

Pending.

## Phase 6 — NNUE bug hunt

Pending.

## Phase 7 — documentation mismatch audit

Pending.

## Phase 8 — verification and CI refresh

Pending.

## Limitations

Pending.
