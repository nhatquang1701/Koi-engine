# GPU NNUE inference verification (sm_61 first)

Plan: [2026-09-20-gpu-nnue-inference.md](../plans/2026-09-20-gpu-nnue-inference.md)

## Environment

- Windows x64, MSVC 14.44 (Visual Studio 2022 Community), CMake 3.31 + Ninja,
  Release tree under `build/release`.
- NVIDIA GeForce GTX 1060 6 GB, compute capability sm_61 (Pascal), driver
  582.66, CUDA toolkit 12.9.1 (`C:\Program Files\NVIDIA GPU Computing
  Toolkit\CUDA\v12.9`). CUDA 13.x no longer supports Pascal, so 12.9 is required
  for both the PTX build and the driver.
- No kernels, networks, or run artifacts are committed; everything lives in
  `build/` or the git-ignored `artifacts/` tree.

## Scope

GPU inference is an opt-in acceleration for the version 5 NNUE evaluation:

- `KOI_GPU_NNUE=1` enables the feature; the advertised UCI handshake and
  `EvalFile` semantics are unchanged, and no UCI option was added.
- The GPU path is used only while `Threads > 1`; Threads=1 keeps the
  deterministic CPU path.
- Only version 5 containers with hidden 1536 and L1 32 are accepted by the
  kernel; anything else falls back to the CPU network with one stderr line.
- Any driver, device, module, upload, launch, download, or capacity failure
  falls back to the CPU evaluation for that request, so the engine always
  produces a legal result.

## Build pipeline (nvcc -> embedded PTX -> driver API)

- `CMakeLists.txt` detects `nvcc` (`$ENV{CUDA_PATH}/bin` plus the v12.9 default
  hint) behind `KOI_ENABLE_GPU_NNUE`. When found it compiles
  `src/koi/gpu/koi_nnue_v5.cu` with `-ptx -arch=compute_61` into
  `build/release/generated/koi_nnue_v5.ptx` and `cmake/embed_ptx.cmake` writes
  `koi_nnue_v5_ptx.hpp` (chunked escaped string literals; MSVC's C2026 literal
  limit and dangling-escape chunk boundaries are both handled).
- Assembly-time only: the engine never links CUDA libraries. `nvcuda.dll` is
  loaded dynamically (`src/koi/gpu/cuda_driver.{hpp,cpp}`) and only the driver
  entry points that are actually resolved to non-null are used.
- Without `nvcc` the build stays CPU-only: `KOI_GPU_INFERENCE_AVAILABLE=0` and
  the service compiles to a stub that reports the feature as unavailable.

## Kernel

`koi_nnue_v5_eval` (one block per position, 256 threads, static shared memory):

1. The first thread enumerates both feature views from a compact position
   (occupancy, piece codes, side to move, both king squares): group A
   `halfka-king-bucket-v1` and the symmetric `threat-pairs-v1` relations for
   both colours, deduplicated through a shared 2304-bit bitmap per view.
2. Each thread owns six hidden slots per view and accumulates the active
   feature rows in int64 from the hidden bias, then applies the CReLU clamp to
   0..127 exactly once (the CPU semantics).
3. Eight warps compute the 32 L1 units over the full-width cross pair products
   `a_own[j] * a_opp[j]`, with a warp-shuffle reduction, the L1 shift, and the
   0..127 clamp.
4. One thread computes the piece-count bucket, the output dot product, the
   output shift, and clamps to int32.
   No floating point is used anywhere. A shared counter raises an overflow flag
   if a view exceeds the 320-feature capacity; the service then reports failure
   and the request falls back to the CPU.

## Evidence

- `build/release/koi_gpu_probe.exe`: `gpu-probe: device NVIDIA GeForce GTX
  1060 6GB (sm_61), driver 13000`, `gpu-probe: score 0 overflow 0`,
  `gpu-probe: OK`. This exercises the whole PTX/driver path with zero weights.
- `build/release/gpu_nnue_tests.exe`: `PASS GPU NNUE matches CPU scalar`,
  `PASS GPU NNUE batch sizes agree` (`run=2 pass=2 fail=0`). The tests build a
  deterministic hidden-1536/L1-32 network and compare GPU scores with the CPU
  `NnueWorker` scalar evaluation on five golden positions (startpos, two
  Italian midgames, kiwipete, a pawn endgame) and across batch sizes
  1/2/3/64/256. The GPU result is bit-exact in every case.
- Engine smokes (Threads=2, `go depth 3`):
  - boot path with `KOI_NNUE_PATH=<net>` and `KOI_GPU_NNUE=1` printed
    `koi-engine: GPU NNUE inference enabled.` and returned a legal bestmove.
  - `setoption name EvalFile <net>` with `KOI_GPU_NNUE=1` printed the same
    line and returned a legal bestmove.
  - `KOI_GPU_FORCE_FAIL=1` still returned a legal bestmove through the CPU
    fallback, and a hidden-32 network was rejected by the service with the
    documented fallback message.

## Performance

Timed `koi-bench.exe --nnue <hidden-1536 v5 net>` on this host (wall clock,
whole benchmark; single measurement per cell, so treat as indicative):

| Threads | CPU | GPU (`KOI_GPU_NNUE=1`) |
| --- | --- | --- |
| 1 | 15.95 s | 16.34 s (GPU gated off) |
| 2 | 11.94 s | 13.43 s |
| 4 | 17.49 s | 15.21 s |

The leader-follower batcher (up to `KOI_GPU_BATCH`, default 256, requests per
kernel launch) is active, but the result is roughly parity rather than a clear
speedup: launch latency and low request concurrency dominate, and the AVX2 CPU
evaluation is already fast at hidden 1536. A meaningful GPU win would need a
request-accumulation window or a GPU-resident search design, which this pass
does not attempt.

## Known limitations

- sm_61 / `compute_61` PTX only; other GPUs and other architectures are out of
  scope for this pass.
- No speed claim: the GPU path is functional, bit-exact, and opt-in, but on
  this machine it does not beat the CPU AVX2 path.
- No strength or Elo claim of any kind; the classical evaluator remains the
  engine default and NNUE stays opt-in.
- Threads=1 always uses the CPU path, so perft, fixed-depth single-thread
  runs, and the existing deterministic suite are unaffected.
- The bench numbers were taken on a machine that also runs other work; they
  are not a controlled performance measurement.
