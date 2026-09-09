#!/usr/bin/env python3
"""Export a validated Koi-native NNUE container from split corpora."""

from __future__ import annotations

import sys

from tune_eval import nnue_main


if __name__ == "__main__":
    raise SystemExit(nnue_main(sys.argv[1:]))
