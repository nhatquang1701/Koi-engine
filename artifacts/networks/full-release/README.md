# Network files in this directory

WARNING: the v5 files here are synthetic test fixtures, NOT trained networks.
Loading one with `EvalFile` makes the engine play with random weights.

| File | What it is |
| --- | --- |
| koi-v4-release.nnue, koi-v4-repeat.nnue, koi-v4.nnue | v4 test fixtures (590,219 B; synthetic weights) |
| koi-v5-release.nnue | v5 GPU fixture (113,302,043 B; sha256 45AB9BE6A873EE00EABA1864CEEC6ACDB46FA963C805AD667019BEADA5224E39; identical to gpu-fixture-validation/release-v5.nnue) |
| koi-v5.nnue, koi-v5-repeat.nnue | v5 synthetic test container (2,359,931 B; {36864, 32, 8, 8}; sha256 E84A4BE1D631200FB3E22A28F7B89982870D13892C26AB91001A5C33D2FE0701) |

Trained networks available elsewhere in the repository:

- `artifacts/training/bullet/koi-v4-bullet/koi.nnue` — trained v4 net (18,882,699 B); loads and evaluates sanely.
- `artifacts/training/wp48*`, `artifacts/training/bullet/wp48*` — v5 smoke/experiment nets (weak; up-pawn evals within about +/-60 cp).

The release packages do not ship any network file; NNUE assets are user-supplied.
