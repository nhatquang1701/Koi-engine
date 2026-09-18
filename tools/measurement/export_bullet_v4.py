#!/usr/bin/env python3
"""Export a bullet checkpoint to a KOI-NNUE v4 container.

Bullet saves raw f32 weights in ``<out>/<net_id>/<net_id>-N/raw.bin`` in
``save_format`` order: ``l0w`` (hidden x 9216, column-major = feature-major),
``l0b`` (hidden), ``l1w`` (8 x hidden/2, column-major), ``l1b`` (8). The
network output is in units of 100 cp because bullet trains with
``eval_scale = 100`` and a pure evaluation target, so the standard v4
quantization applies:

    W1_q = clip(rint(W1 * 2^s1), +-32767)
    b1_q = rint(b1 * 2^s1)
    W2_q = clip(rint(W2 * 100 * 2^k3 / 2^(2 s1)), +-127)
    b2_q = rint(b2 * 100 * 2^k3)

The hidden/output shift pair is chosen by grid search over a sample of the
validation text file produced by ``to_bullet.py`` and the same progress lines
as the PyTorch trainer are printed so the NNUE Studio can parse them.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
import time
from pathlib import Path

import numpy as np

REPO_ROOT = Path(__file__).resolve().parents[2]
MEASUREMENT_DIR = REPO_ROOT / "tools" / "measurement"
if str(MEASUREMENT_DIR) not in sys.path:
    sys.path.insert(0, str(MEASUREMENT_DIR))

import koi_dataset  # noqa: E402
import train_nnue_koi  # noqa: E402

BYTES_PER_HIDDEN = train_nnue_koi.INPUT_UNITS * 4 + 4 + train_nnue_koi.OUTPUT_BUCKETS * 2
DEFAULT_CHECKPOINT = REPO_ROOT / "artifacts" / "training" / "bullet"
DEFAULT_NET_OUT = REPO_ROOT / "artifacts" / "training" / "koi-v4-bullet.nnue"
DEFAULT_META_OUT = REPO_ROOT / "artifacts" / "training" / "koi-v4-bullet.metadata.json"


class ExportError(RuntimeError):
    """Raised for malformed checkpoints or validation files."""


def log(message: str) -> None:
    print(message, flush=True)


def infer_hidden_units(raw_bytes: int) -> int:
    if raw_bytes <= 32 or (raw_bytes - 32) % BYTES_PER_HIDDEN != 0:
        raise ExportError(
            f"raw.bin has {raw_bytes} bytes; expected 32 + hidden * {BYTES_PER_HIDDEN}"
        )
    hidden = (raw_bytes - 32) // BYTES_PER_HIDDEN
    if hidden < 32 or hidden % 2 != 0 or hidden > 8192:
        raise ExportError(f"raw.bin implies an invalid hidden width of {hidden}")
    return hidden


def latest_checkpoint(directory: Path) -> Path:
    if directory.is_file():
        return directory
    candidates = sorted(
        (
            path
            for pattern in ("*-*", "*/*-*")
            for path in directory.glob(pattern)
            if (path / "raw.bin").is_file()
        ),
        key=lambda path: (path.stat().st_mtime, path.name),
    )
    if not candidates:
        raise ExportError(f"no checkpoint with raw.bin under {directory}")
    return candidates[-1]


def read_raw_weights(raw_path: Path) -> tuple[int, dict[str, np.ndarray]]:
    raw = raw_path.read_bytes()
    hidden = infer_hidden_units(len(raw))
    half = hidden // 2
    offset = 0

    def take(count: int) -> np.ndarray:
        nonlocal offset
        end = offset + count * 4
        values = np.frombuffer(raw[offset:end], dtype="<f4").astype(np.float64)
        offset = end
        return values

    # bullet stores affine weights column-major (see SavedFormat::transpose_impl
    # and ModelBuilder::new_affine, which registers shape (output, input)), so
    # l0w is already feature-major: flat[i * hidden + h]. Reading it row-major as
    # (INPUT_UNITS, hidden) therefore matches the KOI v4 payload layout directly.
    feature_weights = take(hidden * train_nnue_koi.INPUT_UNITS).reshape(
        train_nnue_koi.INPUT_UNITS, hidden
    )
    hidden_bias = take(hidden)
    # l1w has shape (8, half) column-major, which is the same byte order as
    # row-major (half, 8); transposing gives the bucket-major (8, half) form.
    output_column_major = take(train_nnue_koi.OUTPUT_BUCKETS * half).reshape(half, train_nnue_koi.OUTPUT_BUCKETS)
    output_weights = output_column_major.T
    output_bias = take(train_nnue_koi.OUTPUT_BUCKETS)
    if offset != len(raw):
        raise ExportError("raw.bin has trailing bytes after the saved tensors")
    return hidden, {
        "feature_weights": feature_weights,
        "hidden_bias": hidden_bias,
        "output_weights": output_weights,
        "output_bias": output_bias,
    }


def load_validation(path: Path, limit: int, seed: int) -> tuple[np.ndarray, np.ndarray, np.ndarray, np.ndarray]:
    import chess

    indices: list[int] = []
    offsets = [0]
    scores: list[int] = []
    buckets: list[int] = []
    skipped = 0
    with path.open("r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            line = line.strip()
            if not line:
                continue
            parts = line.split("|")
            if len(parts) != 3:
                skipped += 1
                continue
            fen = parts[0].strip()
            try:
                white_cp = int(round(float(parts[1].strip())))
                board = chess.Board(fen)
            except (ValueError, OverflowError, TypeError):
                skipped += 1
                continue
            stm_cp = white_cp if board.turn == chess.WHITE else -white_cp
            sparse = koi_dataset.halfka_king_bucket_indices(board)
            indices.extend(sparse)
            offsets.append(len(indices))
            scores.append(stm_cp)
            buckets.append(koi_dataset.output_bucket(board))
    if len(scores) <= 1:
        raise ExportError(f"no usable rows in {path} (skipped {skipped})")
    if skipped:
        log(f"export: skipped {skipped} malformed validation rows")
    rows = len(scores)
    if limit > 0 and rows > limit:
        rng = np.random.default_rng(seed)
        chosen = np.sort(rng.choice(rows, size=limit, replace=False))
        index_array = np.asarray(indices, dtype=np.int64)
        new_indices: list[int] = []
        new_offsets = [0]
        for row in chosen:
            new_indices.extend(index_array[offsets[row]:offsets[row + 1]].tolist())
            new_offsets.append(len(new_indices))
        indices = new_indices
        offsets = new_offsets
        scores = [scores[row] for row in chosen]
        buckets = [buckets[row] for row in chosen]
    return (
        np.asarray(indices, dtype=np.int64),
        np.asarray(offsets, dtype=np.int64),
        np.asarray(scores, dtype=np.int64),
        np.asarray(buckets, dtype=np.int64),
    )


def quantize_candidate(weights: dict[str, np.ndarray], hidden_shift: int, output_shift: int) -> dict:
    scale = float(1 << hidden_shift)
    output_scale = train_nnue_koi.TARGET_SCALE * float(1 << output_shift)
    return {
        "feature_weights": np.clip(
            np.rint(weights["feature_weights"] * scale),
            -train_nnue_koi.W1_LIMIT,
            train_nnue_koi.W1_LIMIT,
        ).astype(np.int64),
        "hidden_bias": np.rint(weights["hidden_bias"] * scale).astype(np.int64),
        "output_weights": np.clip(
            np.rint(weights["output_weights"] * output_scale / (scale * scale)),
            -train_nnue_koi.W2_LIMIT,
            train_nnue_koi.W2_LIMIT,
        ).astype(np.int64),
        "output_bias": np.rint(weights["output_bias"] * output_scale).astype(np.int64),
        "output_shift": output_shift,
    }


def float_activations(
    weights: dict[str, np.ndarray],
    indices: np.ndarray,
    offsets: np.ndarray,
    samples: np.ndarray,
    hidden_units: int,
) -> np.ndarray:
    hidden = np.tile(weights["hidden_bias"], (samples.size, 1))
    for position, sample in enumerate(samples):
        begin = offsets[sample]
        end = offsets[sample + 1]
        if end > begin:
            hidden[position, :] += weights["feature_weights"][indices[begin:end], :].sum(axis=0)
    np.clip(hidden, 0.0, 1.0, out=hidden)
    return hidden


def float_scores(weights: dict[str, np.ndarray], hidden: np.ndarray, buckets: np.ndarray) -> np.ndarray:
    half = hidden.shape[1] // 2
    pairs = hidden[:, :half] * hidden[:, half:]
    return (
        weights["output_bias"][buckets]
        + (pairs * weights["output_weights"][buckets]).sum(axis=1)
    ) * train_nnue_koi.TARGET_SCALE


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Export a bullet checkpoint to a KOI-NNUE v4 container."
    )
    parser.add_argument(
        "--checkpoint",
        type=Path,
        default=DEFAULT_CHECKPOINT,
        help="checkpoint directory or raw.bin path (default: artifacts/training/bullet)",
    )
    parser.add_argument(
        "--validation",
        type=Path,
        default=None,
        help="validation text file from to_bullet.py (default: beside the checkpoint)",
    )
    parser.add_argument("--net-out", type=Path, default=DEFAULT_NET_OUT)
    parser.add_argument("--meta-out", type=Path, default=DEFAULT_META_OUT)
    parser.add_argument(
        "--hidden",
        type=int,
        default=0,
        help="expected hidden width from the training run (0 = infer from raw.bin)",
    )
    parser.add_argument("--hidden-shifts", type=int, nargs="+", default=[6, 7, 8])
    parser.add_argument("--output-shifts", type=int, nargs="+", default=[12, 14, 16, 18, 20])
    parser.add_argument("--tune-samples", type=int, default=4000)
    parser.add_argument("--seed", type=int, default=20260916)
    parser.add_argument("--net-id", default=None, help="metadata net id (default: checkpoint name)")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        checkpoint = latest_checkpoint(args.checkpoint)
        hidden, weights = read_raw_weights(checkpoint / "raw.bin")
    except (ExportError, OSError) as error:
        print(f"export_bullet_v4: {error}", file=sys.stderr)
        return 2

    if args.hidden and args.hidden != hidden:
        print(
            f"export_bullet_v4: checkpoint hidden width {hidden} does not match "
            f"--hidden {args.hidden}",
            file=sys.stderr,
        )
        return 2

    log(f"export: checkpoint {checkpoint} hidden={hidden}")
    if hidden % 16 != 0:
        log(f"export: hidden {hidden} is not a multiple of 16; AVX2 falls back to scalar")

    validation = args.validation
    if validation is None:
        candidate = checkpoint.parents[1] / "validation.txt"
        validation = candidate if candidate.is_file() else None
    if validation is not None and not validation.is_file():
        print(f"export_bullet_v4: validation file {validation} not found", file=sys.stderr)
        return 2

    float_mae = None
    if validation is not None:
        try:
            indices, offsets, scores, buckets = load_validation(validation, args.tune_samples, args.seed)
        except (ExportError, OSError) as error:
            print(f"export_bullet_v4: {error}", file=sys.stderr)
            return 2
        activations = float_activations(weights, indices, offsets, np.arange(scores.size), hidden)
        float_mae = float(np.mean(np.abs(float_scores(weights, activations, buckets) - scores)))
        log(f"export: float val_mae_cp {float_mae:.1f} on {scores.size} rows")
    else:
        log("export: no validation file; writing zero shifts without a search")
        indices = offsets = scores = buckets = None

    best = None
    if scores is not None:
        for hidden_shift in args.hidden_shifts:
            params = quantize_candidate(weights, hidden_shift, args.output_shifts[0])
            hidden_values = train_nnue_koi.hidden_activations(
                params, indices, offsets, np.arange(scores.size), hidden
            )
            for output_shift in args.output_shifts:
                candidate = quantize_candidate(weights, hidden_shift, output_shift)
                predicted = train_nnue_koi.integer_scores(candidate, hidden_values, buckets, hidden)
                mae = float(np.mean(np.abs(predicted - scores)))
                log(f"quantization v4 s1={hidden_shift} k3={output_shift} val_mae_cp {mae:.1f}")
                if best is None or mae < best[0]:
                    best = (mae, hidden_shift, output_shift, candidate)
    if best is None:
        hidden_shift = args.hidden_shifts[0]
        output_shift = args.output_shifts[0]
        params = quantize_candidate(weights, hidden_shift, output_shift)
        mae = None
    else:
        mae, hidden_shift, output_shift, params = best
        log(f"selected v4 s1={hidden_shift} k3={output_shift} val_mae_cp {mae:.1f}")

    payload = train_nnue_koi.nnue_payload_v4(params, hidden)
    container, payload_hash = train_nnue_koi.nnue_container_v4(payload, hidden, hidden_shift, output_shift)
    args.net_out.parent.mkdir(parents=True, exist_ok=True)
    args.net_out.write_bytes(container)
    w1_saturation, w2_saturation = train_nnue_koi.saturation_fractions(params, hidden)
    metadata = {
        "schema": "koi-nnue-training-metadata-v2",
        "created": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "corpus": str(validation) if validation is not None else "",
        "rows_used": int(scores.size) if scores is not None else 0,
        "val_rows": int(scores.size) if scores is not None else 0,
        "feature_set": train_nnue_koi.FEATURE_SET.decode(),
        "quantization": train_nnue_koi.QUANTIZATION.decode(),
        "architecture": {
            "input": train_nnue_koi.INPUT_UNITS,
            "hidden": hidden,
            "output_buckets": train_nnue_koi.OUTPUT_BUCKETS,
        },
        "activation": "crelu-pair",
        "hidden_shift": hidden_shift,
        "output_shift": output_shift,
        "backend": "bullet",
        "checkpoint": str(checkpoint),
        "val_mae_cp": float_mae,
        "val_round_trip_mae_cp": mae,
        "w1_saturation": w1_saturation,
        "w2_saturation": w2_saturation,
        "payload_sha256": payload_hash,
        "network_sha256": hashlib.sha256(container).hexdigest(),
        "command": " ".join(sys.argv),
    }
    args.meta_out.parent.mkdir(parents=True, exist_ok=True)
    args.meta_out.write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    log(f"wrote {args.net_out} ({len(container)} bytes) and {args.meta_out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
