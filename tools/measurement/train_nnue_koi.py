#!/usr/bin/env python3
"""Train a KOI-NNUE network (v4 ``halfka-king-bucket-v1`` or v5 dual-perspective
``halfka-king-bucket-v1+threat-pairs-v1``).

Reads either a ``koi-dataset-v1``/``koi-dataset-v2`` binary dataset (sparse
indices, produced by ``koi_dataset.py``) or the ``FEN;cp;best_move`` text
corpus, trains a feature-transformer network on CPU with PyTorch, then
quantizes it so that the integer forward pass reproduces the C++ ``NnueWorker``
semantics:

v4 (``--arch v4``):

    hidden[h] = clamp(clip(hidden_bias[h] + sum_f x[f] * feature_weights[f, h],
                           -2^31, 2^31 - 1), 0, 127)
    pairs[j]  = hidden[j] * hidden[j + hidden/2]
    score     = (output_bias[b] + sum_j output_weights[b, j] * pairs[j]) >> k3

v5 (``--arch v5``, the default):

    own/opp[h] = clamp(clip(hidden_bias[h] + sum_f x[h] * feature_weights[f, h],
                            -2^31, 2^31 - 1), 0, 127)
    pairs[j]   = own[j] * opp[j]                       # j < hidden
    l1[k]      = clamp((sum_j l1_weights[k, j] * pairs[j] + l1_bias[k]) >> s_l1,
                       0, 127)
    score      = (output_bias[b] + sum_k output_weights[b, k] * l1[k]) >> k3

with ``b = min(7, (32 - pieces) / 4)``.  The exported v4 container is version 4
(``hidden_shift``/``output_shift`` bytes, metadata ``koi-nnue-training-metadata-v2``);
the v5 container is version 5 (``hidden_shift``/``output_shift``/``l1_shift``
bytes, metadata ``koi-nnue-training-metadata-v3``).

The trainer prints the studio progress contract lines:

    epoch <i>/<n> train_loss <f> val_loss <f> val_mae_cp <f> time <f>s
    quantization v4 s1=<s> k3=<k> val_mae_cp <f>
    selected v4 s1=<s> k3=<k> val_mae_cp <f>
    wrote <net> (<bytes> bytes) and <metadata>
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import pathlib
import struct
import sys
import time

import numpy as np

try:
    import torch
    import torch.nn as nn
except ImportError:  # pragma: no cover - exercised only without PyTorch
    torch = None
    nn = None

TOOLS_DIR = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS_DIR))

try:
    import koi_dataset
except ImportError:  # pragma: no cover - reported when the text path is used
    koi_dataset = None

MAGIC = b"KOI-NNUE"
VERSION = 4
V5_VERSION = 5
INPUT_UNITS = 12 * 12 * 64
V5_INPUT_UNITS = 36864
OUTPUT_BUCKETS = 8
QUANTIZATION = b"int16/int8"
FEATURE_SET = b"halfka-king-bucket-v1"
THREAT_FEATURE_SET = b"threat-pairs-v1"
V5_FEATURE_SET = b"halfka-king-bucket-v1+threat-pairs-v1"
V5_L1_UNITS = 32
INT32_MIN = -(2**31)
INT32_MAX = 2**31 - 1
W1_LIMIT = 32767
W2_LIMIT = 127
L1_LIMIT = 127
TARGET_SCALE = 100.0


class TrainerError(Exception):
    """A user-facing trainer error."""


def log(message: str) -> None:
    print(message, flush=True)


# ---------------------------------------------------------------------------
# Dataset loading
# ---------------------------------------------------------------------------


def _require_dataset_module():
    if koi_dataset is None:
        raise TrainerError(
            "the text corpus path needs koi_dataset.py with the in-tree koi_chess "
            "package; pass --dataset to use a binary dataset"
        )
    return koi_dataset


def load_text_corpus(path: pathlib.Path, limit: int) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """Encode a ``FEN;cp;best_move`` corpus with the shared Python encoder."""
    module = _require_dataset_module()
    if not path.exists():
        raise TrainerError(f"corpus not found: {path}")
    chunks: list[list[int]] = []
    scores: list[int] = []
    with path.open("r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            parts = line.strip().split(";")
            if len(parts) < 2:
                continue
            try:
                cp = int(parts[1])
            except ValueError:
                continue
            if abs(cp) > module.MAX_ABS_CP:
                continue
            try:
                import koi_chess as chess

                board = chess.Board(parts[0])
            except ValueError:
                continue
            chunks.append(module.halfka_king_bucket_indices(board))
            scores.append(cp)
            if limit and len(scores) >= limit:
                break
    return _pack_rows(chunks, scores)


def load_binary_dataset(path: pathlib.Path, limit: int) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """Read a ``koi-dataset-v1`` dataset into flat indices/offsets/scores."""
    if not path.exists():
        raise TrainerError(f"dataset not found: {path}")
    data = path.read_bytes()
    if len(data) < 8 + 4 + 2:
        raise TrainerError(f"{path} is too small to be a koi-dataset-v1 file")
    magic, version, feature_length = struct.unpack_from("<8sIH", data, 0)
    if magic != b"KOI-DATA":
        raise TrainerError(f"{path} has magic {magic!r}; expected b'KOI-DATA'")
    if version != 1:
        raise TrainerError(f"{path} has dataset version {version}; expected 1")
    feature_set = data[14 : 14 + feature_length].decode("utf-8", errors="replace")
    if feature_set != FEATURE_SET.decode():
        raise TrainerError(f"{path} uses feature set {feature_set!r}; expected {FEATURE_SET.decode()!r}")
    header_end = 14 + feature_length + 8
    if header_end > len(data):
        raise TrainerError(f"{path} is truncated before the record count")
    count = struct.unpack_from("<Q", data, 14 + feature_length)[0]
    if count > len(data):
        raise TrainerError(f"{path} claims {count} records but is only {len(data)} bytes")
    indices = np.empty(count * 32, dtype=np.int32)
    offsets = np.zeros(count + 1, dtype=np.int64)
    scores = np.zeros(count, dtype=np.int32)
    written = 0
    position = 14 + feature_length + 8
    for row in range(count):
        if position + 2 > len(data):
            raise TrainerError(f"{path} ends early at record {row}")
        features = struct.unpack_from("<H", data, position)[0]
        position += 2
        if features > 32:
            raise TrainerError(f"{path} record {row} has {features} features; maximum is 32")
        if position + features * 2 + 4 > len(data):
            raise TrainerError(f"{path} record {row} is truncated")
        values = struct.unpack_from(f"<{features}H", data, position)
        position += features * 2
        scores[row] = struct.unpack_from("<i", data, position)[0]
        position += 4
        indices[written : written + features] = values
        written += features
        offsets[row + 1] = written
        if limit and row + 1 >= limit:
            count = row + 1
            offsets = offsets[: count + 1]
            scores = scores[:count]
            break
    if not limit and position != len(data):
        raise TrainerError(f"{path} has {len(data) - position} trailing bytes")
    return indices[:written], offsets, scores


def load_binary_dataset_v2(
    path: pathlib.Path, limit: int
) -> tuple[np.ndarray, np.ndarray, np.ndarray, np.ndarray, np.ndarray, np.ndarray]:
    """Read a ``koi-dataset-v2`` dataset into own/opp indices, offsets, scores, buckets."""
    if not path.exists():
        raise TrainerError(f"dataset not found: {path}")
    data = path.read_bytes()
    if len(data) < 8 + 4 + 2:
        raise TrainerError(f"{path} is too small to be a koi-dataset-v2 file")
    magic, version, group_count = struct.unpack_from("<8sIH", data, 0)
    if magic != b"KOI-DATA":
        raise TrainerError(f"{path} has magic {magic!r}; expected b'KOI-DATA'")
    if version != 2:
        raise TrainerError(f"{path} has dataset version {version}; expected 2")
    position = 14
    groups: list[str] = []
    for _ in range(group_count):
        if position + 2 > len(data):
            raise TrainerError(f"{path} is truncated in the group table")
        length = struct.unpack_from("<H", data, position)[0]
        position += 2
        name = data[position : position + length].decode("utf-8", errors="replace")
        position += length
        groups.append(name)
    expected_groups = [FEATURE_SET.decode(), THREAT_FEATURE_SET.decode()]
    if group_count != 2 or groups != expected_groups:
        raise TrainerError(
            f"{path} has groups {groups!r}; expected {expected_groups!r}"
        )
    if position + 8 > len(data):
        raise TrainerError(f"{path} is truncated before the record count")
    count = struct.unpack_from("<Q", data, position)[0]
    position += 8
    if count > len(data):
        raise TrainerError(f"{path} claims {count} records but is only {len(data)} bytes")
    capacity = 160  # group A (<= 32) + group B (<= 128) per side
    own_indices = np.empty(count * capacity, dtype=np.int32)
    own_offsets = np.zeros(count + 1, dtype=np.int64)
    opp_indices = np.empty(count * capacity, dtype=np.int32)
    opp_offsets = np.zeros(count + 1, dtype=np.int64)
    scores = np.zeros(count, dtype=np.int32)
    buckets = np.zeros(count, dtype=np.int64)
    own_written = 0
    opp_written = 0
    loaded = 0
    for row in range(count):
        if position + 8 > len(data):
            raise TrainerError(f"{path} ends early at record {row}")
        counts = struct.unpack_from("<4H", data, position)
        position += 8
        total_own = counts[0] + counts[1]
        total_opp = counts[2] + counts[3]
        if total_own > capacity or total_opp > capacity:
            raise TrainerError(f"{path} record {row} has too many indices")
        values = struct.unpack_from(f"<{total_own + total_opp}H", data, position)
        position += (total_own + total_opp) * 2
        if position + 4 > len(data):
            raise TrainerError(f"{path} record {row} is truncated")
        scores[row] = struct.unpack_from("<i", data, position)[0]
        position += 4
        own_indices[own_written : own_written + total_own] = values[:total_own]
        own_written += total_own
        own_offsets[row + 1] = own_written
        opp_indices[opp_written : opp_written + total_opp] = values[total_own:]
        opp_written += total_opp
        opp_offsets[row + 1] = opp_written
        # Group A holds exactly one feature per piece, so its count is the
        # piece count used by the piece-count output buckets.  Deriving the
        # bucket from the combined A+B feature count (the old behavior)
        # selected the wrong head whenever threat features were present.
        buckets[row] = min(7, (32 - min(counts[0], 32)) // 4)
        loaded = row + 1
        if limit and loaded >= limit:
            break
    own_offsets = own_offsets[: loaded + 1]
    opp_offsets = opp_offsets[: loaded + 1]
    scores = scores[:loaded]
    buckets = buckets[:loaded]
    if not limit and position != len(data):
        raise TrainerError(f"{path} has {len(data) - position} trailing bytes")
    return (own_indices[:own_written], own_offsets,
            opp_indices[:opp_written], opp_offsets, scores, buckets)


def load_text_corpus_v5(
    path: pathlib.Path, limit: int
) -> tuple[np.ndarray, np.ndarray, np.ndarray, np.ndarray, np.ndarray, np.ndarray]:
    """Read a ``FEN;cp`` corpus into own/opp feature packs and buckets for v5."""
    module = _require_dataset_module()
    if not path.exists():
        raise TrainerError(f"corpus not found: {path}")
    own_rows: list[list[int]] = []
    opp_rows: list[list[int]] = []
    scores: list[int] = []
    buckets: list[int] = []
    with path.open("r", encoding="utf-8", errors="replace") as handle:
        for raw in handle:
            raw = raw.strip()
            if not raw or raw.startswith("#"):
                continue
            parsed = module.parse_row(raw.encode("utf-8", errors="replace"))
            if parsed is None:
                continue
            board, score = parsed
            if abs(int(score)) > module.MAX_ABS_CP:
                continue
            a_own, b_own, a_opp, b_opp = module.encode_record(board)
            own_rows.append([int(value) for value in list(a_own) + list(b_own)])
            opp_rows.append([int(value) for value in list(a_opp) + list(b_opp)])
            scores.append(int(score))
            buckets.append(int(module.output_bucket(board)))
            if limit and len(scores) >= limit:
                break
    if not scores:
        raise TrainerError(f"corpus {path} produced no usable rows")
    own_flat, own_offsets, _ = _pack_rows(own_rows, scores)
    opp_flat, opp_offsets, packed_scores = _pack_rows(opp_rows, scores)
    return (own_flat, own_offsets, opp_flat, opp_offsets, packed_scores,
            np.asarray(buckets, dtype=np.int64))


def _pack_rows(rows: list[list[int]], scores: list[int]) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    offsets = np.zeros(len(rows) + 1, dtype=np.int64)
    total = 0
    for index, active in enumerate(rows):
        total += len(active)
        offsets[index + 1] = total
    flat = np.asarray([value for row in rows for value in row], dtype=np.int32)
    return flat, offsets, np.asarray(scores, dtype=np.int32)


def piece_count_buckets(offsets: np.ndarray) -> np.ndarray:
    counts = offsets[1:] - offsets[:-1]
    return np.minimum(7, (32 - counts) // 4).astype(np.int64)


# ---------------------------------------------------------------------------
# Model
# ---------------------------------------------------------------------------


if nn is not None:

    class KoiNet(nn.Module):
        """EmbeddingBag feature layer plus CReLU pair products and bucket heads."""

        def __init__(self, hidden_units: int) -> None:
            super().__init__()
            if hidden_units < 32 or hidden_units % 2 != 0:
                raise TrainerError("hidden units must be even and at least 32")
            self.hidden_units = hidden_units
            self.half = hidden_units // 2
            self.feature = nn.EmbeddingBag(INPUT_UNITS, hidden_units, mode="sum")
            nn.init.uniform_(self.feature.weight, -0.1, 0.1)
            self.hidden_bias = nn.Parameter(torch.zeros(hidden_units))
            self.output_weight = nn.Parameter(torch.empty(OUTPUT_BUCKETS, self.half))
            nn.init.uniform_(self.output_weight, -0.05, 0.05)
            self.output_bias = nn.Parameter(torch.zeros(OUTPUT_BUCKETS))

        def forward(self, flat: torch.Tensor, weights: torch.Tensor,
                    buckets: torch.Tensor) -> torch.Tensor:
            hidden = torch.clamp(
                self.feature(flat, per_sample_weights=weights) + self.hidden_bias,
                0.0,
                1.0,
            )
            pairs = hidden[:, : self.half] * hidden[:, self.half :]
            weight = self.output_weight[buckets]
            bias = self.output_bias[buckets]
            return (pairs * weight).sum(dim=1) + bias

    class KoiNetV5(nn.Module):
        """Shared feature layer for both perspectives, full-width cross pairs, L1 head."""

        def __init__(self, hidden_units: int, l1_units: int = V5_L1_UNITS) -> None:
            super().__init__()
            if hidden_units < 32 or hidden_units % 2 != 0:
                raise TrainerError("hidden units must be even and at least 32")
            if l1_units < 8 or l1_units > 128:
                raise TrainerError("L1 units must be between 8 and 128")
            self.hidden_units = hidden_units
            self.l1_units = l1_units
            self.feature = nn.EmbeddingBag(V5_INPUT_UNITS, hidden_units, mode="sum")
            nn.init.uniform_(self.feature.weight, -0.1, 0.1)
            self.hidden_bias = nn.Parameter(torch.zeros(hidden_units))
            self.l1_weight = nn.Parameter(torch.empty(l1_units, hidden_units))
            # The pair products are small (roughly 0.05 rms), so the default
            # +-0.05 init leaves the L1 pre-activations near zero and the head
            # cannot move the score.  Scale the init so a freshly built head
            # already produces unit-variance activations.
            nn.init.uniform_(self.l1_weight, -34.0 / math.sqrt(hidden_units),
                             34.0 / math.sqrt(hidden_units))
            self.l1_bias = nn.Parameter(torch.zeros(l1_units))
            self.output_weight = nn.Parameter(torch.empty(OUTPUT_BUCKETS, l1_units))
            # Predictions start within a couple of target units instead of at
            # zero, which is what the integer export can actually represent.
            nn.init.uniform_(self.output_weight, -0.9, 0.9)
            self.output_bias = nn.Parameter(torch.zeros(OUTPUT_BUCKETS))

        def forward(self, own_flat, own_weights, opp_flat, opp_weights, buckets):
            # The exported integer net clamps activations to 0..127 after
            # shifting, so the training forward keeps the same [0, 1] range.
            own = torch.clamp(
                self.feature(own_flat, per_sample_weights=own_weights) + self.hidden_bias,
                0.0, 1.0)
            opp = torch.clamp(
                self.feature(opp_flat, per_sample_weights=opp_weights) + self.hidden_bias,
                0.0, 1.0)
            pairs = own * opp
            l1 = torch.clamp(pairs @ self.l1_weight.T + self.l1_bias, 0.0, 1.0)
            return (l1 * self.output_weight[buckets]).sum(dim=1) + self.output_bias[buckets]

else:  # pragma: no cover - only without PyTorch

    class KoiNet:  # type: ignore[no-redef]
        def __init__(self, hidden_units: int) -> None:
            raise TrainerError("PyTorch is required for training; install torch")

    class KoiNetV5:  # type: ignore[no-redef]
        def __init__(self, *args, **kwargs) -> None:  # noqa: ARG002
            raise TrainerError("PyTorch is required for training; install torch")


def batches(indices: np.ndarray, offsets: np.ndarray, buckets: np.ndarray,
            targets: np.ndarray, order: np.ndarray, batch_size: int, pad_length: int):
    for start in range(0, order.size, batch_size):
        rows = order[start : start + batch_size]
        lengths = (offsets[rows + 1] - offsets[rows]).astype(np.int64)
        flat = np.zeros((rows.size, pad_length), dtype=np.int64)
        weights = np.zeros((rows.size, pad_length), dtype=np.float32)
        for row, sample in enumerate(rows):
            begin = offsets[sample]
            end = offsets[sample + 1]
            flat[row, : lengths[row]] = indices[begin:end]
            weights[row, : lengths[row]] = 1.0
        yield (
            torch.from_numpy(flat),
            torch.from_numpy(weights),
            torch.from_numpy(buckets[rows]),
            torch.from_numpy(targets[rows].astype(np.float32)),
        )


def validate(model: "KoiNet", indices: np.ndarray, offsets: np.ndarray,
             buckets: np.ndarray, scores: np.ndarray, order: np.ndarray,
             batch_size: int, pad_length: int) -> np.ndarray:
    model.eval()
    prediction = np.empty(order.size, dtype=np.float64)
    position = 0
    with torch.no_grad():
        for flat, weights, batch_buckets, _ in batches(
            indices, offsets, buckets, scores, order, batch_size, pad_length
        ):
            value = model(flat, weights, batch_buckets)
            prediction[position : position + flat.shape[0]] = value.numpy()
            position += flat.shape[0]
    return prediction * TARGET_SCALE


def batches_dual(own_indices, own_offsets, opp_indices, opp_offsets, buckets, targets,
                 order, batch_size, pad_length):
    for start in range(0, order.size, batch_size):
        rows = order[start : start + batch_size]
        own_flat = np.zeros((rows.size, pad_length), dtype=np.int64)
        own_weights = np.zeros((rows.size, pad_length), dtype=np.float32)
        opp_flat = np.zeros((rows.size, pad_length), dtype=np.int64)
        opp_weights = np.zeros((rows.size, pad_length), dtype=np.float32)
        for row, sample in enumerate(rows):
            own_begin, own_end = own_offsets[sample], own_offsets[sample + 1]
            opp_begin, opp_end = opp_offsets[sample], opp_offsets[sample + 1]
            own_flat[row, : own_end - own_begin] = own_indices[own_begin:own_end]
            own_weights[row, : own_end - own_begin] = 1.0
            opp_flat[row, : opp_end - opp_begin] = opp_indices[opp_begin:opp_end]
            opp_weights[row, : opp_end - opp_begin] = 1.0
        yield (torch.from_numpy(own_flat), torch.from_numpy(own_weights),
               torch.from_numpy(opp_flat), torch.from_numpy(opp_weights),
               torch.from_numpy(buckets[rows]),
               torch.from_numpy(targets[rows].astype(np.float32)))


def validate_v5(model, own_indices, own_offsets, opp_indices, opp_offsets, buckets,
                scores, order, batch_size, pad_length) -> np.ndarray:
    model.eval()
    prediction = np.empty(order.size, dtype=np.float64)
    position = 0
    with torch.no_grad():
        for own_flat, own_weights, opp_flat, opp_weights, batch_buckets, _ in batches_dual(
            own_indices, own_offsets, opp_indices, opp_offsets, buckets, scores,
            order, batch_size, pad_length
        ):
            value = model(own_flat, own_weights, opp_flat, opp_weights, batch_buckets)
            prediction[position : position + own_flat.shape[0]] = value.numpy()
            position += own_flat.shape[0]
    return prediction * TARGET_SCALE


# ---------------------------------------------------------------------------
# Quantization and the integer reference
# ---------------------------------------------------------------------------


def quantize_v4(model: "KoiNet", hidden_shift: int, output_shift: int) -> dict:
    """Fixed-point quantization with explicit shifts (container version 4)."""
    scale = float(2**hidden_shift)
    feature = model.feature.weight.detach().numpy().astype(np.float64)
    hidden_bias = model.hidden_bias.detach().numpy().astype(np.float64)
    output_weight = model.output_weight.detach().numpy().astype(np.float64)
    output_bias = model.output_bias.detach().numpy().astype(np.float64)

    feature_q = np.clip(np.rint(feature * scale), -W1_LIMIT, W1_LIMIT).astype(np.int64)
    hidden_bias_q = np.rint(hidden_bias * scale).astype(np.int64)
    output_q = np.clip(
        np.rint(output_weight * TARGET_SCALE * (2.0**output_shift) / (scale * scale)),
        -W2_LIMIT,
        W2_LIMIT,
    ).astype(np.int64)
    output_bias_q = np.rint(output_bias * TARGET_SCALE * (2.0**output_shift)).astype(np.int64)
    return {
        "feature_weights": feature_q,
        "hidden_bias": hidden_bias_q,
        "output_weights": output_q,
        "output_bias": output_bias_q,
        "hidden_shift": hidden_shift,
        "output_shift": output_shift,
    }


def quantize_v5(model, hidden_shift: int, l1_shift: int) -> dict:
    scale = float(2**hidden_shift)
    l1_scale = float(2**l1_shift)
    feature = model.feature.weight.detach().numpy().astype(np.float64)
    hidden_bias = model.hidden_bias.detach().numpy().astype(np.float64)
    l1_weight = model.l1_weight.detach().numpy().astype(np.float64)
    l1_bias = model.l1_bias.detach().numpy().astype(np.float64)
    feature_q = np.clip(np.rint(feature * scale), -W1_LIMIT, W1_LIMIT).astype(np.int64)
    hidden_bias_q = np.rint(hidden_bias * scale).astype(np.int64)
    l1_weight_q = np.clip(np.rint(l1_weight * l1_scale / 127.0), -L1_LIMIT,
                          L1_LIMIT).astype(np.int64)
    l1_bias_q = np.rint(l1_bias * 127.0 * l1_scale).astype(np.int64)
    return {"feature_weights": feature_q, "hidden_bias": hidden_bias_q,
            "l1_weights": l1_weight_q, "l1_bias": l1_bias_q,
            "hidden_shift": hidden_shift, "l1_shift": l1_shift}


def hidden_activations_dual(params, own_indices, own_offsets, opp_indices, opp_offsets,
                           samples, hidden_units):
    weight = params["feature_weights"].reshape(V5_INPUT_UNITS, hidden_units)
    bias = params["hidden_bias"].astype(np.int64)
    results = []
    for indices, offsets in ((own_indices, own_offsets), (opp_indices, opp_offsets)):
        hidden = np.empty((samples.size, hidden_units), dtype=np.int64)
        for row, sample in enumerate(samples):
            begin, end = offsets[sample], offsets[sample + 1]
            value = bias.copy()
            if end > begin:
                value = value + weight[indices[begin:end], :].sum(axis=0)
            hidden[row] = np.clip(np.clip(value, INT32_MIN, INT32_MAX), 0, 127)
        results.append(hidden)
    return results[0], results[1]


def integer_scores_v5(params, own, opp, buckets):
    pairs = own * opp
    l1 = pairs @ params["l1_weights"].T + params["l1_bias"]
    l1 >>= params["l1_shift"]
    l1 = np.clip(l1, 0, 127)
    output = params["output_bias"][buckets] + (l1 * params["output_weights"][buckets]).sum(axis=1)
    return output >> params["output_shift"]


def saturation_fractions_v5(params) -> tuple[float, float, float]:
    w1 = float(np.mean(np.abs(params["feature_weights"]) >= W1_LIMIT))
    l1 = float(np.mean(np.abs(params["l1_weights"]) >= L1_LIMIT))
    w2 = float(np.mean(np.abs(params["output_weights"]) >= L1_LIMIT))
    return w1, l1, w2


def hidden_activations(params: dict, indices: np.ndarray, offsets: np.ndarray,
                       samples: np.ndarray, hidden_units: int) -> np.ndarray:
    """Hidden activations clipped to the CReLU range (int64, one row per sample)."""
    weight = params["feature_weights"].reshape(INPUT_UNITS, hidden_units)
    count = samples.size
    hidden = np.tile(params["hidden_bias"], (count, 1))
    for position, sample in enumerate(samples):
        begin = offsets[sample]
        end = offsets[sample + 1]
        if end > begin:
            hidden[position, :] += weight[indices[begin:end], :].sum(axis=0)
    np.clip(hidden, INT32_MIN, INT32_MAX, out=hidden)
    np.clip(hidden, 0, 127, out=hidden)
    return hidden


def integer_scores(params: dict, hidden: np.ndarray, buckets: np.ndarray,
                   hidden_units: int) -> np.ndarray:
    half = hidden_units // 2
    pairs = hidden[:, :half] * hidden[:, half:]
    weight = params["output_weights"]
    bias = params["output_bias"]
    y = bias[buckets] + (pairs * weight[buckets]).sum(axis=1)
    return y >> int(params["output_shift"])


def saturation_fractions(params: dict, hidden_units: int) -> tuple[float, float]:
    weight = params["feature_weights"]
    output = params["output_weights"]
    return (
        float(np.mean(np.abs(weight) >= W1_LIMIT)),
        float(np.mean(np.abs(output) >= W2_LIMIT)),
    )


# ---------------------------------------------------------------------------
# Container writing
# ---------------------------------------------------------------------------


def nnue_payload_v4(params: dict, hidden_units: int) -> bytes:
    feature = params["feature_weights"].astype("<i2").reshape(-1).tobytes()
    hidden_bias = params["hidden_bias"].astype("<i4").reshape(-1).tobytes()
    output = params["output_weights"].astype("<i1").reshape(-1).tobytes()
    output_bias = params["output_bias"].astype("<i4").reshape(-1).tobytes()
    return feature + hidden_bias + output + output_bias


def nnue_container_v4(payload: bytes, hidden_units: int, hidden_shift: int,
                      output_shift: int) -> tuple[bytes, str]:
    payload_hash = hashlib.sha256(payload).hexdigest()
    header = struct.pack(
        "<8sIIIIIBB2xHHQ",
        MAGIC,
        VERSION,
        INPUT_UNITS,
        hidden_units,
        OUTPUT_BUCKETS,
        0,
        hidden_shift,
        output_shift,
        len(QUANTIZATION),
        len(FEATURE_SET),
        len(payload),
    )
    container = (
        header
        + bytes.fromhex(payload_hash)
        + QUANTIZATION
        + FEATURE_SET
        + payload
    )
    return container, payload_hash


def nnue_payload_v5(params, hidden_units: int, l1_units: int) -> bytes:
    return (params["feature_weights"].astype("<i2").reshape(-1).tobytes() +
            params["hidden_bias"].astype("<i4").reshape(-1).tobytes() +
            params["l1_weights"].astype("<i1").reshape(-1).tobytes() +
            params["l1_bias"].astype("<i4").reshape(-1).tobytes() +
            params["output_weights"].astype("<i1").reshape(-1).tobytes() +
            params["output_bias"].astype("<i4").reshape(-1).tobytes())


def nnue_container_v5(payload, hidden_units: int, l1_units: int, hidden_shift: int,
                      output_shift: int, l1_shift: int):
    payload_hash = hashlib.sha256(payload).hexdigest()
    header = struct.pack("<8sIIIIIBBBBHHQ", MAGIC, V5_VERSION, V5_INPUT_UNITS,
                         hidden_units, OUTPUT_BUCKETS, l1_units, hidden_shift,
                         output_shift, l1_shift, 0, len(QUANTIZATION),
                         len(V5_FEATURE_SET), len(payload))
    container = header + bytes.fromhex(payload_hash) + QUANTIZATION + V5_FEATURE_SET + payload
    return container, payload_hash


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dataset", type=pathlib.Path, default=None,
                        help="koi-dataset-v1 binary dataset (preferred)")
    parser.add_argument("--corpus", type=pathlib.Path, default=None,
                        help="text FEN;cp;best_move corpus")
    parser.add_argument("--net-out", type=pathlib.Path,
                        default=pathlib.Path("artifacts/training/koi-v4.nnue"))
    parser.add_argument("--meta-out", type=pathlib.Path,
                        default=pathlib.Path("artifacts/training/koi-v4.metadata.json"))
    parser.add_argument("--rows", type=int, default=0, help="0 = all available rows")
    parser.add_argument("--epochs", type=int, default=10)
    parser.add_argument("--batch-size", type=int, default=8192)
    parser.add_argument("--learning-rate", type=float, default=0.002)
    parser.add_argument("--weight-decay", type=float, default=1e-4)
    parser.add_argument("--val-fraction", type=float, default=0.05)
    parser.add_argument("--seed", type=int, default=20260916)
    parser.add_argument("--threads", type=int, default=4)
    parser.add_argument("--hidden-units", type=int, default=1024)
    parser.add_argument("--hidden-shifts", type=int, nargs="+", default=[6, 7, 8])
    parser.add_argument("--output-shifts", type=int, nargs="+", default=[4, 5, 6, 7, 8])
    parser.add_argument("--tune-samples", type=int, default=4000)
    parser.add_argument("--float-out", type=pathlib.Path, default=None,
                        help="optional path for a float checkpoint after training")
    parser.add_argument("--float-in", type=pathlib.Path, default=None,
                        help="load a float checkpoint and skip training (quantize only)")
    parser.add_argument("--arch", choices=["v4", "v5"], default="v5",
                        help="network architecture to train (default: v5)")
    parser.add_argument("--l1-units", type=int, default=V5_L1_UNITS,
                        help="v5 hidden L1 width (8..128)")
    parser.add_argument("--l1-shifts", type=int, nargs="+", default=[6, 7, 8],
                        help="v5 quantized L1 shift candidates")
    return parser


def main_v5(args, rng) -> int:
    """Train, quantize, and export a v5 network (threats + dual-perspective head)."""
    if torch is None:  # pragma: no cover - environment dependent
        raise TrainerError("PyTorch is required for training; install torch")
    if not args.dataset and not args.corpus:
        raise TrainerError("either --dataset or --corpus is required")
    torch.set_num_threads(max(1, args.threads))
    hidden_units = args.hidden_units
    if hidden_units < 32 or hidden_units % 2 != 0:
        raise TrainerError("hidden units must be even and at least 32")
    l1_units = args.l1_units
    if l1_units < 8 or l1_units > 128:
        raise TrainerError("L1 units must be between 8 and 128")
    if args.dataset:
        (own_indices, own_offsets, opp_indices, opp_offsets, scores,
         buckets) = load_binary_dataset_v2(pathlib.Path(args.dataset), args.rows)
    else:
        (own_indices, own_offsets, opp_indices, opp_offsets, scores,
         buckets) = load_text_corpus_v5(pathlib.Path(args.corpus), args.rows)
    rows = own_offsets.size - 1
    if rows <= 0:
        raise TrainerError("no training rows were loaded")
    buckets = np.asarray(buckets, dtype=np.int64)
    if buckets.size != rows or int(buckets.min()) < 0 or int(buckets.max()) >= OUTPUT_BUCKETS:
        raise TrainerError("piece-count buckets are outside the v5 head range")
    own_active = int(np.max(own_offsets[1:] - own_offsets[:-1]))
    opp_active = int(np.max(opp_offsets[1:] - opp_offsets[:-1]))
    pad_length = max(own_active, opp_active, 1)
    started = time.perf_counter()
    log(f"loaded {rows} rows ({own_active} own, {opp_active} opp, pad {pad_length}) "
        f"in {time.perf_counter() - started:.2f}s")
    order = rng.permutation(rows)
    val_count = max(1, int(rows * args.val_fraction))
    val_order = order[:val_count]
    train_order = order[val_count:]
    targets = scores.astype(np.float32) / TARGET_SCALE
    model = KoiNetV5(hidden_units, l1_units)
    if args.float_in:
        model.load_state_dict(torch.load(args.float_in, map_location="cpu"))
        log(f"loaded float checkpoint from {args.float_in}")
    epochs = 0 if args.float_in else max(1, args.epochs)
    optimizer = torch.optim.AdamW(model.parameters(), lr=args.learning_rate,
                                  weight_decay=args.weight_decay)
    scheduler = torch.optim.lr_scheduler.CosineAnnealingLR(optimizer, T_max=max(1, epochs))
    loss_fn = torch.nn.SmoothL1Loss(reduction="sum")
    for epoch in range(1, epochs + 1):
        model.train()
        epoch_started = time.perf_counter()
        loss_total = 0.0
        seen = 0
        for (own_flat, own_weights, opp_flat, opp_weights, batch_buckets,
             batch_targets) in batches_dual(
                own_indices, own_offsets, opp_indices, opp_offsets, buckets, targets,
                train_order, args.batch_size, pad_length):
            optimizer.zero_grad()
            prediction = model(own_flat, own_weights, opp_flat, opp_weights, batch_buckets)
            loss = loss_fn(prediction, batch_targets)
            loss.backward()
            optimizer.step()
            loss_total += float(loss.detach())
            seen += int(own_flat.shape[0])
        scheduler.step()
        train_loss = loss_total / max(1, seen)
        val_prediction = validate_v5(model, own_indices, own_offsets, opp_indices,
                                     opp_offsets, buckets, targets, val_order,
                                     args.batch_size, pad_length)
        val_loss = float(np.mean(np.abs(val_prediction / TARGET_SCALE - targets[val_order])))
        val_mae = float(np.mean(np.abs(val_prediction - scores[val_order])))
        log(f"epoch {epoch}/{epochs} train_loss {train_loss:.5f} val_loss {val_loss:.5f} "
            f"val_mae_cp {val_mae:.2f} time {time.perf_counter() - epoch_started:.1f}s")
    if args.float_out:
        torch.save(model.state_dict(), args.float_out)
        log(f"saved float checkpoint to {args.float_out}")

    tune_order = val_order
    if args.tune_samples and args.tune_samples < tune_order.size:
        tune_order = rng.choice(tune_order, size=args.tune_samples, replace=False)
    output_weight = model.output_weight.detach().numpy().astype(np.float64)
    output_bias = model.output_bias.detach().numpy().astype(np.float64)
    best = None
    for hidden_shift in args.hidden_shifts:
        for l1_shift in args.l1_shifts:
            params = quantize_v5(model, hidden_shift, l1_shift)
            own_hidden, opp_hidden = hidden_activations_dual(
                params, own_indices, own_offsets, opp_indices, opp_offsets, tune_order,
                hidden_units)
            for output_shift in args.output_shifts:
                params["output_weights"] = np.clip(
                    np.rint(output_weight * TARGET_SCALE * float(2**output_shift) / 127.0),
                    -L1_LIMIT, L1_LIMIT).astype(np.int64)
                params["output_bias"] = np.rint(
                    output_bias * TARGET_SCALE * float(2**output_shift)).astype(np.int64)
                params["output_shift"] = output_shift
                candidate = integer_scores_v5(params, own_hidden, opp_hidden,
                                              buckets[tune_order])
                mae = float(np.mean(np.abs(candidate - scores[tune_order])))
                log(f"quantization v5 s1={hidden_shift} s_l1={l1_shift} k3={output_shift} "
                    f"val_mae_cp {mae:.2f}")
                if best is None or mae < best[0]:
                    best = (mae, hidden_shift, l1_shift, output_shift, dict(params))
    if best is None:
        raise TrainerError("no quantization candidate was evaluated")
    best_mae, best_hidden_shift, best_l1_shift, best_output_shift, best_params = best
    log(f"selected v5 s1={best_hidden_shift} s_l1={best_l1_shift} k3={best_output_shift} "
        f"val_mae_cp {best_mae:.2f}")

    float_prediction = validate_v5(model, own_indices, own_offsets, opp_indices, opp_offsets,
                                   buckets, targets, val_order, args.batch_size, pad_length)
    float_mae = float(np.mean(np.abs(float_prediction - scores[val_order])))
    w1_saturation, l1_saturation, w2_saturation = saturation_fractions_v5(best_params)
    payload = nnue_payload_v5(best_params, hidden_units, l1_units)
    container, payload_hash = nnue_container_v5(payload, hidden_units, l1_units,
                                                best_hidden_shift, best_output_shift,
                                                best_l1_shift)
    net_out = args.net_out
    meta_out = args.meta_out
    if pathlib.Path(net_out).name == "koi-v4.nnue":
        net_out = str(pathlib.Path(net_out).with_name("koi-v5.nnue"))
    if pathlib.Path(meta_out).name == "koi-v4.metadata.json":
        meta_out = str(pathlib.Path(meta_out).with_name("koi-v5.metadata.json"))
    net_path = pathlib.Path(net_out)
    net_path.parent.mkdir(parents=True, exist_ok=True)
    net_path.write_bytes(container)
    metadata = {
        "schema": "koi-nnue-training-metadata-v3",
        "created": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "corpus": str(args.dataset or args.corpus),
        "rows_used": int(rows),
        "val_rows": int(val_order.size),
        "feature_set": V5_FEATURE_SET.decode(),
        "quantization": QUANTIZATION.decode(),
        "architecture": {
            "input": V5_INPUT_UNITS,
            "hidden": hidden_units,
            "l1": l1_units,
            "output_buckets": OUTPUT_BUCKETS,
            "groups": [FEATURE_SET.decode(), THREAT_FEATURE_SET.decode()],
        },
        "activation": "crelu-pair-l1",
        "hidden_shift": best_hidden_shift,
        "l1_shift": best_l1_shift,
        "output_shift": best_output_shift,
        "epochs": max(1, args.epochs),
        "batch_size": args.batch_size,
        "learning_rate": args.learning_rate,
        "seed": args.seed,
        "target_scale": TARGET_SCALE,
        "val_mae_cp": float_mae,
        "val_round_trip_mae_cp": best_mae,
        "w1_saturation": w1_saturation,
        "l1_saturation": l1_saturation,
        "w2_saturation": w2_saturation,
        "payload_sha256": payload_hash,
        "network_sha256": hashlib.sha256(container).hexdigest(),
        "command": " ".join(sys.argv),
    }
    meta_path = pathlib.Path(meta_out)
    meta_path.parent.mkdir(parents=True, exist_ok=True)
    meta_path.write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    log(f"wrote {net_path} ({len(container)} bytes) and {meta_path}")
    return 0


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if torch is None:
        print("train_nnue_koi.py: PyTorch is required for training (pip install torch)",
              file=sys.stderr)
        return 2
    if args.dataset is None and args.corpus is None:
        print("train_nnue_koi.py: pass --dataset or --corpus", file=sys.stderr)
        return 2
    torch.set_num_threads(max(1, args.threads))
    rng = np.random.default_rng(args.seed)
    torch.manual_seed(args.seed)
    if args.arch == "v5":
        return main_v5(args, rng)

    started = time.time()
    try:
        limit = args.rows if args.rows > 0 else 0
        if args.dataset is not None:
            indices, offsets, scores = load_binary_dataset(args.dataset, limit)
        else:
            indices, offsets, scores = load_text_corpus(args.corpus, limit)
    except TrainerError as error:
        print(f"train_nnue_koi.py: {error}", file=sys.stderr)
        return 2
    if offsets.size <= 1:
        print("train_nnue_koi.py: dataset has no usable rows", file=sys.stderr)
        return 2
    buckets = piece_count_buckets(offsets)
    pad_length = int((offsets[1:] - offsets[:-1]).max())
    log(
        f"loaded {offsets.size - 1} rows ({indices.size / max(offsets.size - 1, 1):.1f} "
        f"active, pad {pad_length}) in {time.time() - started:.1f}s"
    )

    order = rng.permutation(offsets.size - 1)
    val_count = max(1, int((offsets.size - 1) * args.val_fraction))
    val_order = order[:val_count]
    train_order = order[val_count:]
    targets = scores.astype(np.float64) / TARGET_SCALE

    model = KoiNet(args.hidden_units)
    if args.float_in is not None:
        model.load_state_dict(torch.load(args.float_in, map_location="cpu"))
        log(f"loaded float checkpoint {args.float_in}; skipping training")

    optimizer = torch.optim.AdamW(
        model.parameters(), lr=args.learning_rate, weight_decay=args.weight_decay
    )
    scheduler = torch.optim.lr_scheduler.CosineAnnealingLR(
        optimizer, T_max=max(1, args.epochs)
    )
    loss_fn = nn.SmoothL1Loss()
    epochs = args.epochs if args.float_in is None else 0
    for epoch in range(epochs):
        model.train()
        epoch_started = time.time()
        total = 0.0
        seen = 0
        permutation = rng.permutation(train_order)
        for flat, weights, batch_buckets, batch_targets in batches(
            indices, offsets, buckets, targets, permutation, args.batch_size, pad_length
        ):
            optimizer.zero_grad(set_to_none=True)
            prediction = model(flat, weights, batch_buckets)
            loss = loss_fn(prediction, batch_targets)
            loss.backward()
            optimizer.step()
            total += float(loss.detach()) * flat.shape[0]
            seen += flat.shape[0]
        prediction = validate(model, indices, offsets, buckets, scores, val_order,
                              args.batch_size, pad_length)
        val_mae = float(np.mean(np.abs(prediction - scores[val_order])))
        val_loss = float(np.mean(np.abs(prediction / TARGET_SCALE -
                                        scores[val_order] / TARGET_SCALE)))
        log(
            f"epoch {epoch + 1}/{epochs} train_loss {total / max(seen, 1):.5f} "
            f"val_loss {val_loss:.5f} val_mae_cp {val_mae:.1f} "
            f"time {time.time() - epoch_started:.1f}s"
        )
        scheduler.step()

    if args.float_out is not None:
        args.float_out.parent.mkdir(parents=True, exist_ok=True)
        torch.save(model.state_dict(), args.float_out)
        log(f"wrote float checkpoint {args.float_out}")

    tune_order = val_order if val_order.size <= args.tune_samples else \
        rng.choice(val_order, size=args.tune_samples, replace=False)
    best = None
    for hidden_shift in args.hidden_shifts:
        params = quantize_v4(model, hidden_shift, 0)
        hidden = hidden_activations(params, indices, offsets, tune_order, args.hidden_units)
        for output_shift in args.output_shifts:
            params["output_weights"] = np.clip(
                np.rint(model.output_weight.detach().numpy().astype(np.float64)
                        * TARGET_SCALE * (2.0**output_shift) / (2.0 ** (2 * hidden_shift))),
                -W2_LIMIT, W2_LIMIT,
            ).astype(np.int64)
            params["output_bias"] = np.rint(
                model.output_bias.detach().numpy().astype(np.float64)
                * TARGET_SCALE * (2.0**output_shift)
            ).astype(np.int64)
            params["output_shift"] = output_shift
            candidate = integer_scores(params, hidden, buckets[tune_order], args.hidden_units)
            mae = float(np.mean(np.abs(candidate - scores[tune_order])))
            log(f"quantization v4 s1={hidden_shift} k3={output_shift} val_mae_cp {mae:.1f}")
            if best is None or mae < best[0]:
                best = (mae, hidden_shift, output_shift, {
                    key: (value.copy() if isinstance(value, np.ndarray) else value)
                    for key, value in params.items()
                })
    assert best is not None
    mae, hidden_shift, output_shift, params = best
    log(f"selected v4 s1={hidden_shift} k3={output_shift} val_mae_cp {mae:.1f}")

    float_mae = float(np.mean(np.abs(
        validate(model, indices, offsets, buckets, scores, val_order,
                 args.batch_size, pad_length) - scores[val_order]
    )))
    w1_saturation, w2_saturation = saturation_fractions(params, args.hidden_units)
    payload = nnue_payload_v4(params, args.hidden_units)
    container, payload_hash = nnue_container_v4(payload, args.hidden_units,
                                                hidden_shift, output_shift)
    args.net_out.parent.mkdir(parents=True, exist_ok=True)
    args.net_out.write_bytes(container)
    metadata = {
        "schema": "koi-nnue-training-metadata-v2",
        "created": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "corpus": str(args.dataset if args.dataset is not None else args.corpus),
        "rows_used": int(offsets.size - 1),
        "val_rows": int(val_order.size),
        "feature_set": FEATURE_SET.decode(),
        "quantization": QUANTIZATION.decode(),
        "architecture": {
            "input": INPUT_UNITS,
            "hidden": args.hidden_units,
            "output_buckets": OUTPUT_BUCKETS,
        },
        "activation": "crelu-pair",
        "hidden_shift": hidden_shift,
        "output_shift": output_shift,
        "epochs": epochs,
        "batch_size": args.batch_size,
        "learning_rate": args.learning_rate,
        "seed": args.seed,
        "target_scale": TARGET_SCALE,
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
