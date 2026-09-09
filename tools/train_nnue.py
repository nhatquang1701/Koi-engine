#!/usr/bin/env python3
"""Train or synthesize a Koi-native NNUE container.

This stable entry point delegates to the versioned ``nnue`` command in
``tune_eval.py`` so training and export share one container implementation.
"""

from __future__ import annotations

import sys

from tune_eval import nnue_main


if __name__ == "__main__":
    raise SystemExit(nnue_main(sys.argv[1:]))
