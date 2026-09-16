#!/usr/bin/env python3
"""Train a Stockfish-labelled evaluation network for the KOI-NNUE v2 container.

Reads the ``FEN;cp;bestmove`` corpus produced by gen_training_data.py, encodes
positions with the exact 960-input piece-square-king-pawn-v2 feature set, trains
a sparse first layer with PyTorch (CPU), then quantizes the network so that the
integer forward pass reproduces the C++ NnueWorker semantics bit-for-bit:

    hidden_sum[h] = hidden_bias[h] + sum_f x[f] * feature_weights[f, h]
    hidden_act[h] = clip(hidden_sum[h], 0, 127)
    bottleneck_act[b] = clip(bottleneck_bias[b]
                             + sum_h hidden_act[h] * bottleneck_weights[h, b], 0, 127)
    score = output_bias + sum_b bottleneck_act[b] * output_weights[b]

The exported container is written with tune_eval.nnue_payload/nnue_container so
the C++ loader validates the same magic, version, quantization and SHA-256.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import sys
import time

import numpy as np
import torch
import torch.nn as nn

TOOLS_DIR = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS_DIR))

from tune_eval import (  # noqa: E402
    NNUE_ARCHITECTURE,
    NNUE_FEATURE_SET,
    NNUE_QUANTIZATION,
    nnue_container,
    nnue_payload,
)

INPUT_UNITS, HIDDEN_UNITS, BOTTLENECK_UNITS, _ = NNUE_ARCHITECTURE
PIECE_OFFSET = {"p": 0, "n": 1, "b": 2, "r": 3, "q": 4}


def encode_fen(fen: str) -> list[int]:
    """Return the active input indices of a FEN (no zero entries).

    Features are side-to-move relative, matching the C++ encoder: for black to
    move every square is mirrored vertically (square ^ 56) and the colours are
    swapped so the mover always looks like white.
    """
    fields = fen.split()
    black_to_move = len(fields) > 1 and fields[1] == "b"

    def perspective_square(square: int) -> int:
        return square ^ 56 if black_to_move else square

    def perspective_color(white: bool) -> int:
        mover_is_white = not black_to_move
        return 0 if white == mover_is_white else 1

    active: list[int] = []
    pawns: list[list[int]] = [[], []]
    kings: list[int | None] = [None, None]
    rank = 7
    file = 0
    for character in fields[0]:
        if character == "/":
            rank -= 1
            file = 0
        elif character.isdigit():
            file += int(character)
        else:
            square = rank * 8 + file
            color = perspective_color(character.isupper())
            piece = character.lower()
            if piece == "k":
                kings[color] = perspective_square(square)
            else:
                active.append((color * 6 + PIECE_OFFSET[piece]) * 64 +
                              perspective_square(square))
                if piece == "p":
                    pawns[color].append(perspective_square(square))
            file += 1

    for color in range(2):
        if kings[color] is not None:
            active.append(768 + color * 64 + kings[color])

    for color in range(2):
        enemy_pawns = pawns[1 - color]
        for pawn_file in range(8):
            file_pawns = [s for s in pawns[color] if s % 8 == pawn_file]
            if not file_pawns:
                continue
            base = 896 + color * 32 + pawn_file * 4
            active.append(base)
            if len(file_pawns) >= 2:
                active.append(base + 1)
            isolated = True
            for adjacent_file in (pawn_file - 1, pawn_file + 1):
                if 0 <= adjacent_file < 8 and any(
                        s % 8 == adjacent_file for s in pawns[color]):
                    isolated = False
                    break
            if isolated:
                active.append(base + 2)
            passed = False
            for square in file_pawns:
                square_rank = square // 8
                blocked = False
                for enemy_square in enemy_pawns:
                    if enemy_square % 8 not in (pawn_file - 1, pawn_file, pawn_file + 1):
                        continue
                    if enemy_square // 8 > square_rank:
                        blocked = True
                        break
                if not blocked:
                    passed = True
                    break
            if passed:
                active.append(base + 3)
    return active


class KoiNet(nn.Module):
    def __init__(self) -> None:
        super().__init__()
        self.feature = nn.EmbeddingBag(INPUT_UNITS, HIDDEN_UNITS, mode="sum")
        nn.init.uniform_(self.feature.weight, -0.1, 0.1)
        self.hidden_bias = nn.Parameter(torch.zeros(HIDDEN_UNITS))
        self.bottleneck = nn.Linear(HIDDEN_UNITS, BOTTLENECK_UNITS)
        self.output = nn.Linear(BOTTLENECK_UNITS, 1)

    def forward(self, indices: torch.Tensor, weights: torch.Tensor) -> torch.Tensor:
        hidden = torch.clamp(self.feature(indices, per_sample_weights=weights)
                             + self.hidden_bias, 0.0, 1.0)
        bottleneck = torch.clamp(self.bottleneck(hidden), 0.0, 1.0)
        return self.output(bottleneck).squeeze(1)


def load_corpus(path: pathlib.Path, limit: int | None) -> tuple[list[str], np.ndarray]:
    fens: list[str] = []
    scores: list[int] = []
    with path.open("r", encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split(";")
            if len(parts) < 2:
                continue
            try:
                score = int(parts[1])
            except ValueError:
                continue
            if abs(score) > 4000:
                continue
            fens.append(parts[0])
            scores.append(score)
            if limit is not None and len(fens) >= limit:
                break
    return fens, np.asarray(scores, dtype=np.float32)


def encode_all(fens: list[str]) -> tuple[np.ndarray, np.ndarray]:
    offsets = np.zeros(len(fens) + 1, dtype=np.int64)
    chunks: list[np.ndarray] = []
    total = 0
    for index, fen in enumerate(fens):
        active = np.asarray(encode_fen(fen), dtype=np.int32)
        chunks.append(active)
        total += active.size
        offsets[index + 1] = total
    return np.concatenate(chunks) if chunks else np.zeros(0, dtype=np.int32), offsets


def batches(indices: np.ndarray, offsets: np.ndarray, scores: np.ndarray,
            order: np.ndarray, batch_size: int, pad_length: int):
    for start in range(0, order.size, batch_size):
        rows = order[start:start + batch_size]
        lengths = offsets[rows + 1] - offsets[rows]
        flat = np.zeros((rows.size, pad_length), dtype=np.int64)
        weights = np.zeros((rows.size, pad_length), dtype=np.float32)
        for row, sample in enumerate(rows):
            begin = offsets[sample]
            end = offsets[sample + 1]
            count = lengths[row]
            flat[row, :count] = indices[begin:end]
            weights[row, :count] = 1.0
        yield (torch.from_numpy(flat), torch.from_numpy(weights),
               torch.from_numpy(scores[rows]))


def quantize(model: KoiNet, scale_hidden: int, scale_bottleneck: int, score_scale: float):
    feature = model.feature.weight.detach().numpy()
    hidden_bias = model.hidden_bias.detach().numpy()
    bottleneck = model.bottleneck.weight.detach().numpy().T  # (256, 32)
    bottleneck_bias = model.bottleneck.bias.detach().numpy()
    output = model.output.weight.detach().numpy()[0]  # (32,)
    output_bias = float(model.output.bias.detach().numpy()[0])

    feature_q = np.clip(np.rint(feature * scale_hidden), -32768, 32767).astype(np.int64)
    hidden_bias_q = np.rint(hidden_bias * scale_hidden).astype(np.int64)
    bottleneck_q = np.clip(np.rint(bottleneck * scale_bottleneck / max(scale_hidden, 1)),
                           -128, 127).astype(np.int64)
    bottleneck_bias_q = np.rint(bottleneck_bias * scale_bottleneck).astype(np.int64)
    output_q = np.clip(np.rint(output * score_scale / max(scale_bottleneck, 1)),
                       -128, 127).astype(np.int64)
    output_bias_q = int(round(output_bias * score_scale))
    return {
        "feature_weights": feature_q.reshape(-1),
        "hidden_bias": hidden_bias_q,
        "bottleneck_weights": bottleneck_q.reshape(-1),
        "bottleneck_bias": bottleneck_bias_q,
        "output_weights": output_q,
        "output_bias": output_bias_q,
    }


def integer_scores(params: dict, indices: np.ndarray, offsets: np.ndarray,
                   samples: np.ndarray | None = None) -> np.ndarray:
    """Integer forward pass for the selected samples (defaults to all)."""
    if samples is None:
        samples = np.arange(len(offsets) - 1)
    weights = params["feature_weights"].reshape(INPUT_UNITS, HIDDEN_UNITS)
    count = samples.size
    one_hot = np.zeros((count, INPUT_UNITS), dtype=np.int64)
    for position, sample in enumerate(samples):
        one_hot[position, indices[offsets[sample]:offsets[sample + 1]]] = 1
    hidden = one_hot @ weights.astype(np.int64) + params["hidden_bias"]
    np.clip(hidden, 0, 127, out=hidden)
    bottleneck = hidden @ params["bottleneck_weights"].reshape(
        HIDDEN_UNITS, BOTTLENECK_UNITS).astype(np.int64) + params["bottleneck_bias"]
    np.clip(bottleneck, 0, 127, out=bottleneck)
    return bottleneck @ params["output_weights"].astype(np.int64) + params["output_bias"]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--corpus", type=pathlib.Path,
                        default=pathlib.Path("artifacts/training/labels.txt"))
    parser.add_argument("--net-out", type=pathlib.Path,
                        default=pathlib.Path("artifacts/training/koi-sf-v1.nnue"))
    parser.add_argument("--meta-out", type=pathlib.Path,
                        default=pathlib.Path("artifacts/training/koi-sf-v1.metadata.json"))
    parser.add_argument("--rows", type=int, default=0, help="0 = all available rows")
    parser.add_argument("--epochs", type=int, default=4)
    parser.add_argument("--batch-size", type=int, default=4096)
    parser.add_argument("--learning-rate", type=float, default=1e-3)
    parser.add_argument("--val-fraction", type=float, default=0.05)
    parser.add_argument("--seed", type=int, default=20260916)
    parser.add_argument("--threads", type=int, default=4)
    parser.add_argument("--hidden-scale", type=int, default=0, help="0 = auto grid search")
    parser.add_argument("--bottleneck-scale", type=int, default=0)
    # The integer model clips activations at 127, so the quantization scale that
    # reproduces the float model's [0, 1] clamp is 127; neighbouring scales are
    # grid-searched because weight rounding shifts the optimum slightly.
    parser.add_argument("--float-out", type=pathlib.Path, default=None,
                        help="optional path for a float checkpoint after training")
    parser.add_argument("--float-in", type=pathlib.Path, default=None,
                        help="load a float checkpoint and skip training (quantize only)")
    args = parser.parse_args()

    torch.set_num_threads(args.threads)
    rng = np.random.default_rng(args.seed)
    if not args.corpus.exists():
        print(f"corpus not found: {args.corpus}", file=sys.stderr)
        return 1

    started = time.time()
    fens, scores = load_corpus(args.corpus, args.rows if args.rows > 0 else None)
    if not fens:
        print("corpus has no usable rows", file=sys.stderr)
        return 1
    print(f"loaded {len(fens)} rows in {time.time() - started:.1f}s", flush=True)

    indices, offsets = encode_all(fens)
    pad_length = int(offsets[1:].max() - 0) if len(fens) else 0
    pad_length = int((offsets[1:] - offsets[:-1]).max())
    print(f"encoded features: mean {indices.size / len(fens):.1f} active, "
          f"pad {pad_length}", flush=True)

    order = rng.permutation(len(fens))
    val_count = max(1, int(len(fens) * args.val_fraction))
    val_order = order[:val_count]
    train_order = order[val_count:]

    model = KoiNet()
    optimizer = torch.optim.Adam(model.parameters(), lr=args.learning_rate)
    loss_fn = nn.SmoothL1Loss()
    target_scale = 100.0  # labels are centipawns; model outputs pawns
    train_scores = scores / target_scale
    if args.float_in is not None:
        model.load_state_dict(torch.load(args.float_in, map_location="cpu"))
        print(f"loaded float checkpoint {args.float_in}; skipping training", flush=True)

    for epoch in range(args.epochs if args.float_in is None else 0):
        model.train()
        epoch_started = time.time()
        total = 0.0
        seen = 0
        permutation = rng.permutation(train_order)
        for flat, weights, batch_scores in batches(indices, offsets, train_scores,
                                                   permutation, args.batch_size, pad_length):
            optimizer.zero_grad(set_to_none=True)
            prediction = model(flat, weights)
            loss = loss_fn(prediction, batch_scores)
            loss.backward()
            optimizer.step()
            total += float(loss.detach()) * flat.shape[0]
            seen += flat.shape[0]
        model.eval()
        with torch.no_grad():
            val_loss = 0.0
            val_seen = 0
            val_prediction = np.empty(val_order.size, dtype=np.float64)
            position = 0
            for flat, weights, batch_scores in batches(indices, offsets, train_scores,
                                                       val_order, args.batch_size, pad_length):
                prediction = model(flat, weights)
                val_loss += float(loss_fn(prediction, batch_scores)) * flat.shape[0]
                val_seen += flat.shape[0]
                val_prediction[position:position + flat.shape[0]] = prediction.numpy()
                position += flat.shape[0]
        # Centipawn error against the Stockfish labels (side-to-move view).
        val_cp = val_prediction * target_scale
        val_mae = float(np.mean(np.abs(val_cp - scores[val_order])))
        print(f"epoch {epoch + 1}/{args.epochs} train_loss {total / max(seen, 1):.5f} "
              f"val_loss {val_loss / max(val_seen, 1):.5f} val_mae_cp {val_mae:.1f} "
              f"time {time.time() - epoch_started:.1f}s", flush=True)

    if args.float_out is not None:
        args.float_out.parent.mkdir(parents=True, exist_ok=True)
        torch.save(model.state_dict(), args.float_out)
        print(f"wrote float checkpoint {args.float_out}", flush=True)

    # Quantization: grid search the two activation scales by integer validation MAE.
    hidden_candidates = [args.hidden_scale] if args.hidden_scale else [96, 112, 127, 144]
    bottleneck_candidates = [args.bottleneck_scale] if args.bottleneck_scale else [96, 112, 127, 144]
    tune_samples = val_order if val_order.size <= 4000 else \
        rng.choice(val_order, size=4000, replace=False)
    best = None
    for hidden_scale in hidden_candidates:
        for bottleneck_scale in bottleneck_candidates:
            params = quantize(model, hidden_scale, bottleneck_scale, target_scale)
            candidate = integer_scores(params, indices, offsets, tune_samples)
            mae = float(np.mean(np.abs(candidate - scores[tune_samples])))
            print(f"quantization hidden={hidden_scale} bottleneck={bottleneck_scale} "
                  f"val_mae_cp {mae:.1f}", flush=True)
            if best is None or mae < best[0]:
                best = (mae, hidden_scale, bottleneck_scale, params)
    assert best is not None
    mae, hidden_scale, bottleneck_scale, params = best
    print(f"selected hidden={hidden_scale} bottleneck={bottleneck_scale} "
          f"val_mae_cp {mae:.1f}", flush=True)

    payload = nnue_payload(
        params["feature_weights"].tolist(),
        params["hidden_bias"].tolist(),
        params["bottleneck_weights"].tolist(),
        params["bottleneck_bias"].tolist(),
        params["output_weights"].tolist(),
        params["output_bias"],
    )
    container, payload_hash = nnue_container(payload)
    args.net_out.parent.mkdir(parents=True, exist_ok=True)
    args.net_out.write_bytes(container)
    metadata = {
        "schema": "koi-nnue-training-metadata-v1",
        "created": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "corpus": str(args.corpus),
        "rows_used": int(len(fens)),
        "val_rows": int(val_order.size),
        "feature_set": NNUE_FEATURE_SET.decode(),
        "quantization": NNUE_QUANTIZATION.decode(),
        "architecture": list(NNUE_ARCHITECTURE),
        "epochs": args.epochs,
        "batch_size": args.batch_size,
        "learning_rate": args.learning_rate,
        "seed": args.seed,
        "target_scale": target_scale,
        "hidden_scale": hidden_scale,
        "bottleneck_scale": bottleneck_scale,
        "val_mae_cp": mae,
        "payload_sha256": payload_hash,
        "network_sha256": __import__("hashlib").sha256(container).hexdigest(),
        "command": " ".join(sys.argv),
    }
    args.meta_out.parent.mkdir(parents=True, exist_ok=True)
    args.meta_out.write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {args.net_out} ({len(container)} bytes) and {args.meta_out}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
