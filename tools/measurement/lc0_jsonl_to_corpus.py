"""Convert gen_lc0_labels.py JSONL output into flat `FEN;cp` training corpora.

Each JSONL record carries the Lc0 teacher's value (``score_cp``, side-to-move
perspective) plus the per-move visit/policy detail that stays in the JSONL as
the policy archive.  This converter emits the flat ``FEN;cp`` corpus that
``train_nnue_koi.py`` consumes directly, applying the same two conventions as
``gen_training_data.py``: positions whose |cp| exceeds the cap are skipped, and
duplicate positions are dropped (key = first four FEN fields).

Usage:
  python tools/measurement/lc0_jsonl_to_corpus.py \
      --input artifacts/training/lc0-curriculum-20261004/labels-general.jsonl \
      --input artifacts/training/lc0-curriculum-20261004/labels-tactical.jsonl \
      --output artifacts/training/lc0-curriculum-20261004/corpus.txt
"""

from __future__ import annotations

import argparse
import json
import time
from pathlib import Path


def log(message: str) -> None:
    print(f"[{time.strftime('%H:%M:%S')}] {message}", flush=True)


def position_key(fen: str) -> str:
    """First four FEN fields: placement, side, castling, en passant."""
    return " ".join(fen.split(" ")[:4])


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", action="append", required=True,
                        help="JSONL file from gen_lc0_labels.py (repeatable)")
    parser.add_argument("--output", required=True, help="flat FEN;cp corpus")
    parser.add_argument("--max-abs-cp", type=float, default=4000.0,
                        help="skip positions whose |cp| exceeds this cap")
    parser.add_argument("--no-dedupe", action="store_true")
    args = parser.parse_args()

    out = Path(args.output)
    out.parent.mkdir(parents=True, exist_ok=True)

    seen: set[str] = set()
    total = written = skipped_cp = skipped_dup = bad = 0
    with out.open("w", encoding="utf-8", newline="\n") as fh:
        for src in args.input:
            for line in Path(src).open("r", encoding="utf-8"):
                line = line.strip()
                if not line:
                    continue
                total += 1
                try:
                    record = json.loads(line)
                except json.JSONDecodeError:
                    bad += 1
                    continue
                fen = record.get("fen")
                cp = record.get("score_cp")
                if not fen or cp is None:
                    bad += 1
                    continue
                if abs(cp) > args.max_abs_cp:
                    skipped_cp += 1
                    continue
                key = position_key(fen)
                if not args.no_dedupe:
                    if key in seen:
                        skipped_dup += 1
                        continue
                    seen.add(key)
                fh.write(f"{fen};{int(round(cp))}\n")
                written += 1
                if written % 100000 == 0:
                    log(f"{written} rows written")
    log(f"done: total={total} written={written} "
        f"skipped_cp={skipped_cp} skipped_dup={skipped_dup} bad={bad}")


if __name__ == "__main__":
    main()
