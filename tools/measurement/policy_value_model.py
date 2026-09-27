"""CPU reference implementation and strict v1 container for Koi policy/value models.

The model consumes Koi v5 sparse feature IDs that are already encoded from the
side-to-move perspective. For black, UCI action squares are vertically mirrored
so the action embeddings use the same canonical perspective.
"""

from __future__ import annotations

from dataclasses import dataclass
import hashlib
import math
import os
from pathlib import Path
import re
import struct
import tempfile
from typing import Iterable, Mapping, Any
import zlib

import numpy as np


MAGIC = b"KOIPV1\0\0"
VERSION = 1
FEATURE_SCHEMA_ID = 5
ACTION_ENCODING_ID = 1
HIDDEN_SIZE = 64
FEATURE_COUNT = 36_864
HEADER_STRUCT = struct.Struct("<8sIIIIIQI")
HEADER_SIZE = HEADER_STRUCT.size
WEIGHT_SPECS = (
    ("state_embedding", (FEATURE_COUNT, HIDDEN_SIZE)),
    ("state_bias", (HIDDEN_SIZE,)),
    ("from_embedding", (64, HIDDEN_SIZE)),
    ("to_embedding", (64, HIDDEN_SIZE)),
    ("promotion_embedding", (5, HIDDEN_SIZE)),
    ("value_weights", (HIDDEN_SIZE, 3)),
    ("value_bias", (3,)),
)
WEIGHT_NAMES = tuple(name for name, _ in WEIGHT_SPECS)
PAYLOAD_BYTES = sum(math.prod(shape) for _, shape in WEIGHT_SPECS) * 4 + 4
_ACTION_RE = re.compile(r"^([a-h][1-8])([a-h][1-8])([qrbn])?$")
_PROMOTION_INDEX = {None: 0, "q": 1, "r": 2, "b": 3, "n": 4}
_ACTION_FIELDS = {"uci", "from", "to", "promotion"}


class PolicyValueModelError(ValueError):
    """Invalid model file, weights, sparse features, or legal action input."""


@dataclass(frozen=True)
class Evaluation:
    """Policy probabilities aligned to legal action order and STM WDL/value."""

    priors: tuple[float, ...]
    wdl: tuple[float, float, float]
    value: float


def _as_float32_array(value: Any, shape: tuple[int, ...], name: str) -> np.ndarray:
    try:
        array = np.asarray(value, dtype="<f4")
    except (TypeError, ValueError, OverflowError) as error:
        raise PolicyValueModelError(f"{name} must be float32-compatible") from error
    if array.shape != shape:
        raise PolicyValueModelError(f"{name} must have shape {shape}")
    if not np.isfinite(array).all():
        raise PolicyValueModelError(f"{name} contains NaN or infinity")
    return np.ascontiguousarray(array, dtype="<f4")


def _softmax(logits: np.ndarray) -> np.ndarray:
    # Promote before subtraction/exponentiation so finite float32 model values
    # do not overflow during accumulation or destabilize normalization.
    values = np.asarray(logits, dtype=np.float64)
    values = values - np.max(values)
    probabilities = np.exp(values)
    return probabilities / probabilities.sum()


def _square_index(square: Any, label: str) -> int:
    if not isinstance(square, str) or re.fullmatch(r"[a-h][1-8]", square) is None:
        raise PolicyValueModelError(f"{label} must be a lowercase algebraic square")
    return (ord(square[1]) - ord("1")) * 8 + (ord(square[0]) - ord("a"))


def _validate_actions(actions: Any, black: bool) -> list[tuple[int, int, int]]:
    if not isinstance(actions, (list, tuple)) or not actions:
        raise PolicyValueModelError("legal_actions must be a non-empty ordered list")
    parsed: list[tuple[int, int, int]] = []
    seen: set[str] = set()
    for offset, action in enumerate(actions):
        label = f"legal_actions[{offset}]"
        if not isinstance(action, Mapping) or set(action) != _ACTION_FIELDS:
            raise PolicyValueModelError(f"{label} must contain exactly uci, from, to, and promotion")
        uci = action["uci"]
        if not isinstance(uci, str) or _ACTION_RE.fullmatch(uci) is None:
            raise PolicyValueModelError(f"{label}.uci is not a valid lowercase UCI action")
        match = _ACTION_RE.fullmatch(uci)
        assert match is not None
        source_text, target_text, encoded_promotion = match.groups()
        if source_text == target_text:
            raise PolicyValueModelError(f"{label}.uci cannot move a square to itself")
        if action["from"] != source_text or action["to"] != target_text:
            raise PolicyValueModelError(f"{label} UCI and from/to fields disagree")
        promotion = action["promotion"]
        if (promotion is not None and (not isinstance(promotion, str) or promotion not in _PROMOTION_INDEX)) or promotion != encoded_promotion:
            raise PolicyValueModelError(f"{label}.promotion and UCI suffix disagree")
        if uci in seen:
            raise PolicyValueModelError(f"{label}.uci duplicates an earlier legal action")
        seen.add(uci)
        source = _square_index(source_text, f"{label}.from")
        target = _square_index(target_text, f"{label}.to")
        if black:
            source ^= 56
            target ^= 56
        parsed.append((source, target, _PROMOTION_INDEX[promotion]))
    return parsed


class PolicyValueModel:
    """Fixed-shape v1 Koi policy/value model with NumPy CPU evaluation."""

    def __init__(
        self,
        state_embedding: Any,
        state_bias: Any,
        from_embedding: Any,
        to_embedding: Any,
        promotion_embedding: Any,
        policy_bias: float,
        value_weights: Any,
        value_bias: Any,
    ) -> None:
        values = {
            "state_embedding": state_embedding,
            "state_bias": state_bias,
            "from_embedding": from_embedding,
            "to_embedding": to_embedding,
            "promotion_embedding": promotion_embedding,
            "value_weights": value_weights,
            "value_bias": value_bias,
        }
        for name, shape in WEIGHT_SPECS:
            setattr(self, name, _as_float32_array(values[name], shape, name))
        self.policy_bias = policy_bias

    @property
    def policy_bias(self) -> float:
        return self._policy_bias

    @policy_bias.setter
    def policy_bias(self, value: Any) -> None:
        if isinstance(value, bool):
            raise PolicyValueModelError("policy_bias must be a finite scalar")
        try:
            scalar = float(value)
        except (TypeError, ValueError, OverflowError) as error:
            raise PolicyValueModelError("policy_bias must be a finite scalar") from error
        if not math.isfinite(scalar) or abs(scalar) > np.finfo(np.float32).max:
            raise PolicyValueModelError("policy_bias must be a finite float32 scalar")
        self._policy_bias = float(np.float32(scalar))

    @classmethod
    def zeros(cls) -> "PolicyValueModel":
        """Return a deterministic all-zero model, useful as a reference fixture."""
        arrays = [np.zeros(shape, dtype="<f4") for _, shape in WEIGHT_SPECS]
        return cls(*arrays[:5], 0.0, *arrays[5:])

    def evaluate(
        self,
        sparse_features: Iterable[int],
        legal_actions: list[Mapping[str, Any]] | tuple[Mapping[str, Any], ...],
        *,
        side_to_move: str = "white",
    ) -> Evaluation:
        """Evaluate one position; priors preserve exactly the supplied action order."""
        if side_to_move not in ("white", "black", "w", "b"):
            raise PolicyValueModelError("side_to_move must be white or black")
        black = side_to_move in ("black", "b")
        try:
            feature_values = list(sparse_features)
        except TypeError as error:
            raise PolicyValueModelError("sparse_features must be an iterable of feature indices") from error
        normalized: list[int] = []
        seen_features: set[int] = set()
        for index in feature_values:
            if isinstance(index, bool) or not isinstance(index, (int, np.integer)):
                raise PolicyValueModelError("sparse feature indices must be integers")
            feature = int(index)
            if feature < 0 or feature >= FEATURE_COUNT:
                raise PolicyValueModelError(f"sparse feature index {feature} is out of range")
            if feature in seen_features:
                raise PolicyValueModelError(f"sparse feature index {feature} is duplicated")
            seen_features.add(feature)
            normalized.append(feature)
        actions = _validate_actions(legal_actions, black)

        if normalized:
            hidden = self.state_bias.astype(np.float64) + self.state_embedding[normalized].astype(np.float64).sum(axis=0)
        else:
            hidden = self.state_bias.astype(np.float64)
        hidden = np.maximum(hidden, 0.0)
        logits = np.asarray([
            np.dot(hidden, self.from_embedding[source].astype(np.float64)
                   + self.to_embedding[target].astype(np.float64)
                   + self.promotion_embedding[promotion].astype(np.float64))
            + self.policy_bias
            for source, target, promotion in actions
        ], dtype=np.float64)
        priors = _softmax(logits)
        value_logits = hidden @ self.value_weights.astype(np.float64) + self.value_bias.astype(np.float64)
        wdl = _softmax(value_logits)
        value = float(wdl[0] - wdl[2])
        return Evaluation(
            tuple(float(value) for value in priors),
            (float(wdl[0]), float(wdl[1]), float(wdl[2])),
            value,
        )

    def _payload(self) -> bytes:
        arrays = [getattr(self, name) for name, _ in WEIGHT_SPECS]
        if not math.isfinite(self.policy_bias) or abs(self.policy_bias) > np.finfo(np.float32).max:
            raise PolicyValueModelError("policy_bias must be a finite float32 scalar")
        for name, array in zip(WEIGHT_NAMES, arrays):
            if not np.isfinite(array).all():
                raise PolicyValueModelError(f"{name} contains NaN or infinity")
        chunks = [array.astype("<f4", copy=False).tobytes(order="C") for array in arrays]
        chunks.insert(5, struct.pack("<f", self.policy_bias))
        return b"".join(chunks)

    def write(self, path: str | os.PathLike[str]) -> None:
        """Atomically write a validated v1 model file."""
        destination = Path(path)
        temporary_path: Path | None = None
        try:
            payload = self._payload()
            header = HEADER_STRUCT.pack(
                MAGIC, VERSION, FEATURE_SCHEMA_ID, ACTION_ENCODING_ID,
                HIDDEN_SIZE, FEATURE_COUNT, len(payload), zlib.crc32(payload) & 0xFFFFFFFF,
            )
            destination.parent.mkdir(parents=True, exist_ok=True)
            with tempfile.NamedTemporaryFile(
                mode="wb", dir=destination.parent, prefix=f".{destination.name}.",
                suffix=".tmp", delete=False,
            ) as stream:
                temporary_path = Path(stream.name)
                stream.write(header)
                stream.write(payload)
            os.replace(temporary_path, destination)
        except (OSError, struct.error) as error:
            if temporary_path is not None:
                try:
                    temporary_path.unlink(missing_ok=True)
                except OSError:
                    pass
            raise PolicyValueModelError(f"cannot write policy/value model: {error}") from error
        except BaseException:
            if temporary_path is not None:
                try:
                    temporary_path.unlink(missing_ok=True)
                except OSError:
                    pass
            raise

    @classmethod
    def read(cls, path: str | os.PathLike[str]) -> "PolicyValueModel":
        """Read one exact v1 container, rejecting unsupported or corrupt data."""
        try:
            encoded = Path(path).read_bytes()
        except OSError as error:
            raise PolicyValueModelError(f"cannot read policy/value model: {error}") from error
        if len(encoded) < HEADER_SIZE:
            raise PolicyValueModelError("truncated policy/value model header")
        magic, version, feature_schema, action_encoding, hidden, features, payload_length, crc = HEADER_STRUCT.unpack_from(encoded)
        if magic != MAGIC:
            raise PolicyValueModelError("invalid policy/value model magic")
        if version != VERSION:
            raise PolicyValueModelError(f"unsupported policy/value model version: {version}")
        if feature_schema != FEATURE_SCHEMA_ID:
            raise PolicyValueModelError(f"unsupported feature schema ID: {feature_schema}")
        if action_encoding != ACTION_ENCODING_ID:
            raise PolicyValueModelError(f"unsupported action encoding ID: {action_encoding}")
        if hidden != HIDDEN_SIZE or features != FEATURE_COUNT:
            raise PolicyValueModelError("unsupported policy/value model dimensions")
        if payload_length != PAYLOAD_BYTES:
            raise PolicyValueModelError("invalid policy/value model payload length")
        if len(encoded) != HEADER_SIZE + payload_length:
            raise PolicyValueModelError("policy/value model is truncated or has trailing bytes")
        payload = encoded[HEADER_SIZE:]
        if (zlib.crc32(payload) & 0xFFFFFFFF) != crc:
            raise PolicyValueModelError("policy/value model CRC32 mismatch")
        offset = 0
        arrays = []
        for _, shape in WEIGHT_SPECS[:5]:
            count = math.prod(shape)
            size = count * 4
            array = np.frombuffer(payload, dtype="<f4", count=count, offset=offset).copy().reshape(shape)
            arrays.append(array)
            offset += size
        policy_bias = struct.unpack_from("<f", payload, offset)[0]
        offset += 4
        for _, shape in WEIGHT_SPECS[5:]:
            count = math.prod(shape)
            size = count * 4
            array = np.frombuffer(payload, dtype="<f4", count=count, offset=offset).copy().reshape(shape)
            arrays.append(array)
            offset += size
        if offset != len(payload):
            raise PolicyValueModelError("invalid policy/value model payload layout")
        try:
            return cls(*arrays[:5], policy_bias, *arrays[5:])
        except PolicyValueModelError as error:
            raise PolicyValueModelError(f"invalid policy/value model weights: {error}") from error


def sha256_file(path: str | os.PathLike[str]) -> str:
    """Return the lowercase SHA-256 digest of a model file."""
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()
