#!/usr/bin/env python3
"""Validate a small FEN/result corpus and emit deterministic eval metadata."""

from __future__ import annotations

import argparse
import csv
import hashlib
import pathlib
import re
import sys
from typing import Iterable


RESULTS = {"1-0": "1-0", "0-1": "0-1", "1/2-1/2": "1/2-1/2", "1": "1-0", "-1": "0-1", "0": "1/2-1/2", "win": "1-0", "loss": "0-1", "draw": "1/2-1/2"}
CANONICAL_HEADER = pathlib.Path(__file__).resolve().parents[1] / "src" / "koi" / "evaluation_parameters.hpp"


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


def generated_header(rows: Iterable[tuple[str, str]]) -> str:
    normalized = "\n".join(f"{fen},{result}" for fen, result in rows) + "\n"
    corpus_hash = hashlib.sha256(normalized.encode("utf-8")).hexdigest()
    parameter_version, parameters = canonical_parameters()
    lines = [
        "#pragma once",
        "",
        '#include "koi/evaluation_parameters.hpp"',
        "",
        "namespace koi {",
        "",
        f'inline constexpr std::string_view kTunedEvaluationParameterVersion = "{parameter_version}";',
        f'inline constexpr std::string_view kTunedEvaluationCorpusSha256 = "{corpus_hash}";',
        "",
    ]
    for name, value in parameters:
        lines.append(f"inline constexpr int kTunedEvaluation_{name} = {value};")
    lines.extend(["", "} // namespace koi", ""])
    return "\n".join(lines)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("corpus", nargs="?", type=pathlib.Path)
    parser.add_argument("-i", "--input", dest="input_path", type=pathlib.Path)
    parser.add_argument("-o", "--output", type=pathlib.Path, default=pathlib.Path("-"))
    args = parser.parse_args(argv)
    input_path = args.input_path or args.corpus
    if input_path is None:
        parser.error("a corpus path is required")
    try:
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
