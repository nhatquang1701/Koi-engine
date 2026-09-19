"""Koi NNUE version 4 trainer backend.

Wraps ``tools/measurement/train_nnue_koi.py``, the trainer for the
``halfka-king-bucket-v1`` feature set and the ``KOI-NNUE`` version 4 container
(9216 inputs, CReLU pair products, eight piece-count output buckets, explicit
``hidden_shift``/``output_shift``).  It emits the same progress contract the
studio parses:

    epoch <i>/<n> train_loss <f> val_loss <f> val_mae_cp <f> time <f>s
    quantization v4 s1=<s> k3=<k> val_mae_cp <f>
    selected v4 s1=<s> k3=<k> val_mae_cp <f>
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

TRAINER = REPO_ROOT / "tools" / "measurement" / "train_nnue_koi.py"

DEFAULT_HIDDEN_UNITS = 1024
DEFAULT_HIDDEN_SHIFTS = (6, 7, 8)
DEFAULT_OUTPUT_SHIFTS = (12, 14, 16, 18, 20)


class KoiBackend:
    name = "koi"
    label = "PyTorch KOI v4 (CPU)"

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
        argv = [str(TRAINER)]
        dataset = config.get("koi_dataset")
        if dataset and Path(dataset).exists():
            argv += ["--dataset", str(dataset)]
        else:
            argv += ["--corpus", str(config["corpus"])]
        argv += [
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
            "--arch",
            str(config.get("arch", "v5")),
            "--hidden-units",
            str(config.get("koi_hidden_units",
                           1536 if str(config.get("arch", "v5")) == "v5" else DEFAULT_HIDDEN_UNITS)),
            "--hidden-shifts",
            *[str(shift) for shift in config.get("koi_hidden_shifts", DEFAULT_HIDDEN_SHIFTS)],
            "--output-shifts",
            *[str(shift) for shift in config.get("koi_output_shifts", DEFAULT_OUTPUT_SHIFTS)],
        ]
        if str(config.get("arch", "v5")) == "v5":
            argv += [
                "--l1-units",
                str(config.get("koi_l1_units", 32)),
                "--l1-shifts",
                *[str(shift) for shift in config.get("koi_l1_shifts", [6, 7, 8])],
            ]
        if int(config.get("rows", 0)) > 0:
            argv += ["--rows", str(config["rows"])]
        if config.get("float_out", True):
            argv += ["--float-out", str(run_dir / "net.pt")]
        return [python_executable(), *argv]
