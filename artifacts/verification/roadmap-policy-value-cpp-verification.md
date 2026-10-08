# C++ policy/value v1 model verification

Date: 2026-09-27  
Branch: `koi-engine`  
Base commit: `b49dee4 perf(search): skip qsearch move generation on valid TT cutoffs`  
Change status: local, uncommitted, not pushed

## Implementation

Added `PolicyValueModel` as an immutable CPU loader for the separate `KOIPV1`
container. It validates the 40-byte little-endian header, model/schema/action
versions, fixed dimensions, exact file and payload sizes, CRC32, and every
float32 weight. Inference consumes sorted, side-to-move-relative v5 sparse
features and native legal moves; it mirrors only action squares for Black and
writes legal-order priors and WDL into caller-provided storage without
per-evaluation heap allocation. Alpha-beta and NNUE defaults are unchanged;
the model is not yet selected by UCI or MCTS.

No trained network or binary model file was added. C++ tests synthesize valid
and malformed containers in memory and use a temporary file only for file-load
coverage.

## Verification

- TDD: the first test run failed at runtime because the C++ model API was
  absent; after implementation, all five C++ model tests passed.
- Release: all configured MSVC Release variants and tools built successfully.
- Debug: the C++ model target built and passed 1/1 with C++ modules and GPU
  NNUE disabled.
- Focused Python/C++ model tests passed 2/2.
- Full Release CTest passed 78/78 at `-j2` in 534.26 seconds. Full log:
  `roadmap-policy-value-cpp-release-ctest.log`.
- `python -m mkdocs build --strict` exited 0. Its output retained the existing
  Material for MkDocs notice and historical-file nav advisory.
- Cross-language golden fixture: Python and C++ produced equal priors, WDL,
  and side-to-move value for both White and vertically mirrored Black moves.

## Linux validation

Docker Desktop 29.8.0 was started hidden, and the repository was bind-mounted
into the existing Ubuntu 22.04 image. The container installed GCC 14, CMake,
Ninja, PowerShell, and the repository's declared NumPy requirement. Linux
Release was built with named modules and shadow-diff enabled, GPU NNUE off;
full CTest then passed 72/72 after correcting an optional-PyTorch test issue.
The Linux Debug modules-off build passed the C++ model test 1/1. Logs:
`roadmap-policy-value-linux-container.log` and
`roadmap-policy-value-linux-debug.log`.

The first Linux CTest run passed 71/72. `policy_value_trainer_python` had two
errors because the minimal CI dependency set intentionally omits optional
PyTorch: the real CPU-training test lacked an availability skip, and the CUDA
provenance helper test assumed a real `torch` module. The test now skips actual
training without PyTorch and supplies a small version stub for provenance
coverage. A simulated no-Torch run passed; the complete Linux CTest rerun then
passed 72/72. CPU training remains tested on this Windows host where PyTorch is
installed. No WSL or OS settings were changed.
