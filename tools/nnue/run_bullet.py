"""Drives the bullet GPU trainer for Koi and speaks the studio progress contract.

The wrapper chains the whole pipeline:

1. ``to_bullet.py`` converts the text corpus into bulletformat ``.data`` files
   (unless ``--train-data``/``--skip-convert`` is given).
2. ``bullet_train.exe`` (built from ``tools/nnue/bullet_train``) trains the v4
   architecture on the GPU.
3. After each saved checkpoint the wrapper measures validation MAE and prints an
   ``epoch i/n train_loss ... val_loss ... val_mae_cp ... time ...s`` line so the
   studio can chart progress.
4. ``export_bullet_v4.py`` quantizes the final checkpoint into a ``KOI-NNUE``
   version 4 container and prints the ``quantization``/``selected``/``wrote``
   contract lines.

Use ``--dry-run`` to print the resolved commands without running anything.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

import numpy as np

REPO_ROOT = Path(__file__).resolve().parents[2]
TOOLS_MEASUREMENT = REPO_ROOT / "tools" / "measurement"
if str(TOOLS_MEASUREMENT) not in sys.path:
    sys.path.insert(0, str(TOOLS_MEASUREMENT))

import export_bullet_v4  # noqa: E402
import to_bullet  # noqa: E402

DEFAULT_TRAINER = REPO_ROOT / "tools" / "nnue" / "bullet_train" / "target" / "release" / "bullet_train.exe"
DEFAULT_DATA_DIR = REPO_ROOT / "artifacts" / "training" / "bullet"
CUDA_BIN_CANDIDATES = (
    Path(os.environ.get("CUDA_PATH", "")) / "bin" if os.environ.get("CUDA_PATH") else None,
    Path(r"C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.9\bin"),
    Path(r"C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.8\bin"),
    Path(r"C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.6\bin"),
)
_SUPERBATCH_RE = re.compile(
    r"superbatch\s+(?P<superbatch>\d+)\s+\|\s+time\s+(?P<seconds>[\d.]+)s\s+\|\s+"
    r"running loss\s+(?P<loss>[\d.]+)"
)
_SAVED_RE = re.compile(r"Saved\s+\[(?P<name>[^\]]+)\]")
_ANSI_RE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")


def strip_ansi(text: str) -> str:
    return _ANSI_RE.sub("", text)


class BulletRunError(RuntimeError):
    pass


def cuda_bin_directory() -> Path | None:
    for candidate in CUDA_BIN_CANDIDATES:
        if candidate is not None and candidate.is_dir():
            return candidate
    return None


def find_trainer(explicit: str | None = None) -> Path:
    if explicit:
        path = Path(explicit)
        if path.is_file():
            return path
        raise BulletRunError(f"trainer not found at {path}")
    if DEFAULT_TRAINER.is_file():
        return DEFAULT_TRAINER
    raise BulletRunError(
        f"trainer not found at {DEFAULT_TRAINER}; build it with "
        "'cargo build --release --features cuda --bin bullet_train'"
    )


def parse_bullet_line(line: str) -> dict | None:
    """Translates a bullet progress line into a machine-readable event."""
    stripped = strip_ansi(line).strip()
    match = _SUPERBATCH_RE.search(stripped)
    if match:
        return {
            "kind": "superbatch",
            "superbatch": int(match.group("superbatch")),
            "seconds": float(match.group("seconds")),
            "loss": float(match.group("loss")),
        }
    match = _SAVED_RE.match(stripped)
    if match:
        return {"kind": "saved", "name": match.group("name")}
    return None


def format_epoch_line(epoch: int, total: int, loss: float, val_mae_cp: float, seconds: float) -> str:
    return (
        f"epoch {epoch}/{total} train_loss {loss:.5f} val_loss {loss:.5f} "
        f"val_mae_cp {val_mae_cp:.1f} time {seconds:.1f}s"
    )


def build_train_command(
    trainer: Path,
    data: Path,
    out: Path,
    net_id: str,
    hidden: int,
    batch: int,
    batches_per_superbatch: int,
    superbatches: int,
    lr: float,
    final_lr: float,
    seed: int,
    threads: int,
    save_rate: int,
) -> list[str]:
    return [
        str(trainer),
        "--data", str(data),
        "--out", str(out),
        "--net-id", net_id,
        "--hidden", str(hidden),
        "--batch", str(batch),
        "--batches-per-superbatch", str(batches_per_superbatch),
        "--superbatches", str(superbatches),
        "--lr", repr(lr),
        "--final-lr", repr(final_lr),
        "--seed", str(seed),
        "--threads", str(threads),
        "--save-rate", str(save_rate),
    ]


def build_export_command(
    checkpoint: Path,
    validation: Path,
    net_out: Path,
    meta_out: Path,
    hidden: int,
    hidden_shifts: list[int],
    output_shifts: list[int],
    tune_samples: int,
) -> list[str]:
    return [
        sys.executable,
        str(TOOLS_MEASUREMENT / "export_bullet_v4.py"),
        "--checkpoint", str(checkpoint),
        "--validation", str(validation),
        "--net-out", str(net_out),
        "--meta-out", str(meta_out),
        "--hidden", str(hidden),
        "--hidden-shifts", *[str(shift) for shift in hidden_shifts],
        "--output-shifts", *[str(shift) for shift in output_shifts],
        "--tune-samples", str(tune_samples),
    ]


def build_convert_command(corpus: Path, data_dir: Path, val_fraction: float, limit: int) -> list[str]:
    return [
        sys.executable,
        str(REPO_ROOT / "tools" / "nnue" / "to_bullet.py"),
        "--input", str(corpus),
        "--output-dir", str(data_dir),
        "--val-fraction", repr(val_fraction),
        "--limit", str(limit),
    ]


def measure_val_mae(raw_path: Path, validation: Path, hidden: int, limit: int = 2000) -> float | None:
    """Float validation MAE for one checkpoint, or None when unmeasurable."""
    try:
        stored_hidden, weights = export_bullet_v4.read_raw_weights(raw_path)
        indices, offsets, scores, buckets = export_bullet_v4.load_validation(validation, limit, 0)
    except (export_bullet_v4.ExportError, OSError):
        return None
    samples = np.arange(scores.size)
    activations = export_bullet_v4.float_activations(weights, indices, offsets, samples, stored_hidden)
    predictions = export_bullet_v4.float_scores(weights, activations, buckets)
    return float(np.abs(predictions - scores).mean())


def _stream(command: list[str], env: dict[str, str]) -> int:
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               text=True, env=env, bufsize=1)
    assert process.stdout is not None
    for line in process.stdout:
        sys.stdout.write(line)
        sys.stdout.flush()
    return process.wait()


def run_pipeline(args: argparse.Namespace) -> int:
    out_dir = args.out
    out_dir.mkdir(parents=True, exist_ok=True)
    data_dir = args.data_dir
    checkpoint_dir = args.checkpoint_dir or (out_dir / "checkpoints")
    net_name = args.net_name
    meta_name = Path(net_name).with_suffix(".metadata.json").name

    trainer = find_trainer(args.trainer)
    data = args.train_data
    validation_text = args.validation

    env = dict(os.environ)
    cuda_bin = cuda_bin_directory()
    if cuda_bin is not None:
        env["PATH"] = str(cuda_bin) + os.pathsep + env.get("PATH", "")

    convert_command = None
    if data is None:
        convert_command = build_convert_command(args.corpus, data_dir, args.val_fraction, args.rows)
        data = data_dir / "train.data"
        validation_text = validation_text or (data_dir / "validation.txt")

    train_command = build_train_command(
        trainer, data, checkpoint_dir, args.net_id, args.hidden, args.batch,
        args.batches_per_superbatch, args.superbatches, args.lr, args.final_lr,
        args.seed, args.threads, args.save_rate,
    )
    export_command = build_export_command(
        checkpoint_dir, validation_text, out_dir / net_name, out_dir / meta_name,
        args.hidden, args.hidden_shifts, args.output_shifts, args.tune_samples,
    )

    if args.dry_run:
        if convert_command is not None:
            print("dry-run convert: " + subprocess.list2cmdline(convert_command))
        print("dry-run train: " + subprocess.list2cmdline(train_command))
        print("dry-run export: " + subprocess.list2cmdline(export_command))
        return 0

    if convert_command is not None:
        if not args.corpus.is_file():
            raise BulletRunError(f"corpus not found at {args.corpus}")
        code = _stream(convert_command, env)
        if code != 0:
            raise BulletRunError(f"dataset conversion failed with exit code {code}")
        if not data.is_file():
            raise BulletRunError(f"converted dataset missing at {data}")

    if not data.is_file():
        raise BulletRunError(f"training dataset not found at {data}")

    process = subprocess.Popen(train_command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               text=True, env=env, bufsize=1)
    assert process.stdout is not None
    latest_raw = None
    last_superbatch: dict | None = None
    for line in process.stdout:
        clean = strip_ansi(line)
        sys.stdout.write(clean)
        sys.stdout.flush()
        event = parse_bullet_line(clean)
        if event is None:
            continue
        if event["kind"] == "superbatch":
            last_superbatch = event
            continue
        latest_name = event["name"]
        candidate = checkpoint_dir / latest_name / "raw.bin"
        if not candidate.is_file():
            continue
        latest_raw = candidate
        if validation_text is not None and validation_text.is_file():
            mae = measure_val_mae(candidate, validation_text, args.hidden)
            if mae is not None:
                loss = last_superbatch["loss"] if last_superbatch else 0.0
                seconds = last_superbatch["seconds"] if last_superbatch else 0.0
                print(format_epoch_line(
                    _saved_index(latest_name, args.superbatches),
                    args.superbatches,
                    loss,
                    mae,
                    seconds,
                ))
                sys.stdout.flush()
    code = process.wait()
    if code != 0:
        raise BulletRunError(f"bullet training failed with exit code {code}")
    if latest_raw is None:
        raise BulletRunError(f"no checkpoint found under {checkpoint_dir}")

    print(f"run_bullet: exporting {latest_raw}")
    export_command = build_export_command(
        checkpoint_dir, validation_text, out_dir / net_name, out_dir / meta_name,
        args.hidden, args.hidden_shifts, args.output_shifts, args.tune_samples,
    )
    code = _stream(export_command, env)
    if code != 0:
        raise BulletRunError(f"export failed with exit code {code}")
    print(f"run_bullet: finished {out_dir / net_name}")
    return 0


def _saved_index(name: str, fallback: int) -> int:
    try:
        return int(name.rsplit("-", 1)[1])
    except (IndexError, ValueError):
        return fallback


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Train a Koi v4 network with bullet.")
    parser.add_argument("--corpus", type=Path, default=REPO_ROOT / "artifacts" / "training" / "labels.txt")
    parser.add_argument("--out", type=Path, required=True, help="run directory for the network and metadata")
    parser.add_argument("--data-dir", type=Path, default=DEFAULT_DATA_DIR)
    parser.add_argument("--checkpoint-dir", type=Path)
    parser.add_argument("--train-data", type=Path, help="skip conversion and use this .data file")
    parser.add_argument("--validation", type=Path, help="validation text file for MAE reporting")
    parser.add_argument("--net-name", default="koi.nnue")
    parser.add_argument("--net-id", default="koi-v4")
    parser.add_argument("--hidden", type=int, default=1024)
    parser.add_argument("--batch", type=int, default=8192)
    parser.add_argument("--batches-per-superbatch", type=int, default=610)
    parser.add_argument("--superbatches", type=int, default=100)
    parser.add_argument("--lr", type=float, default=0.002)
    parser.add_argument("--final-lr", type=float, default=0.0002)
    parser.add_argument("--seed", type=int, default=20260916)
    parser.add_argument("--threads", type=int, default=4)
    parser.add_argument("--save-rate", type=int, default=10)
    parser.add_argument("--val-fraction", type=float, default=0.05)
    parser.add_argument("--rows", type=int, default=0)
    parser.add_argument("--tune-samples", type=int, default=4000)
    parser.add_argument("--hidden-shifts", type=int, nargs="+", default=[6, 7, 8])
    parser.add_argument("--output-shifts", type=int, nargs="+", default=[12, 14, 16, 18, 20])
    parser.add_argument("--trainer", help="explicit path to bullet_train.exe")
    parser.add_argument("--dry-run", action="store_true")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        return run_pipeline(args)
    except (BulletRunError, to_bullet.ConversionError, export_bullet_v4.ExportError) as error:
        print(f"run_bullet: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
