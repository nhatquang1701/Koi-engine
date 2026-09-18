"""bullet (Rust/CUDA) trainer backend.

Trains the KOI-NNUE v4 architecture on the GPU through
``tools/nnue/run_bullet.py``. The wrapper converts the label corpus to
bulletformat, runs the pinned ``tools/nnue/bullet_train`` crate, measures
validation MAE after every saved checkpoint, and exports the v4 container
through ``tools/measurement/export_bullet_v4.py``.

The backend is available when the wrapper exists, a CUDA 12.x toolkit is
installed, and either the release trainer binary is built or cargo is present
to build it.
"""

from __future__ import annotations

import shutil
from pathlib import Path

from studio_core import (
    REPO_ROOT,
    metadata_path,
    network_file_name,
    network_path,
    python_executable,
)

TRAINER = REPO_ROOT / "tools" / "nnue" / "bullet_train" / "target" / "release" / "bullet_train.exe"
RUNNER = REPO_ROOT / "tools" / "nnue" / "run_bullet.py"
DEFAULT_DATA_DIR = REPO_ROOT / "artifacts" / "training" / "bullet"
CUDA_BIN_CANDIDATES = (
    Path(r"C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.9\bin"),
    Path(r"C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.8\bin"),
    Path(r"C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.6\bin"),
)


def _cargo() -> str | None:
    found = shutil.which("cargo")
    if found:
        return found
    local = Path.home() / ".cargo" / "bin" / "cargo.exe"
    return str(local) if local.exists() else None


def cuda_bin_directory() -> Path | None:
    """First CUDA 12.x ``bin`` directory that exists on this machine."""
    for candidate in CUDA_BIN_CANDIDATES:
        if candidate.is_dir():
            return candidate
    return None


class BulletBackend:
    name = "bullet"
    label = "bullet (Rust/CUDA GPU)"

    def available(self) -> bool:
        return self.unavailable_reason() is None

    def unavailable_reason(self) -> str | None:
        if not RUNNER.is_file():
            return f"{RUNNER} is missing"
        if cuda_bin_directory() is None:
            return "CUDA toolkit 12.x is not installed (no CUDA bin directory found)"
        if not TRAINER.is_file() and _cargo() is None:
            return (
                f"{TRAINER} is not built and cargo is not installed; build it with "
                "'cargo build --release --features cuda --bin bullet_train'"
            )
        return None

    def cargo(self) -> str | None:
        return _cargo()

    def cuda_bin(self) -> Path | None:
        return cuda_bin_directory()

    def net_path(self, run_dir: Path, config: dict | None = None) -> Path:
        return network_path(run_dir, config)

    def metadata_path(self, run_dir: Path, config: dict | None = None) -> Path:
        return metadata_path(run_dir, config)

    def build_command(self, run_dir: Path, config: dict) -> list[str]:
        reason = self.unavailable_reason()
        if reason is not None:
            raise RuntimeError(reason)
        corpus = Path(config.get("corpus", REPO_ROOT / "artifacts" / "training" / "labels.txt"))
        command = [
            python_executable(),
            str(RUNNER),
            "--corpus", str(corpus),
            "--out", str(run_dir),
            "--net-name", network_file_name(config),
            "--hidden", str(config.get("bullet_hidden_units", config.get("koi_hidden_units", 1024))),
            "--batch", str(config.get("batch_size", 8192)),
            "--superbatches", str(config.get("bullet_superbatches", config.get("epochs", 10))),
            "--lr", repr(config.get("learning_rate", 0.002)),
            "--final-lr", repr(config.get("bullet_final_learning_rate", 0.0002)),
            "--seed", str(config.get("seed", 20260916)),
            "--threads", str(config.get("threads", 4)),
            "--save-rate", str(config.get("bullet_save_rate", 1)),
            "--val-fraction", repr(config.get("val_fraction", 0.05)),
            "--data-dir", str(config.get("bullet_data_dir", DEFAULT_DATA_DIR)),
            "--hidden-shifts", *[str(value) for value in config.get("koi_hidden_shifts", [6, 7, 8])],
            "--output-shifts", *[str(value) for value in config.get("koi_output_shifts", [12, 14, 16, 18, 20])],
        ]
        rows = int(config.get("rows", 0))
        if rows > 0:
            command += ["--rows", str(rows)]
        if config.get("bullet_net_id"):
            command += ["--net-id", str(config["bullet_net_id"])]
        return command

    def planned_layout(self) -> dict[str, str]:
        return {
            "converter": str(REPO_ROOT / "tools" / "nnue" / "to_bullet.py"),
            "crate": str(REPO_ROOT / "tools" / "nnue" / "bullet_train"),
            "runner": str(RUNNER),
            "data": str(DEFAULT_DATA_DIR),
        }
