"""bullet (Rust) trainer backend - placeholder.

``bullet`` (github.com/jw1912/bullet) is the trainer most new engines adopt,
and the Rust toolchain is already installed on this machine (``cargo 1.98``).
The studio keeps a slot for it so the GUI, run layout, validation and install
steps need no rework once the integration lands.

To enable it, three pieces are required:

1. ``tools/nnue/to_binpack.py`` - convert ``artifacts/training/labels.txt``
   (``FEN;cp;bestmove``) into bullet's training format (a ``.binpack`` file
   with position, score and game metadata).
2. ``tools/nnue/bullet_train/`` - a small Cargo crate depending on the
   ``bullet`` crate whose CLI mirrors the torch trainer: reads the binpack,
   trains the network, and writes a Koi container through
   ``tools/measurement/tune_eval.py`` helpers (or a Rust port of the v3
   serializer).
3. A progress adapter: bullet prints its own summary; translate it into the
   studio contract lines (``epoch <i>/<n> ... val_mae_cp <f>``) either in the
   crate or in this module.

Until then the backend reports why it is unavailable instead of failing at
launch time.
"""

from __future__ import annotations

import shutil
from pathlib import Path

from studio_core import REPO_ROOT


def _cargo() -> str | None:
    found = shutil.which("cargo")
    if found:
        return found
    local = Path.home() / ".cargo" / "bin" / "cargo.exe"
    return str(local) if local.exists() else None


class BulletBackend:
    name = "bullet"
    label = "bullet (Rust, experimental)"

    def available(self) -> bool:
        # Deliberately False until the crate and the binpack converter land.
        return False

    def unavailable_reason(self) -> str | None:
        cargo = _cargo()
        if cargo is None:
            return "Rust is not installed (install rustup to prepare this backend)"
        return (
            "not implemented yet: needs tools/nnue/to_binpack.py and a "
            f"bullet_train Cargo crate ({cargo} is available)"
        )

    def cargo(self) -> str | None:
        return _cargo()

    def net_path(self, run_dir: Path) -> Path:
        return Path(run_dir) / "net.nnue"

    def metadata_path(self, run_dir: Path) -> Path:
        return Path(run_dir) / "net.metadata.json"

    def build_command(self, run_dir: Path, config: dict) -> list[str]:
        raise RuntimeError(self.unavailable_reason())

    def planned_layout(self) -> dict[str, str]:
        return {
            "converter": str(REPO_ROOT / "tools" / "nnue" / "to_binpack.py"),
            "crate": str(REPO_ROOT / "tools" / "nnue" / "bullet_train"),
            "data": str(REPO_ROOT / "artifacts" / "training" / "bullet"),
        }
