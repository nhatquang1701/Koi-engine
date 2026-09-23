#!/bin/sh
# Launcher for the Koi NNUE Studio GUI on Linux.
set -e
KOI_REPO=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
KOI_STUDIO="$KOI_REPO/tools/nnue/koi_nnue_studio.py"

if command -v python3 >/dev/null 2>&1; then
    PYTHON=python3
elif command -v python >/dev/null 2>&1; then
    PYTHON=python
else
    echo "koi-nnue-studio: python3 is required to run the studio" >&2
    exit 1
fi

exec "$PYTHON" "$KOI_STUDIO" "$@"
