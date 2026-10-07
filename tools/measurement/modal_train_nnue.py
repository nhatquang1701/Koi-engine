"""Train a Koi NNUE network on Modal (serverless GPU).

Optional cloud path for the same trainer used locally
(``tools/measurement/train_nnue_koi.py``).  The default run is the decisive
v6-on-Stockfish-labels recipe; every parameter can be overridden from the CLI.
The container runs the corpus perspective validator first and aborts on a
mismatch, exactly like the local training wrappers.

Setup (one time, from the repository root):

    pip install -U modal
    modal token new                      # browser login; the free plan is fine

Upload the corpus once into the Modal volume:

    modal volume put koi-training artifacts/training/labels.txt /labels.txt

Smoke test (20 epochs, a few minutes on an A10):

    modal run tools/measurement/modal_train_nnue.py --epochs 20 --run-name koi-v6-labels-smoke

Full run (440 epochs, a few hours):

    modal run tools/measurement/modal_train_nnue.py

Download the outputs (net + metadata + log):

    modal volume get koi-training out/koi-v6-labels-modal .

Notes:

- The function requests an A10 GPU ($1.10/h).  That is the best speed/cost
  pick for this workload: the trainer is embedding/optimizer memory-bound, and
  the A10's ~600 GB/s of memory bandwidth is twice the L4's for 1.4x the
  price.  Edit the ``gpu=`` argument in the ``@app.function`` decorator to use
  a different class (``L4`` for the cheapest run, ``A100``, ``H100``, ...);
  ``gpu="any"`` is the cheapest fallback.
- A smoke run costs cents and a full run a few dollars; the free plan's
  monthly compute credit covers the test.
- Outputs are written to ``/data/out/<run-name>/`` inside the volume; commit
  happens after training and ``modal volume get`` copies them back.
"""

from __future__ import annotations

import pathlib
import subprocess
import sys

import modal

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]

app = modal.App("koi-nnue-train")

volume = modal.Volume.from_name("koi-training", create_if_missing=True)

image = (
    modal.Image.debian_slim(python_version="3.12")
    .pip_install("torch", "numpy")
    .add_local_dir(str(REPO_ROOT / "tools"), remote_path="/root/koi/tools")
)

TRAIN_SCRIPT = "/root/koi/tools/measurement/train_nnue_koi.py"
VALIDATE_SCRIPT = "/root/koi/tools/measurement/validate_corpus_perspective.py"


@app.function(image=image, gpu="A10", volumes={"/data": volume}, timeout=12 * 60 * 60)
def train(
    corpus: str = "/data/labels.txt",
    arch: str = "v6",
    epochs: int = 440,
    batch_size: int = 4096,
    learning_rate: float = 0.0015,
    threads: int = 2,
    val_fraction: float = 0.05,
    seed: int = 20260916,
    hidden_units: int = 1536,
    l1_units: int = 32,
    tune_samples: int = 20000,
    run_name: str = "koi-v6-labels-modal",
) -> str:
    """Run the validator, then the trainer, on one GPU.  Returns the output dir."""
    import os

    out_dir = f"/data/out/{run_name}"
    os.makedirs(out_dir, exist_ok=True)
    net_out = f"{out_dir}/{run_name}.nnue"
    meta_out = f"{out_dir}/{run_name}.metadata.json"
    float_out = f"{out_dir}/{run_name}.pt"
    log_path = f"{out_dir}/train.log"

    env = dict(os.environ, KOI_NNUE_DEVICE="cuda")
    with open(log_path, "w", encoding="utf-8") as log:
        subprocess.run(
            [sys.executable, VALIDATE_SCRIPT, corpus, "--sample", "20000"],
            stdout=log,
            stderr=subprocess.STDOUT,
            check=True,
            env=env,
        )
        subprocess.run(
            [
                sys.executable,
                TRAIN_SCRIPT,
                "--corpus", corpus,
                "--arch", arch,
                "--epochs", str(epochs),
                "--batch-size", str(batch_size),
                "--learning-rate", str(learning_rate),
                "--threads", str(threads),
                "--val-fraction", str(val_fraction),
                "--seed", str(seed),
                "--hidden-units", str(hidden_units),
                "--l1-units", str(l1_units),
                "--hidden-shifts", "6", "7", "8",
                "--l1-shifts", "6", "7", "8",
                "--tune-samples", str(tune_samples),
                "--net-out", net_out,
                "--meta-out", meta_out,
                "--float-out", float_out,
            ],
            stdout=log,
            stderr=subprocess.STDOUT,
            check=True,
            env=env,
        )

    volume.commit()
    return out_dir


@app.local_entrypoint()
def main(
    corpus: str = "/data/labels.txt",
    arch: str = "v6",
    epochs: int = 440,
    batch_size: int = 4096,
    learning_rate: float = 0.0015,
    threads: int = 2,
    val_fraction: float = 0.05,
    seed: int = 20260916,
    hidden_units: int = 1536,
    l1_units: int = 32,
    tune_samples: int = 20000,
    run_name: str = "koi-v6-labels-modal",
) -> None:
    print(f"launching {run_name}: arch={arch} epochs={epochs} corpus={corpus}")
    out_dir = train.remote(
        corpus=corpus,
        arch=arch,
        epochs=epochs,
        batch_size=batch_size,
        learning_rate=learning_rate,
        threads=threads,
        val_fraction=val_fraction,
        seed=seed,
        hidden_units=hidden_units,
        l1_units=l1_units,
        tune_samples=tune_samples,
        run_name=run_name,
    )
    print(f"done: {out_dir}")
    print(f"download with: modal volume get koi-training out/{run_name} .")
