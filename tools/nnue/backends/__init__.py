"""Trainer backend registry for the Koi NNUE Studio.

A backend is deliberately small: it reports whether it can run, builds the
command line for a training run and says where the produced files live.  The
run layout, progress parsing, validation and install steps are backend
agnostic, so adding a new trainer (for example a future bullet/Rust one) means
adding one module here and nothing else.
"""

from __future__ import annotations

from .bullet_backend import BulletBackend
from .koi_backend import KoiBackend
from .torch_backend import TorchBackend

_BACKENDS = (KoiBackend(), TorchBackend(), BulletBackend())


def available_backends():
    """All known backends, in display order."""
    return list(_BACKENDS)


def get_backend(name: str):
    for backend in _BACKENDS:
        if backend.name == name:
            return backend
    raise KeyError(f"unknown backend: {name}")
