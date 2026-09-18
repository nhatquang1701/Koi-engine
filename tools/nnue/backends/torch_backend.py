"""PyTorch (CPU) trainer backend.

Wraps ``tools/measurement/train_nnue_sf.py``, the trainer that produced the
current ``koi-sf-v1`` network.  It emits the progress contract the studio
parses:

    epoch <i>/<n> train_loss <f> val_loss <f> val_mae_cp <f> time <f>s
    quantization ... val_mae_cp <f>
    selected ... val_mae_cp <f>
    wrote <net> (<bytes> bytes) and <metadata>
"""

from __future__ import annotations

import importlib.util
from pathlib import Path

from studio_core import (
    REPO_ROOT,
    metadata_file_name,
    metadata_path,
    network_file_name,
    network_path,
    python_executable,
)

TRAINER = REPO_ROOT / "tools" / "measurement" / "train_nnue_sf.py"


class TorchBackend:
    name = "torch"
    label = "PyTorch (CPU)"

    def available(self) -> bool:
        return importlib.util.find_spec("torch") is not None and TRAINER.exists()

    def unavailable_reason(self) -> str | None:
        if not TRAINER.exists():
            return f"{TRAINER} is missing"
        if importlib.util.find_spec("torch") is None:
            return "PyTorch is not installed for this interpreter (pip install torch)"
        return None

    def net_path(self, run_dir: Path, config: dict | None = None) -> Path:
        return network_path(run_dir, config)

    def metadata_path(self, run_dir: Path, config: dict | None = None) -> Path:
        return metadata_path(run_dir, config)

    def build_command(self, run_dir: Path, config: dict) -> list[str]:
        run_dir = Path(run_dir)
        net_name = network_file_name(config)
        meta_name = metadata_file_name(config)
        argv = [
            str(TRAINER),
            "--corpus",
            str(config["corpus"]),
            "--net-out",
            str(run_dir / net_name),
            "--meta-out",
            str(run_dir / meta_name),
            "--epochs",
            str(config["epochs"]),
            "--batch-size",
            str(config["batch_size"]),
            "--learning-rate",
            repr(float(config["learning_rate"])),
            "--threads",
            str(config["threads"]),
            "--val-fraction",
            repr(float(config["val_fraction"])),
            "--seed",
            str(config["seed"]),
            "--format",
            str(config.get("format", "v3")),
            "--hidden-shift",
            str(config.get("hidden_shift", 7)),
            "--bottleneck-shift",
            str(config.get("bottleneck_shift", 7)),
            "--output-shifts",
            *[str(shift) for shift in config.get("output_shifts", [3, 4, 5, 6])],
        ]
        if int(config.get("rows", 0)) > 0:
            argv += ["--rows", str(config["rows"])]
        if config.get("float_out", True):
            argv += ["--float-out", str(run_dir / "net.pt")]
        return [python_executable(), *argv]
