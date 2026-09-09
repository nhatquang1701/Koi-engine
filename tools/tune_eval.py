#!/usr/bin/env python3
"""Validate classical corpora or export a deterministic opt-in Koi NNUE network."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import pathlib
import re
import struct
import sys
from typing import Any, Iterable, Mapping


RESULTS = {"1-0": "1-0", "0-1": "0-1", "1/2-1/2": "1/2-1/2", "1": "1-0", "-1": "0-1", "0": "1/2-1/2", "win": "1-0", "loss": "0-1", "draw": "1/2-1/2"}
CANONICAL_HEADER = pathlib.Path(__file__).resolve().parents[1] / "src" / "koi" / "evaluation_parameters.hpp"
NNUE_MAGIC = b"KOI-NNUE"
NNUE_FORMAT_VERSION = 2
NNUE_QUANTIZATION = b"int16/int8"
NNUE_FEATURE_SET = b"piece-square-king-pawn-v2"
NNUE_ARCHITECTURE = (960, 256, 32, 1)
NNUE_HYPERPARAMETERS = {
    "epochs": 1,
    "batch_size": 64,
    "learning_rate": 0.01,
    "quantization_scale": 32,
}


def canonical_parameters(header_path: pathlib.Path = CANONICAL_HEADER) -> tuple[str, list[tuple[str, int]]]:
    source = header_path.read_text(encoding="utf-8")
    version_match = re.search(r'string_view\s+version\s*=\s*"([^"]+)"', source)
    fields = [(name, int(value)) for name, value in re.findall(
        r"\bint\s+(\w+)\s*=\s*(-?\d+);", source
    )]
    if version_match is None or not fields:
        raise ValueError("canonical evaluation header has no versioned integer parameters")
    return version_match.group(1), fields


def fallback_valid_fen(fen: str) -> bool:
    fields = fen.split()
    if len(fields) != 6 or fields[1] not in {"w", "b"}:
        return False
    ranks = fields[0].split("/")
    if len(ranks) != 8:
        return False
    kings = {"K": 0, "k": 0}
    for rank in ranks:
        width = 0
        for character in rank:
            if character.isdigit() and character in "12345678":
                width += int(character)
            elif character in "PNBRQKpnbrqk":
                width += 1
                if character in kings:
                    kings[character] += 1
            else:
                return False
        if width != 8:
            return False
    if kings["K"] != 1 or kings["k"] != 1:
        return False
    if fields[2] != "-" and any(character not in "KQkq" for character in fields[2]):
        return False
    if fields[3] != "-" and (len(fields[3]) != 2 or fields[3][0] not in "abcdefgh" or fields[3][1] not in "36"):
        return False
    return fields[4].isdigit() and fields[5].isdigit()


def valid_fen(fen: str) -> bool:
    try:
        import chess  # type: ignore
    except ImportError:
        return fallback_valid_fen(fen)
    try:
        return chess.Board(fen).is_valid()
    except ValueError:
        return False


def parse_rows(text: str) -> list[tuple[str, str]]:
    rows: list[tuple[str, str]] = []
    for raw_line in text.splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        if "," in line or "\t" in line:
            dialect = csv.excel_tab if "\t" in line and "," not in line else csv.excel
            parsed = next(csv.reader([line], dialect=dialect))
            if len(parsed) != 2:
                raise ValueError("each corpus row must contain FEN and result")
            fen, result = (value.strip() for value in parsed)
        elif "|" in line:
            fen, result = (value.strip() for value in line.split("|", 1))
        else:
            try:
                fen, result = line.rsplit(None, 1)
            except ValueError as error:
                raise ValueError("each corpus row must contain FEN and result") from error
        if fen.lower() == "fen" and result.lower() == "result":
            continue
        normalized_result = RESULTS.get(result.lower())
        if not normalized_result:
            raise ValueError(f"invalid result: {result}")
        fen = " ".join(fen.split())
        if not valid_fen(fen):
            raise ValueError(f"invalid FEN: {fen}")
        rows.append((fen, normalized_result))
    if not rows:
        raise ValueError("corpus must contain at least one data row")
    return rows


def canonical_corpus_text(rows: Iterable[tuple[str, str]]) -> str:
    return "\n".join(f"{fen},{result}" for fen, result in rows) + "\n"


def corpus_sha256(rows: Iterable[tuple[str, str]]) -> str:
    return hashlib.sha256(canonical_corpus_text(rows).encode("utf-8")).hexdigest()


def _pack_int16(values: Iterable[int]) -> bytes:
    return b"".join(struct.pack("<h", value) for value in values)


def _pack_int32(values: Iterable[int]) -> bytes:
    return b"".join(struct.pack("<i", value) for value in values)


def _pack_int8(values: Iterable[int]) -> bytes:
    return b"".join(struct.pack("<b", value) for value in values)


def nnue_payload(
    feature_weights: Iterable[int],
    hidden_bias: Iterable[int],
    bottleneck_weights: Iterable[int],
    bottleneck_bias: Iterable[int],
    output_weights: Iterable[int],
    output_bias: int,
) -> bytes:
    return b"".join(
        (
            _pack_int16(feature_weights),
            _pack_int32(hidden_bias),
            _pack_int8(bottleneck_weights),
            _pack_int32(bottleneck_bias),
            _pack_int8(output_weights),
            struct.pack("<i", output_bias),
        )
    )


def nnue_container(payload: bytes) -> tuple[bytes, str]:
    payload_hash = hashlib.sha256(payload).hexdigest()
    header = struct.pack(
        "<8sI4IHHQ",
        NNUE_MAGIC,
        NNUE_FORMAT_VERSION,
        *NNUE_ARCHITECTURE,
        len(NNUE_QUANTIZATION),
        len(NNUE_FEATURE_SET),
        len(payload),
    )
    encoded = (
        header
        + bytes.fromhex(payload_hash)
        + NNUE_QUANTIZATION
        + NNUE_FEATURE_SET
        + payload
    )
    return encoded, payload_hash


def synthetic_nnue_payload(seed: int) -> bytes:
    input_units, hidden_units, bottleneck_units, _ = NNUE_ARCHITECTURE
    feature_weights = [
        ((index * 31 + seed * 7) % 7) - 3
        for index in range(input_units * hidden_units)
    ]
    hidden_bias = [((index * 13 + seed * 5) % 9) - 4 for index in range(hidden_units)]
    bottleneck_weights = [
        ((index * 17 + seed * 3) % 5) - 2
        for index in range(hidden_units * bottleneck_units)
    ]
    bottleneck_bias = [
        ((index * 19 + seed * 11) % 9) - 4 for index in range(bottleneck_units)
    ]
    output_weights = [((index * 23 + seed) % 5) - 2 for index in range(bottleneck_units)]
    output_bias = (seed % 9) - 4
    return nnue_payload(
        feature_weights,
        hidden_bias,
        bottleneck_weights,
        bottleneck_bias,
        output_weights,
        output_bias,
    )


def _parse_board_for_nnue(fen: str) -> list[str | None]:
    board: list[str | None] = [None] * 64
    ranks = fen.split()[0].split("/")
    for row, encoded_rank in enumerate(ranks):
        file_index = 0
        for character in encoded_rank:
            if character.isdigit():
                file_index += int(character)
                continue
            board[(7 - row) * 8 + file_index] = character
            file_index += 1
    return board


def nnue_v2_features(fen: str) -> list[int]:
    board = _parse_board_for_nnue(fen)
    encoded = [0] * NNUE_ARCHITECTURE[0]
    counts = [[0] * 8 for _ in range(2)]
    pawns: list[list[int]] = [[], []]
    piece_offsets = {"p": 0, "n": 1, "b": 2, "r": 3, "q": 4}
    kings: list[int | None] = [None, None]

    for square, piece in enumerate(board):
        if piece is None:
            continue
        color = 0 if piece.isupper() else 1
        piece_type = piece.lower()
        if piece_type == "k":
            kings[color] = square
            continue
        if piece_type in piece_offsets:
            encoded[(color * 6 + piece_offsets[piece_type]) * 64 + square] = 1
        if piece_type == "p":
            file_index = square % 8
            counts[color][file_index] += 1
            pawns[color].append(square)

    for color, king_square in enumerate(kings):
        if king_square is not None:
            encoded[768 + color * 64 + king_square] = 1

    for color in range(2):
        enemy = 1 - color
        for file_index in range(8):
            file_pawns = [square for square in pawns[color] if square % 8 == file_index]
            if not file_pawns:
                continue
            adjacent = any(
                counts[color][adjacent_file] > 0
                for adjacent_file in (file_index - 1, file_index + 1)
                if 0 <= adjacent_file < 8
            )
            isolated = False
            passed = False
            for square in file_pawns:
                rank = square // 8
                if not adjacent:
                    isolated = True
                enemy_ahead = any(
                    abs(enemy_square % 8 - file_index) <= 1
                    and (enemy_square // 8 > rank if color == 0 else enemy_square // 8 < rank)
                    for enemy_square in pawns[enemy]
                )
                if not enemy_ahead:
                    passed = True
            base = 896 + color * 32 + file_index * 4
            encoded[base] = 1
            encoded[base + 1] = int(counts[color][file_index] >= 2)
            encoded[base + 2] = int(isolated)
            encoded[base + 3] = int(passed)
    return encoded


def torch_nnue_payload(rows: list[tuple[str, str]], seed: int, device_name: str) -> bytes:
    try:
        import torch  # type: ignore
    except ImportError as error:
        raise ValueError("PyTorch is required for --backend torch; use --backend synthetic") from error

    if device_name == "cuda" and not torch.cuda.is_available():
        raise ValueError("--device cuda requested but PyTorch reports no CUDA device")
    device = torch.device("cuda" if device_name == "cuda" else "cpu")
    torch.manual_seed(seed)
    if device.type == "cuda":
        torch.cuda.manual_seed_all(seed)
    torch.use_deterministic_algorithms(True)
    model = torch.nn.Sequential(
        torch.nn.Linear(960, 256),
        torch.nn.ReLU(),
        torch.nn.Linear(256, 32),
        torch.nn.ReLU(),
        torch.nn.Linear(32, 1),
    ).to(device)
    inputs = torch.tensor(
        [nnue_v2_features(fen) for fen, _ in rows],
        dtype=torch.float32,
        device=device,
    )
    label_values = {"1-0": 1.0, "0-1": -1.0, "1/2-1/2": 0.0}
    labels = torch.tensor(
        [[label_values[result]] for _, result in rows],
        dtype=torch.float32,
        device=device,
    )
    optimizer = torch.optim.SGD(model.parameters(), lr=NNUE_HYPERPARAMETERS["learning_rate"])
    loss_function = torch.nn.MSELoss()
    for _ in range(NNUE_HYPERPARAMETERS["epochs"]):
        optimizer.zero_grad(set_to_none=True)
        loss = loss_function(model(inputs), labels)
        loss.backward()
        optimizer.step()

    scale = NNUE_HYPERPARAMETERS["quantization_scale"]
    state = model.state_dict()
    first = state["0.weight"].detach().cpu().tolist()
    first_bias = state["0.bias"].detach().cpu().tolist()
    second = state["2.weight"].detach().cpu().tolist()
    second_bias = state["2.bias"].detach().cpu().tolist()
    output = state["4.weight"].detach().cpu().tolist()[0]
    output_bias = state["4.bias"].detach().cpu().tolist()[0]

    def quantized(value: float, lower: int, upper: int) -> int:
        return max(lower, min(upper, int(round(value * scale))))

    feature_weights = [
        quantized(first[hidden][feature], -32768, 32767)
        for feature in range(960)
        for hidden in range(256)
    ]
    hidden_bias = [quantized(value, -2147483648, 2147483647) for value in first_bias]
    bottleneck_weights = [
        quantized(second[bottleneck][hidden], -128, 127)
        for hidden in range(256)
        for bottleneck in range(32)
    ]
    bottleneck_bias = [quantized(value, -2147483648, 2147483647) for value in second_bias]
    output_weights = [quantized(value, -128, 127) for value in output]
    return nnue_payload(
        feature_weights,
        hidden_bias,
        bottleneck_weights,
        bottleneck_bias,
        output_weights,
        quantized(output_bias, -2147483648, 2147483647),
    )


def _nnue_split_paths(args: argparse.Namespace) -> dict[str, pathlib.Path]:
    explicit = {name: getattr(args, name) for name in ("train", "validation", "holdout")}
    if args.manifest is not None:
        if any(path is not None for path in explicit.values()):
            raise ValueError("--manifest cannot be combined with --train/--validation/--holdout")
        manifest_path = args.manifest
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        values = manifest.get("splits", manifest)
        try:
            explicit = {
                name: pathlib.Path(values[name])
                for name in ("train", "validation", "holdout")
            }
        except (KeyError, TypeError) as error:
            raise ValueError("NNUE manifest must contain train, validation, and holdout paths") from error
        explicit = {
            name: path if path.is_absolute() else manifest_path.parent / path
            for name, path in explicit.items()
        }
    elif not all(path is not None for path in explicit.values()):
        raise ValueError("NNUE export requires --manifest or all three split corpus paths")
    return {name: path for name, path in explicit.items() if path is not None}


def nnue_main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Export a deterministic Koi NNUE network")
    parser.add_argument("--manifest", type=pathlib.Path)
    parser.add_argument("--train", type=pathlib.Path)
    parser.add_argument("--validation", type=pathlib.Path)
    parser.add_argument("--holdout", type=pathlib.Path)
    parser.add_argument("--output-network", type=pathlib.Path, required=True)
    parser.add_argument("--output-metadata", type=pathlib.Path)
    parser.add_argument("--seed", type=int, default=0)
    parser.add_argument("--backend", choices=("synthetic", "torch", "auto"), default="synthetic")
    parser.add_argument("--device", choices=("cpu", "cuda", "auto"), default="cpu")
    args = parser.parse_args(argv)
    try:
        split_paths = _nnue_split_paths(args)
        splits = {
            name: parse_rows(path.read_text(encoding="utf-8"))
            for name, path in split_paths.items()
        }
        backend = args.backend
        device = args.device
        optional_dependencies: dict[str, str] = {}
        if backend == "auto":
            backend = "synthetic"
        if backend == "torch":
            try:
                import torch  # type: ignore
            except ImportError as error:
                raise ValueError(
                    "PyTorch is required for --backend torch; use --backend synthetic"
                ) from error
            optional_dependencies["torch"] = torch.__version__
            if device == "auto":
                device = "cuda" if torch.cuda.is_available() else "cpu"
            payload = torch_nnue_payload(splits["train"], args.seed, device)
        else:
            if device == "auto":
                device = "cpu"
            payload = synthetic_nnue_payload(args.seed)
        network, payload_hash = nnue_container(payload)
        output_network = args.output_network
        output_network.parent.mkdir(parents=True, exist_ok=True)
        output_network.write_bytes(network)
        metadata_path = args.output_metadata or output_network.with_suffix(
            output_network.suffix + ".json"
        )
        metadata_path.parent.mkdir(parents=True, exist_ok=True)
        metadata: dict[str, Any] = {
            "schema": "koi-nnue-training-metadata-v1",
            "feature_set": NNUE_FEATURE_SET.decode("ascii"),
            "architecture": list(NNUE_ARCHITECTURE),
            "quantization": NNUE_QUANTIZATION.decode("ascii"),
            "activation": "clipped-relu",
            "seed": args.seed,
            "backend": backend,
            "device": device,
            "hyperparameters": dict(NNUE_HYPERPARAMETERS),
            "split_hashes": {name: corpus_sha256(rows) for name, rows in splits.items()},
            "split_counts": {name: len(rows) for name, rows in splits.items()},
            "corpus_sha256": corpus_sha256(splits["train"]),
            "payload_sha256": payload_hash,
            "network_sha256": hashlib.sha256(network).hexdigest(),
            "command": [sys.executable, str(pathlib.Path(__file__).resolve()), *argv],
            "optional_dependencies": optional_dependencies,
        }
        metadata_path.write_text(
            json.dumps(metadata, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
            newline="\n",
        )
    except (OSError, ValueError, json.JSONDecodeError, RuntimeError) as error:
        print(f"tune_eval.py nnue: {error}", file=sys.stderr)
        return 2
    return 0


def generated_header(
    rows: Iterable[tuple[str, str]],
    splits: Mapping[str, list[tuple[str, str]]] | None = None,
) -> str:
    train_rows = list(rows)
    split_rows = {
        "train": train_rows,
        "validation": [],
        "holdout": [],
    }
    if splits is not None:
        split_rows.update({name: list(values) for name, values in splits.items()})
    parameter_version, parameters = canonical_parameters()
    lines = [
        "#pragma once",
        "",
        "#include <cstddef>",
        "#include <string_view>",
        '#include "koi/evaluation_parameters.hpp"',
        "",
        "namespace koi {",
        "",
        f'inline constexpr std::string_view kTunedEvaluationParameterVersion = "{parameter_version}";',
        f'inline constexpr std::string_view kTunedEvaluationCorpusSha256 = "{corpus_sha256(train_rows)}";',
        'inline constexpr std::string_view kTunedEvaluationMetadataFormatVersion = "koi-evaluation-parameters-v1";',
        "",
    ]
    for name in ("train", "validation", "holdout"):
        capitalized = name.capitalize()
        values = split_rows[name]
        lines.append(
            f'inline constexpr std::string_view kTunedEvaluation{capitalized}CorpusSha256 = "{corpus_sha256(values)}";'
        )
        lines.append(
            f"inline constexpr std::size_t kTunedEvaluation{capitalized}PositionCount = {len(values)};"
        )
    lines.append("")
    lines.extend([
        "inline constexpr EvaluationParameterMetadata kTunedEvaluationParameterMetadata{",
        '    "koi-evaluation-parameters-v1",',
        f'    "{parameter_version}",',
        '    kTunedEvaluationTrainCorpusSha256,',
        '    kTunedEvaluationValidationCorpusSha256,',
        '    kTunedEvaluationHoldoutCorpusSha256,',
        '    kTunedEvaluationTrainPositionCount,',
        '    kTunedEvaluationValidationPositionCount,',
        '    kTunedEvaluationHoldoutPositionCount,',
        "};",
        "",
    ])
    for name, value in parameters:
        lines.append(f"inline constexpr int kTunedEvaluation_{name} = {value};")
    lines.extend(["", "} // namespace koi", ""])
    return "\n".join(lines)


def main(argv: list[str]) -> int:
    if argv and argv[0] == "nnue":
        return nnue_main(argv[1:])
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("corpus", nargs="?", type=pathlib.Path)
    parser.add_argument("-i", "--input", dest="input_path", type=pathlib.Path)
    parser.add_argument("--train", type=pathlib.Path)
    parser.add_argument("--validation", type=pathlib.Path)
    parser.add_argument("--holdout", type=pathlib.Path)
    parser.add_argument("-o", "--output", type=pathlib.Path, default=pathlib.Path("-"))
    args = parser.parse_args(argv)
    try:
        split_paths = {
            "train": args.train,
            "validation": args.validation,
            "holdout": args.holdout,
        }
        if any(path is not None for path in split_paths.values()):
            if not all(path is not None for path in split_paths.values()):
                parser.error("--train, --validation, and --holdout must be provided together")
            if args.input_path is not None or args.corpus is not None:
                parser.error("split corpora cannot be combined with a positional or --input corpus")
            splits = {
                name: parse_rows(path.read_text(encoding="utf-8"))
                for name, path in split_paths.items()
                if path is not None
            }
            output = generated_header(splits["train"], splits)
        else:
            input_path = args.input_path or args.corpus
            if input_path is None:
                parser.error("a corpus path is required")
            corpus_text = sys.stdin.read() if str(input_path) == "-" else input_path.read_text(encoding="utf-8")
            rows = parse_rows(corpus_text)
            output = generated_header(rows)
        if str(args.output) == "-":
            sys.stdout.write(output)
        else:
            args.output.write_text(output, encoding="utf-8", newline="\n")
    except (OSError, ValueError) as error:
        print(f"tune_eval.py: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
