"""Print Koi NNUE container provenance without loading an engine.

Usage:
    python tools/measurement/nnue_info.py artifacts/training/koi-v7.nnue [--json]

Supports the v5/v6/v8 header layout (magic, version, four layer sizes, four
shift bytes, quantization/feature-set strings, payload hash and size). Older
layouts print the magic and version plus the raw header length.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import struct
import sys

MAGIC = b"KOI-NNUE"
HEADER = struct.Struct("<8sIIIIIBBBBHHQ")
SUPPORTED_LAYOUTS = {5, 6, 8}


class NnueInfoError(RuntimeError):
    """Raised for unusable containers."""


def read_container(path: pathlib.Path) -> dict[str, object]:
    data = path.read_bytes()
    if len(data) < HEADER.size or data[:8] != MAGIC:
        raise NnueInfoError("not a Koi NNUE container")
    (magic, version, input_units, hidden, buckets, l1,
     hidden_shift, output_shift, l1_shift, l1_diff_shift,
     quantization_size, feature_set_size, payload_size) = HEADER.unpack_from(data, 0)
    info: dict[str, object] = {
        "path": str(path),
        "size_bytes": len(data),
        "magic": magic.decode(),
        "version": version,
        "input_units": input_units,
        "hidden": hidden,
        "output_buckets": buckets,
        "l1": l1,
        "hidden_shift": hidden_shift,
        "output_shift": output_shift,
        "l1_shift": l1_shift,
        "l1_diff_shift": l1_diff_shift,
        "quantization_size": quantization_size,
        "feature_set_size": feature_set_size,
        "payload_size": payload_size,
        "header_bytes": HEADER.size,
    }
    if version not in SUPPORTED_LAYOUTS:
        info["layout"] = "unsupported-for-detail"
        return info
    offset = HEADER.size
    if len(data) - offset < 32:
        raise NnueInfoError("container is truncated before the payload hash")
    info["network_sha256"] = data[offset:offset + 32].hex()
    offset += 32
    quantization = data[offset:offset + quantization_size].decode("ascii", "replace")
    offset += quantization_size
    feature_set = data[offset:offset + feature_set_size].decode("ascii", "replace")
    offset += feature_set_size
    payload = data[offset:offset + payload_size]
    if len(payload) != payload_size:
        raise NnueInfoError("container is truncated inside the payload")
    info["quantization"] = quantization
    info["feature_set"] = feature_set
    info["payload_sha256"] = hashlib.sha256(payload).hexdigest()
    info["gpu_ready"] = version == 8
    return info


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("container", type=pathlib.Path)
    parser.add_argument("--json", action="store_true", help="emit machine-readable JSON")
    arguments = parser.parse_args(argv)
    try:
        info = read_container(arguments.container)
    except (OSError, NnueInfoError) as error:
        print(f"nnue_info: {error}", file=sys.stderr)
        return 1
    if arguments.json:
        print(json.dumps(info, indent=2))
    else:
        for key, value in info.items():
            print(f"{key}: {value}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
