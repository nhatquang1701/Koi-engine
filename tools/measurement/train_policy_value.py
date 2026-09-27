"""Deterministic CPU-first trainer for the Koi policy/value v1 reference model.

Training data stays in caller-owned local files. This utility exports only the
fixed v1 model container and a provenance/metrics JSON sidecar.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import random
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Any

import numpy as np
try:
    import torch
    from torch import nn
    import torch.nn.functional as F
except ModuleNotFoundError as error:
    if error.name != "torch":
        raise
    torch = None
    nn = None
    F = None

_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:
    sys.path.insert(0, str(_HERE))

import koi_chess as chess  # noqa: E402
import koi_dataset  # noqa: E402
import policy_value_dataset  # noqa: E402
import policy_value_model  # noqa: E402


OUTCOME_INDEX = {"win": 0, "draw": 1, "loss": 2}
PROMOTION_INDEX = {None: 0, "q": 1, "r": 2, "b": 3, "n": 4}
SOURCE_PATHS = (
    "tools/measurement/train_policy_value.py",
    "tools/measurement/policy_value_model.py",
    "tools/measurement/policy_value_dataset.py",
    "tools/measurement/koi_dataset.py",
) + tuple(
    path.relative_to(_HERE.parent.parent).as_posix()
    for path in sorted((_HERE / "koi_chess").glob("*.py"))
)


def _require_torch() -> None:
    if torch is None:
        raise RuntimeError("PyTorch is required to train a Koi policy/value model; install the optional training dependency")


def position_features(board: chess.Board) -> list[int]:
    """Return the documented v5 sparse features from side-to-move perspective."""
    return koi_dataset.halfka_threat_v5_indices(board, board.turn)


def _read_records(path: str | Path) -> list[dict[str, Any]]:
    records = list(policy_value_dataset.read_jsonl(path))
    if not records:
        raise ValueError(f"dataset is empty: {path}")
    for index, record in enumerate(records, 1):
        if record["feature_schema"] != "halfka-threat-v5":
            raise ValueError(f"{path}: record {index} feature_schema must be halfka-threat-v5")
        if record["position"]["variant"] != "standard":
            raise ValueError(f"{path}: record {index} variant must be standard")
        try:
            board = chess.Board(record["position"]["fen"])
        except (ValueError, TypeError) as error:
            raise ValueError(f"{path}: record {index} has an invalid chess position: {error}") from error
        if not board.is_valid():
            raise ValueError(f"{path}: record {index} has an invalid standard chess position")
        # Verify the supplied legal list matches the local rules authority.
        legal_uci = {move.uci() for move in board.legal_moves}
        supplied = {action["uci"] for action in record["legal_actions"]}
        if supplied != legal_uci:
            raise ValueError(f"{path}: record {index} legal_actions must equal the complete legal move set")
    return records


def _check_disjoint(train: list[dict[str, Any]], validation: list[dict[str, Any]]) -> None:
    for field in ("game_id", "opening_id"):
        train_values = {record[field] for record in train}
        overlap = train_values.intersection(record[field] for record in validation)
        if overlap:
            raise ValueError(f"train and validation datasets have {field} overlap: {sorted(overlap)[0]}")


def _digest(path: str | Path) -> str:
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _write_metadata_atomic(path: str | Path, metadata: dict[str, Any]) -> None:
    destination = Path(path)
    destination.parent.mkdir(parents=True, exist_ok=True)
    encoded = json.dumps(metadata, sort_keys=True, indent=2, allow_nan=False) + "\n"
    temporary_path = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="w", encoding="utf-8", newline="\n", dir=destination.parent,
            prefix=f".{destination.name}.", suffix=".tmp", delete=False,
        ) as stream:
            temporary_path = Path(stream.name)
            stream.write(encoded)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary_path, destination)
    except BaseException:
        if temporary_path is not None:
            temporary_path.unlink(missing_ok=True)
        raise


def _source_revision() -> str:
    try:
        return subprocess.run(
            ["git", "rev-parse", "HEAD"], cwd=_HERE.parent.parent,
            check=True, capture_output=True, text=True,
        ).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


def _source_metadata() -> dict[str, Any]:
    repository_root = _HERE.parent.parent
    try:
        status = subprocess.run(
            ["git", "status", "--porcelain", "--untracked-files=all", "--", *SOURCE_PATHS],
            cwd=repository_root, check=True, capture_output=True, text=True,
        ).stdout
        dirty = bool(status.strip())
    except (OSError, subprocess.CalledProcessError):
        dirty = True
    return {
        "source_revision": _source_revision(),
        "source_dirty": dirty,
        "source_sha256": {relative: _digest(repository_root / relative) for relative in SOURCE_PATHS},
    }


def _synchronize_device(device: torch.device) -> None:
    """Wait for queued accelerator work when timing a CUDA section."""
    if device.type == "cuda":
        torch.cuda.synchronize(device)


def _configure_cuda_determinism() -> None:
    """Set cuBLAS's deterministic workspace before CUDA can initialize."""
    config = os.environ.get("CUBLAS_WORKSPACE_CONFIG")
    initialized = bool(torch.cuda.is_initialized())
    if config is None:
        if initialized:
            raise RuntimeError(
                "CUDA is already initialized without CUBLAS_WORKSPACE_CONFIG; restart with deterministic CUDA configured"
            )
        os.environ["CUBLAS_WORKSPACE_CONFIG"] = ":4096:8"
        return
    if config not in {":4096:8", ":16:8"}:
        if initialized:
            raise RuntimeError(
                "CUDA is already initialized with an incompatible CUBLAS_WORKSPACE_CONFIG; restart with :4096:8 or :16:8"
            )
        raise ValueError("CUBLAS_WORKSPACE_CONFIG must be :4096:8 or :16:8 for deterministic CUDA training")


def _cuda_metadata() -> tuple[bool, list[dict[str, Any]]]:
    available = bool(torch.cuda.is_available())
    devices: list[dict[str, Any]] = []
    if available:
        for index in range(torch.cuda.device_count()):
            properties = torch.cuda.get_device_properties(index)
            devices.append({
                "index": index,
                "name": torch.cuda.get_device_name(index),
                "total_memory_bytes": int(properties.total_memory),
            })
    return available, devices


def _cuda_driver_version() -> str | None:
    """Query the NVIDIA driver version with a short timeout; return None if absent."""
    try:
        result = subprocess.run(
            ["nvidia-smi", "--query-gpu=driver_version", "--format=csv,noheader"],
            check=True, capture_output=True, text=True, timeout=2.0,
        )
    except (OSError, subprocess.SubprocessError):
        return None
    versions = [line.strip() for line in result.stdout.splitlines() if line.strip()]
    return versions[0] if versions else None


def _cuda_provenance() -> dict[str, str | None]:
    return {
        "cuda_build_version": str(torch.version.cuda) if torch.version.cuda is not None else None,
        "cuda_driver_version": _cuda_driver_version(),
    }


if nn is not None:
    class _TorchPolicyValue(nn.Module):
        """Trainable factorization matching the fixed v1 inference container."""

        def __init__(self) -> None:
            super().__init__()
            size = policy_value_model.HIDDEN_SIZE
            self.state = nn.EmbeddingBag(
                policy_value_model.FEATURE_COUNT, size, mode="sum", sparse=True,
                include_last_offset=False,
            )
            self.state_bias = nn.Parameter(torch.zeros(size))
            self.from_embedding = nn.Embedding(64, size)
            self.to_embedding = nn.Embedding(64, size)
            self.promotion_embedding = nn.Embedding(5, size)
            self.policy_bias = nn.Parameter(torch.zeros(()))
            self.value_weights = nn.Parameter(torch.empty(size, 3))
            self.value_bias = nn.Parameter(torch.zeros(3))
            nn.init.normal_(self.state.weight, mean=0.0, std=0.01)
            nn.init.normal_(self.from_embedding.weight, mean=0.0, std=0.01)
            nn.init.normal_(self.to_embedding.weight, mean=0.0, std=0.01)
            nn.init.normal_(self.promotion_embedding.weight, mean=0.0, std=0.01)
            nn.init.xavier_uniform_(self.value_weights)

        def forward(self, features: list[int], actions: list[dict[str, Any]], black: bool):
            indices = torch.tensor(features, dtype=torch.long, device=self.state.weight.device)
            offsets = torch.tensor([0], dtype=torch.long, device=indices.device)
            hidden = F.relu(self.state(indices, offsets)[0] + self.state_bias)
            sources, targets, promotions = [], [], []
            for action in actions:
                source = chess.parse_square(action["from"])
                target = chess.parse_square(action["to"])
                if black:
                    source ^= 56
                    target ^= 56
                sources.append(source)
                targets.append(target)
                promotions.append(PROMOTION_INDEX[action["promotion"]])
            source_tensor = torch.tensor(sources, dtype=torch.long, device=indices.device)
            target_tensor = torch.tensor(targets, dtype=torch.long, device=indices.device)
            promotion_tensor = torch.tensor(promotions, dtype=torch.long, device=indices.device)
            action_vectors = (
                self.from_embedding(source_tensor)
                + self.to_embedding(target_tensor)
                + self.promotion_embedding(promotion_tensor)
            )
            policy_logits = action_vectors @ hidden + self.policy_bias
            value_logits = hidden @ self.value_weights + self.value_bias
            return policy_logits, value_logits

        def to_container(self) -> policy_value_model.PolicyValueModel:
            return policy_value_model.PolicyValueModel(
                self.state.weight.detach().cpu().numpy(),
                self.state_bias.detach().cpu().numpy(),
                self.from_embedding.weight.detach().cpu().numpy(),
                self.to_embedding.weight.detach().cpu().numpy(),
                self.promotion_embedding.weight.detach().cpu().numpy(),
                float(self.policy_bias.detach().cpu()),
                self.value_weights.detach().cpu().numpy(),
                self.value_bias.detach().cpu().numpy(),
            )
else:
    class _TorchPolicyValue:
        def __init__(self) -> None:
            _require_torch()


def _example(record: dict[str, Any], device: torch.device):
    board = chess.Board(record["position"]["fen"])
    features = position_features(board)
    policy_target = torch.tensor(record["policy_targets"], dtype=torch.float32, device=device)
    outcome = torch.tensor(OUTCOME_INDEX[record["outcome"]], dtype=torch.long, device=device)
    value_target = torch.tensor(record["value_target"], dtype=torch.float32, device=device)
    return board, features, policy_target, outcome, value_target


def _losses(model: _TorchPolicyValue, record: dict[str, Any], device: torch.device, value_loss_weight: float):
    board, features, policy_target, outcome, value_target = _example(record, device)
    policy_logits, value_logits = model(features, record["legal_actions"], board.turn == chess.BLACK)
    policy_loss = -(policy_target * F.log_softmax(policy_logits, dim=0)).sum()
    wdl_loss = F.cross_entropy(value_logits.unsqueeze(0), outcome.unsqueeze(0))
    wdl_probabilities = torch.softmax(value_logits, dim=0)
    expected_value = wdl_probabilities[0] - wdl_probabilities[2]
    value_loss = F.mse_loss(expected_value, value_target)
    total = policy_loss + wdl_loss + value_loss_weight * value_loss
    return total, policy_loss, wdl_loss, value_loss


def _evaluate(model, records, device, value_loss_weight):
    totals = np.zeros(4, dtype=np.float64)
    with torch.no_grad():
        for record in records:
            losses = _losses(model, record, device, value_loss_weight)
            totals += np.asarray([float(value.detach().cpu()) for value in losses])
    totals /= len(records)
    return {key: float(value) for key, value in zip(("objective", "policy_ce", "wdl_ce", "value_mse"), totals)}


def train_policy_value(
    train_path: str | Path,
    validation_path: str | Path,
    model_path: str | Path,
    metadata_path: str | Path,
    *,
    epochs: int = 10,
    batch_size: int = 32,
    learning_rate: float = 1e-3,
    value_loss_weight: float = 0.1,
    seed: int = 1,
    device: str = "cpu",
) -> dict[str, Any]:
    """Fit a model, validate on a disjoint split, and write model plus metadata."""
    _require_torch()
    if epochs < 1 or batch_size < 1:
        raise ValueError("epochs and batch_size must be positive")
    if learning_rate <= 0 or not np.isfinite(learning_rate):
        raise ValueError("learning_rate must be a finite positive value")
    if value_loss_weight < 0 or not np.isfinite(value_loss_weight):
        raise ValueError("value_loss_weight must be finite and non-negative")
    if device not in {"auto", "cpu", "cuda"}:
        raise ValueError("device must be auto, cpu, or cuda")
    selected_device = "cuda" if device == "cuda" else "cpu"
    if selected_device == "cuda":
        _configure_cuda_determinism()
        if not torch.cuda.is_available():
            raise RuntimeError("CUDA was requested but is unavailable")
    destinations = (train_path, validation_path, model_path, metadata_path)
    resolved_destinations = [os.path.normcase(str(Path(path).resolve())) for path in destinations]
    if len(set(resolved_destinations)) != len(resolved_destinations):
        raise ValueError("train, validation, model, and metadata paths must resolve to distinct files")
    train_records = _read_records(train_path)
    validation_records = _read_records(validation_path)
    _check_disjoint(train_records, validation_records)

    random.seed(seed)
    np.random.seed(seed)
    torch.manual_seed(seed)
    if selected_device == "cuda":
        torch.cuda.manual_seed_all(seed)
    torch.use_deterministic_algorithms(True)
    if selected_device == "cpu":
        torch.set_num_threads(1)
    target_device = torch.device(selected_device)
    model = _TorchPolicyValue().to(target_device)
    state_optimizer = torch.optim.SparseAdam([model.state.weight], lr=learning_rate)
    dense_parameters = [parameter for name, parameter in model.named_parameters() if name != "state.weight"]
    dense_optimizer = torch.optim.Adam(dense_parameters, lr=learning_rate)
    rng = random.Random(seed)
    _synchronize_device(target_device)
    training_start = time.perf_counter()
    for _epoch in range(epochs):
        order = list(range(len(train_records)))
        rng.shuffle(order)
        model.train()
        for start in range(0, len(order), batch_size):
            batch = order[start:start + batch_size]
            state_optimizer.zero_grad(set_to_none=True)
            dense_optimizer.zero_grad(set_to_none=True)
            losses = [_losses(model, train_records[index], target_device, value_loss_weight)[0] for index in batch]
            torch.stack(losses).mean().backward()
            state_optimizer.step()
            dense_optimizer.step()
    _synchronize_device(target_device)
    training_seconds = time.perf_counter() - training_start

    model.eval()
    train_metrics = _evaluate(model, train_records, target_device, value_loss_weight)
    _synchronize_device(target_device)
    validation_start = time.perf_counter()
    validation_metrics = _evaluate(model, validation_records, target_device, value_loss_weight)
    _synchronize_device(target_device)
    validation_seconds = time.perf_counter() - validation_start
    metrics = {
        "train": train_metrics,
        "validation": validation_metrics,
    }
    container = model.to_container()
    container.write(model_path)
    source_metadata = _source_metadata()
    cuda_available, cuda_devices = _cuda_metadata()
    metadata = {
        "schema": "koi-policy-value-training-metadata-v1",
        "train_sha256": _digest(train_path),
        "validation_sha256": _digest(validation_path),
        **source_metadata,
        "model_sha256": policy_value_model.sha256_file(model_path),
        "hyperparameters": {
            "epochs": epochs, "batch_size": batch_size, "learning_rate": learning_rate,
            "value_loss_weight": value_loss_weight, "seed": seed,
        },
        "device": selected_device,
        "metrics": metrics,
        "dataset_counts": {
            "train": len(train_records),
            "validation": len(validation_records),
        },
        "timing": {
            "training_seconds": training_seconds,
            "validation_seconds": validation_seconds,
            "training_positions": len(train_records) * epochs,
            "validation_positions": len(validation_records),
            "train_positions_per_second": len(train_records) * epochs / training_seconds,
            "validation_positions_per_second": len(validation_records) / validation_seconds,
        },
        "environment": {
            "host": {
                "hostname": platform.node(),
                "system": platform.system(),
                "release": platform.release(),
                "machine": platform.machine(),
                "processor": platform.processor() or platform.machine(),
                "logical_cpu_count": os.cpu_count() or 1,
            },
            "pytorch_version": str(torch.__version__),
            "python_version": platform.python_version(),
            "numpy_version": np.__version__,
            "device": selected_device,
            "device_name": (
                torch.cuda.get_device_name(target_device)
                if selected_device == "cuda"
                else platform.processor() or platform.machine()
            ),
            "cuda_available": cuda_available,
            "cuda_devices": cuda_devices,
            **_cuda_provenance(),
        },
    }
    _write_metadata_atomic(metadata_path, metadata)
    return metrics


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--train", required=True, type=Path)
    parser.add_argument("--validation", required=True, type=Path)
    parser.add_argument("--output-model", required=True, type=Path)
    parser.add_argument("--metadata", required=True, type=Path)
    parser.add_argument("--epochs", type=int, default=10)
    parser.add_argument("--batch-size", type=int, default=32)
    parser.add_argument("--learning-rate", type=float, default=1e-3)
    parser.add_argument("--value-loss-weight", type=float, default=0.1)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--device", choices=("auto", "cpu", "cuda"), default="cpu", help="CPU is the default; select cuda explicitly to opt in")
    args = parser.parse_args(argv)
    metrics = train_policy_value(
        args.train, args.validation, args.output_model, args.metadata,
        epochs=args.epochs, batch_size=args.batch_size,
        learning_rate=args.learning_rate, value_loss_weight=args.value_loss_weight,
        seed=args.seed, device=args.device,
    )
    print(json.dumps(metrics, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
