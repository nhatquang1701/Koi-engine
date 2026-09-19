# GPU NNUE inference (sm_61 first)

Goal: add an opt-in GPU path for the version 5 NNUE evaluation, optimized for
this machine's GTX 1060 (sm_61) only. Other GPUs and architectures are deferred
to later plans. No strength or Elo claims; the classical evaluator remains the
engine default and the CPU scalar evaluation stays the correctness boundary.

## Locked decisions (approved before implementation)

- Integration: search-integrated batched evaluation for `Threads > 1` only; the
  CPU path stays in charge at `Threads == 1`.
- Opt-in surface: environment variable only (`KOI_GPU_NNUE=1`). The advertised
  UCI handshake and the `EvalFile` semantics stay unchanged.
- Kernel delivery: build-time `nvcc -ptx -arch=compute_61` into an embedded
  header; runtime uses the driver API (`nvcuda.dll`) loaded dynamically, and a
  build without `nvcc` stays CPU-only.
- Accuracy: bit-exact integer parity with the CPU scalar evaluation.

## Phases

- G0 [x] CMake `nvcc` -> PTX -> embedded-literal pipeline, the dynamic
  `CudaDriver` loader, and the `koi_gpu_probe` device/launch probe.
- G1 [x] The `koi_nnue_v5_eval` kernel, the `GpuNnueService` batch API, and the
  bit-exact parity tests (five golden positions; batch sizes 1/2/3/64/256).
- G2 [x] The `GpuNnueEvaluator` adapter, `KOI_GPU_NNUE` gating, `Threads > 1`
  wiring, CPU fallback, one-time stderr confirmation, and the engine smokes.
- G3 [x] The leader-follower batcher, indicative benchmarks, the README,
  `tools/README.md`, and `tests/README.md` updates, and the verification record.

## Constraints

- No committed artifacts, networks, or run directories.
- No strength, speed, or Elo claims; the CPU result is the reference.
- The frozen UCI handshake and `EvalFile` semantics must not change.
- The build must stay green and CPU-only when `nvcc` is unavailable.
