#!/usr/bin/env python3
"""Export a bullet v5 checkpoint as a KOI-NNUE v5 container.

The v5 trainer (``koi-bullet-train``) saves one ``raw.bin`` per checkpoint
directory.  The file is a flat list of little-endian float32 values in the
same channel order as the saved model nodes:

* ``l0w``: (36864, hidden) input-major feature weights
* ``l0b``: (hidden) feature bias
* ``l1w``: (hidden, l1) input-major layer-1 weights
* ``l1b``: (l1) layer-1 bias
* ``l2w``: (l1, 8) input-major output weights
* ``l2b``: (8) output bias

The exporter reorders ``l1w`` to the container's unit-major order and
``l2w`` to bucket-major order, quantizes the network, searches the shift
grids for the combination with the smallest integer round-trip MAE on a
validation set, and writes the container plus a metadata record with the
schema ``koi-nnue-training-metadata-v3``.

The integer reference matches ``train_nnue_koi.py`` and the C++ runtime:

* ``e = clamp((sum_j W1h[k, j] * own[j] * opp[j] + b1h[k]) >> s_l1, 0, 127)``
* ``y = (b2o[bucket] + sum_k W2o[bucket, k] * e[k]) >> s_out``
"""

from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import pathlib
import struct
import sys

import numpy as np

TOOLS_DIR = pathlib.Path(__file__).resolve().parent
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))

import koi_dataset  # noqa: E402

MAGIC = b"KOI-NNUE"
VERSION = 5
INPUT_UNITS = 36864
OUTPUT_BUCKETS = 8
DEFAULT_L1_UNITS = 32
QUANTIZATION = b"int16/int8"
FEATURE_SET = b"halfka-king-bucket-v1+threat-pairs-v1"
TARGET_SCALE = 100.0
W1_LIMIT = 32767
L1_LIMIT = 127
W2_LIMIT = 127
MIN_HIDDEN = 32
MAX_HIDDEN = 8192
DEFAULT_HIDDEN = 1536
DEFAULT_SEED = 20260916
DEFAULT_TUNE_SAMPLES = 4000


class ExportError(RuntimeError):
    pass


def log(message: str) -> None:
    print(message, flush=True)


def bytes_per_hidden(l1_units: int) -> int:
    return 4 * (INPUT_UNITS + 1 + l1_units)


def tail_bytes(l1_units: int) -> int:
    return 4 * (9 * l1_units + 8)


def infer_hidden_units(raw_size: int, l1_units: int) -> int:
    remaining = raw_size - tail_bytes(l1_units)
    per_hidden = bytes_per_hidden(l1_units)
    if remaining <= 0 or remaining % per_hidden != 0:
        raise ExportError(
            f"raw size {raw_size} is not a valid v5 checkpoint for l1={l1_units}"
        )
    hidden = remaining // per_hidden
    if hidden < MIN_HIDDEN or hidden > MAX_HIDDEN or hidden % 2 != 0:
        raise ExportError(f"inferred hidden units {hidden} are out of range")
    return hidden


def latest_checkpoint(directory: pathlib.Path) -> pathlib.Path:
    if directory.is_file():
        return directory
    if not directory.is_dir():
        raise ExportError(f"checkpoint path not found: {directory}")
    candidates: list[pathlib.Path] = []
    for child in sorted(directory.iterdir()):
        if not child.is_dir():
            continue
        raw = child / "raw.bin"
        if raw.is_file():
            candidates.append(raw)
    if not candidates:
        raise ExportError(f"no raw.bin checkpoints under {directory}")
    return max(candidates, key=lambda path: (path.stat().st_mtime, path.name))


def read_raw_weights(raw_path: pathlib.Path, hidden: int, l1_units: int) -> dict:
    data = np.fromfile(raw_path, dtype="<f4")
    expected = hidden * bytes_per_hidden(l1_units) // 4 + tail_bytes(l1_units) // 4
    if data.size != expected:
        raise ExportError(
            f"{raw_path} has {data.size} floats; expected {expected} for hidden {hidden}"
        )
    position = 0

    def take(count: int) -> np.ndarray:
        nonlocal position
        chunk = data[position : position + count]
        position += count
        return chunk

    l0w = take(INPUT_UNITS * hidden).reshape(INPUT_UNITS, hidden).astype(np.float32)
    l0b = take(hidden).astype(np.float32)
    l1w = take(hidden * l1_units).reshape(hidden, l1_units).T.astype(np.float32)
    l1b = take(l1_units).astype(np.float32)
    l2w = take(l1_units * OUTPUT_BUCKETS).reshape(l1_units, OUTPUT_BUCKETS).T.astype(np.float32)
    l2b = take(OUTPUT_BUCKETS).astype(np.float32)
    if position != data.size:
        raise ExportError(f"{raw_path} has {data.size - position} trailing floats")
    return {
        "feature_weights": l0w,
        "hidden_bias": l0b,
        "l1_weights": l1w,
        "l1_bias": l1b,
        "output_weights": l2w,
        "output_bias": l2b,
    }


def load_validation(path: pathlib.Path, limit: int, seed: int) -> dict:
    import chess  # noqa: PLC0415 - optional dependency imported on use

    rows: list[tuple[int, int, np.ndarray, np.ndarray]] = []
    with path.open("r", encoding="utf-8", errors="replace") as handle:
        for raw in handle:
            text = raw.strip()
            if not text or text.startswith("#"):
                continue
            parts = [part.strip() for part in text.split("|")]
            if len(parts) < 2:
                continue
            try:
                board = chess.Board(parts[0])
            except ValueError:
                continue
            if not board.is_valid():
                continue
            try:
                white_cp = float(parts[1])
            except ValueError:
                continue
            if not np.isfinite(white_cp) or abs(white_cp) > 4000:
                continue
            score = white_cp if board.turn == chess.WHITE else -white_cp
            groups = koi_dataset.encode_record(board)
            own = np.asarray(list(groups[0]) + list(groups[1]), dtype=np.int32)
            opp = np.asarray(list(groups[2]) + list(groups[3]), dtype=np.int32)
            rows.append((int(round(score)), koi_dataset.output_bucket(board), own, opp))
    if not rows:
        return {"scores": np.zeros(0, dtype=np.int32), "buckets": np.zeros(0, dtype=np.int32),
                "own": [], "opp": []}
    if limit and limit < len(rows):
        rng = np.random.default_rng(seed)
        choice = rng.choice(len(rows), size=limit, replace=False)
        rows = [rows[int(index)] for index in choice]
    return {
        "scores": np.asarray([row[0] for row in rows], dtype=np.int32),
        "buckets": np.asarray([row[1] for row in rows], dtype=np.int32),
        "own": [row[2] for row in rows],
        "opp": [row[3] for row in rows],
    }


def quantize(params: dict, hidden_shift: int, l1_shift: int, output_shift: int) -> dict:
    scale = float(2**hidden_shift)
    l1_scale = float(2**l1_shift)
    out_scale = float(2**output_shift)
    return {
        "feature_weights": np.clip(
            np.rint(params["feature_weights"] * scale), -W1_LIMIT, W1_LIMIT
        ).astype(np.int64),
        "hidden_bias": np.rint(params["hidden_bias"] * scale).astype(np.int64),
        "l1_weights": np.clip(
            np.rint(params["l1_weights"] * l1_scale / 127.0), -L1_LIMIT, L1_LIMIT
        ).astype(np.int64),
        "l1_bias": np.rint(params["l1_bias"] * 127.0 * l1_scale).astype(np.int64),
        "output_weights": np.clip(
            np.rint(params["output_weights"] * TARGET_SCALE * out_scale / 127.0),
            -W2_LIMIT,
            W2_LIMIT,
        ).astype(np.int64),
        "output_bias": np.rint(
            params["output_bias"] * TARGET_SCALE * out_scale
        ).astype(np.int64),
        "hidden_shift": hidden_shift,
        "l1_shift": l1_shift,
        "output_shift": output_shift,
    }


def quantize_feature(params: dict, hidden_shift: int) -> dict:
    scale = float(2**hidden_shift)
    return {
        "feature_weights": np.clip(
            np.rint(params["feature_weights"] * scale), -W1_LIMIT, W1_LIMIT
        ).astype(np.int64),
        "hidden_bias": np.rint(params["hidden_bias"] * scale).astype(np.int64),
    }


def quantize_l1(params: dict, l1_shift: int) -> dict:
    l1_scale = float(2**l1_shift)
    return {
        "l1_weights": np.clip(
            np.rint(params["l1_weights"] * l1_scale / 127.0), -L1_LIMIT, L1_LIMIT
        ).astype(np.int64),
        "l1_bias": np.rint(params["l1_bias"] * 127.0 * l1_scale).astype(np.int64),
    }


def quantize_output(params: dict, output_shift: int) -> dict:
    out_scale = float(2**output_shift)
    return {
        "output_weights": np.clip(
            np.rint(params["output_weights"] * TARGET_SCALE * out_scale / 127.0),
            -W2_LIMIT,
            W2_LIMIT,
        ).astype(np.int64),
        "output_bias": np.rint(
            params["output_bias"] * TARGET_SCALE * out_scale
        ).astype(np.int64),
    }


def hidden_activations(feature_params: dict, validation: dict,
                       hidden: int) -> tuple[np.ndarray, np.ndarray]:
    rows = len(validation["scores"])
    own = np.empty((rows, hidden), dtype=np.int64)
    opp = np.empty((rows, hidden), dtype=np.int64)
    bias = feature_params["hidden_bias"]
    weights = feature_params["feature_weights"]
    int32_min = np.iinfo(np.int32).min
    int32_max = np.iinfo(np.int32).max
    for row in range(rows):
        own_sums = bias.copy()
        for index in validation["own"][row]:
            own_sums += weights[int(index)]
        opp_sums = bias.copy()
        for index in validation["opp"][row]:
            opp_sums += weights[int(index)]
        np.clip(own_sums, int32_min, int32_max, out=own_sums)
        np.clip(opp_sums, int32_min, int32_max, out=opp_sums)
        np.clip(own_sums, 0, 127, out=own_sums)
        np.clip(opp_sums, 0, 127, out=opp_sums)
        own[row] = own_sums
        opp[row] = opp_sums
    return own, opp


def integer_scores(own: np.ndarray, opp: np.ndarray, l1_params: dict,
                   output_params: dict, buckets: np.ndarray, l1_shift: int,
                   output_shift: int) -> np.ndarray:
    pairs = own * opp
    l1 = pairs @ l1_params["l1_weights"].T + l1_params["l1_bias"]
    if l1_shift > 0:
        l1 >>= l1_shift
    np.clip(l1, 0, 127, out=l1)
    weights = output_params["output_weights"][buckets]
    bias = output_params["output_bias"][buckets]
    output = (l1 * weights).sum(axis=1) + bias
    if output_shift > 0:
        output >>= output_shift
    return output


def measure_val_mae(raw_path: pathlib.Path, validation_path: pathlib.Path, hidden: int,
                    l1_units: int, hidden_shift: int = 7, l1_shift: int = 6,
                    output_shift: int = 12, limit: int = 2000) -> float | None:
    """Integer validation MAE for one bullet v5 checkpoint, or None when unmeasurable."""
    try:
        weights = read_raw_weights(raw_path, hidden, l1_units)
        validation = load_validation(validation_path, limit, 0)
    except (ExportError, OSError):
        return None
    if len(validation["scores"]) == 0:
        return None
    feature = quantize_feature(weights, hidden_shift)
    l1 = quantize_l1(weights, l1_shift)
    output = quantize_output(weights, output_shift)
    own, opp = hidden_activations(feature, validation, hidden)
    scores = integer_scores(own, opp, l1, output, validation["buckets"], l1_shift, output_shift)
    return float(np.abs(scores - validation["scores"]).mean())


def payload_bytes(params: dict, hidden: int, l1_units: int) -> bytes:
    return b"".join(
        [
            params["feature_weights"].astype("<i2").reshape(-1).tobytes(),
            params["hidden_bias"].astype("<i4").reshape(-1).tobytes(),
            params["l1_weights"].astype("<i1").reshape(-1).tobytes(),
            params["l1_bias"].astype("<i4").reshape(-1).tobytes(),
            params["output_weights"].astype("<i1").reshape(-1).tobytes(),
            params["output_bias"].astype("<i4").reshape(-1).tobytes(),
        ]
    )


def container_bytes(payload: bytes, hidden: int, l1_units: int, hidden_shift: int,
                    l1_shift: int, output_shift: int) -> tuple[bytes, str]:
    digest = hashlib.sha256(payload).hexdigest()
    header = struct.pack(
        "<8sIIIIIBBBBHHQ",
        MAGIC,
        VERSION,
        INPUT_UNITS,
        hidden,
        OUTPUT_BUCKETS,
        l1_units,
        hidden_shift,
        output_shift,
        l1_shift,
        0,
        len(QUANTIZATION),
        len(FEATURE_SET),
        len(payload),
    )
    container = header + bytes.fromhex(digest) + QUANTIZATION + FEATURE_SET + payload
    return container, digest


def saturation_fractions(params: dict) -> tuple[float, float, float]:
    w1 = float(np.mean(np.abs(params["feature_weights"]) >= W1_LIMIT))
    l1 = float(np.mean(np.abs(params["l1_weights"]) >= L1_LIMIT))
    w2 = float(np.mean(np.abs(params["output_weights"]) >= W2_LIMIT))
    return w1, l1, w2


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--checkpoint", required=True, type=pathlib.Path)
    parser.add_argument("--validation", type=pathlib.Path)
    parser.add_argument("--net-out", required=True, type=pathlib.Path)
    parser.add_argument("--meta-out", type=pathlib.Path)
    parser.add_argument("--hidden", type=int, default=0)
    parser.add_argument("--l1-units", type=int, default=DEFAULT_L1_UNITS)
    parser.add_argument("--hidden-shifts", type=int, nargs="+", default=[6, 7, 8])
    parser.add_argument("--l1-shifts", type=int, nargs="+", default=[6, 7, 8])
    parser.add_argument("--output-shifts", type=int, nargs="+", default=[12, 14, 16, 18, 20])
    parser.add_argument("--tune-samples", type=int, default=DEFAULT_TUNE_SAMPLES)
    parser.add_argument("--seed", type=int, default=DEFAULT_SEED)
    parser.add_argument("--net-id", default="koi-v5")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        raw_path = latest_checkpoint(args.checkpoint)
        raw_size = raw_path.stat().st_size
        hidden = args.hidden if args.hidden else infer_hidden_units(raw_size, args.l1_units)
        if hidden < MIN_HIDDEN or hidden > MAX_HIDDEN or hidden % 2 != 0:
            raise ExportError(f"hidden units {hidden} are out of range")
        params = read_raw_weights(raw_path, hidden, args.l1_units)

        validation = {"scores": np.zeros(0, dtype=np.int32), "buckets": np.zeros(0, dtype=np.int32),
                      "own": [], "opp": []}
        if args.validation is not None and args.validation.is_file():
            validation = load_validation(args.validation, args.tune_samples, args.seed)
        if validation["scores"].size:
            log(f"validation {validation['scores'].size} rows from {args.validation}")
        else:
            log("validation rows unavailable; selecting the first shift combination")

        float_mae = None
        has_validation = validation["scores"].size > 0
        target = validation["scores"].astype(np.int64) if has_validation else None

        best: tuple | None = None
        best_params: dict = {}
        for hidden_shift in args.hidden_shifts:
            feature_params = quantize_feature(params, hidden_shift)
            own = opp = None
            if has_validation:
                own, opp = hidden_activations(feature_params, validation, hidden)
            for l1_shift in args.l1_shifts:
                l1_params = quantize_l1(params, l1_shift)
                for output_shift in args.output_shifts:
                    output_params = quantize_output(params, output_shift)
                    if has_validation:
                        scores = integer_scores(
                            own,
                            opp,
                            l1_params,
                            output_params,
                            validation["buckets"],
                            l1_shift,
                            output_shift,
                        )
                        mae = float(np.mean(np.abs(scores - target)))
                    else:
                        mae = float("inf")
                    log(
                        f"quantization v5 s1={hidden_shift} s_l1={l1_shift} "
                        f"k3={output_shift} val_mae_cp {mae:.2f}"
                    )
                    if best is None or mae < best[0]:
                        best = (mae, hidden_shift, l1_shift, output_shift)
                        best_params = {**feature_params, **l1_params, **output_params}
        assert best is not None
        mae, hidden_shift, l1_shift, output_shift = best
        log(
            f"selected v5 s1={hidden_shift} s_l1={l1_shift} k3={output_shift} "
            f"val_mae_cp {mae:.2f}"
        )
        quantized = {
            **best_params,
            "hidden_shift": hidden_shift,
            "l1_shift": l1_shift,
            "output_shift": output_shift,
        }

        payload = payload_bytes(quantized, hidden, args.l1_units)
        container, payload_digest = container_bytes(
            payload, hidden, args.l1_units, hidden_shift, l1_shift, output_shift
        )
        args.net_out.parent.mkdir(parents=True, exist_ok=True)
        args.net_out.write_bytes(container)
        w1_saturation, l1_saturation, w2_saturation = saturation_fractions(quantized)

        if args.meta_out is not None:
            metadata = {
                "schema": "koi-nnue-training-metadata-v3",
                "created": datetime.datetime.now(datetime.timezone.utc).strftime(
                    "%Y-%m-%dT%H:%M:%SZ"
                ),
                "backend": "bullet",
                "corpus": str(args.validation) if args.validation else None,
                "checkpoint": str(raw_path),
                "rows_used": int(validation["scores"].size),
                "val_rows": int(validation["scores"].size),
                "feature_set": FEATURE_SET.decode(),
                "quantization": QUANTIZATION.decode(),
                "architecture": {
                    "input": INPUT_UNITS,
                    "hidden": hidden,
                    "l1": args.l1_units,
                    "output_buckets": OUTPUT_BUCKETS,
                    "groups": ["halfka-king-bucket-v1", "threat-pairs-v1"],
                },
                "activation": "crelu-pair-l1",
                "hidden_shift": hidden_shift,
                "l1_shift": l1_shift,
                "output_shift": output_shift,
                "seed": args.seed,
                "target_scale": TARGET_SCALE,
                "val_mae_cp": float_mae,
                "val_round_trip_mae_cp": None if mae == float("inf") else mae,
                "w1_saturation": w1_saturation,
                "l1_saturation": l1_saturation,
                "w2_saturation": w2_saturation,
                "payload_sha256": payload_digest,
                "network_sha256": hashlib.sha256(container).hexdigest(),
                "command": " ".join(sys.argv),
            }
            args.meta_out.parent.mkdir(parents=True, exist_ok=True)
            args.meta_out.write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
        log(f"wrote {args.net_out} ({len(container)} bytes) and {args.meta_out}")
        return 0
    except (ExportError, OSError, ValueError) as error:
        print(f"export-bullet-v5: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
