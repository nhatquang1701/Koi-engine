# Koi NNUE Validated Opt-In Phase Design

## Goal

Add a validated, Koi-native NNUE boundary without changing the default
classical evaluator, UCI behavior, search ownership, legality code, or CMake
build wiring. Existing `piece-square-v1` containers remain loadable and
serializable byte-for-byte under the current `KOI-NNUE` format version 2.

## Feature sets and container contract

The existing 72-byte little-endian container header remains unchanged:

1. `KOI-NNUE` magic (8 bytes)
2. format version (u32)
3. four layer sizes (u32 each)
4. quantization string length (u16)
5. feature-set string length (u16)
6. payload byte length (u64)
7. SHA-256 of the payload (32 bytes)
8. quantization string, feature-set string, and payload

The format version remains `2`. The feature set selects the expected input
count, hidden width, and encoder:

| Feature set | Inputs | Layers | Compatibility |
| --- | ---: | --- | --- |
| `piece-square-v1` | 768 | `768->128->32->1` | Existing network contract |
| `piece-square-king-pawn-v2` | 960 | `960->256->32->1` | New opt-in contract |

The payload ordering remains feature-major int16 first-layer weights, int32
first hidden biases, int8 second-layer weights, int32 bottleneck biases, int8
output weights, and an int32 output bias. Serialization computes the payload
length and SHA-256 deterministically; loading rejects malformed lengths,
unknown feature sets, shape mismatches, unsupported quantization, and checksum
failures before exposing weights to inference.

Both feature sets use int16/int8 weights and clipped ReLU after the first two
layers: `clamp(value, 0, 127)`. Accumulation uses a wider signed type before
clipping so valid int16/int8 payloads cannot invoke signed overflow.

## Koi-native v2 feature indexing

The first 768 inputs preserve the v1 absolute-color layout:
`color * 6 * 64 + (piece_type - 1) * 64 + square` for non-king pieces.

Inputs `768..895` are one-hot king-square context:
`768 + color * 64 + king_square`.

Inputs `896..959` are `color * 32 + file * 4 + flag`, with these four flags:

1. pawn presence on the file
2. doubled-pawn presence on the file (at least two pawns)
3. isolated-pawn presence on the file (a pawn with no friendly pawn on either
   adjacent file)
4. passed-pawn presence on the file (a pawn with no enemy pawn on the same or
   adjacent files ahead of it)

All encoded inputs are binary int8 values. The encoder uses the cached
Koi-owned `PositionFeatures`; no native chess-library type crosses the NNUE
boundary.

## Inference and fallback

`NnueWorker` owns mutable accumulators while all network vectors are held by
`shared_ptr<const NnueNetwork>`. Scalar inference is the reference path. An
AVX2-compatible path vectorizes the first-layer accumulation when the binary
has AVX2 support and the runtime CPU query succeeds; it uses the same payload
ordering, clipping, and integer semantics and falls back to scalar safely when
AVX2 is unavailable. Tests invoke both paths and require equal scores and
accumulator values.

`make_evaluator()` continues to choose `ClassicalEvaluator` when no path is
provided or when loading fails. A valid NNUE path is the only opt-in switch;
NNUE is not made the default evaluator.

## Offline training/export boundary

The existing classical-corpus CLI remains compatible. A minimal `nnue`
subcommand accepts train/validation/holdout corpus or manifest inputs, an
output network path, a seed, and optional backend/device selection. The
dependency-free synthetic backend produces deterministic v2 weights for small
tests. An optional PyTorch backend is the only GPU-aware implementation and
reports missing optional libraries clearly. The engine never imports Python.

The exporter writes the Koi container and a stable JSON sidecar containing
canonical corpus hashes and split hashes/counts, seed, architecture and
hyperparameters, exact command, backend/device/dependency information, and the
SHA-256 network payload hash. Repeated synthetic exports with the same inputs
must be byte-identical and test without a GPU or large corpus.

## Scope and verification

Only `src/koi/nnue.hpp/cpp`, `src/koi/evaluation_features.hpp/cpp`, classical
parameter metadata when required for the boundary, `tools/tune_eval.py`, and
NNUE/training tests are in scope. Search, legality, UCI, CMake, and measurement
tools are not changed. Focused C++ NNUE/evaluation tests and Python exporter
tests are run first, followed by the available configured NNUE/evaluation
CTest targets and the existing tune-eval test.
