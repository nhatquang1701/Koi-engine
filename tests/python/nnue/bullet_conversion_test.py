"""Regression tests for the Bullet-to-Koi corpus perspective conversion.

The Bullet text format stores White-relative scores; Koi's trainer and engine
treat a corpus score as side-to-move relative.  The v5 official corpus was
built without the conversion, so every Black-to-move row trained a
sign-inverted target and the network's evaluations became unusable.  These
tests pin the conversion contract used by
``tools/measurement/convert_bullet_corpus.py``.
"""

from __future__ import annotations

import pathlib
import sys
import tempfile
import unittest

import chess

REPO_ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO_ROOT / "tools" / "measurement"))

from convert_bullet_corpus import convert_file, convert_line  # noqa: E402

STARTPOS = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR"


class BulletConversionTests(unittest.TestCase):
    def test_white_to_move_score_is_unchanged(self) -> None:
        row = f"{STARTPOS} w KQkq - 0 1 | 42 | 0.5"
        self.assertEqual(convert_line(row), f"{STARTPOS} w KQkq - 0 1;42")

    def test_black_to_move_score_is_negated(self) -> None:
        row = f"{STARTPOS} b KQkq - 0 1 | 42 | 0.5"
        self.assertEqual(convert_line(row), f"{STARTPOS} b KQkq - 0 1;-42")

    def test_mirrored_pair_converts_to_the_same_side_to_move_score(self) -> None:
        # A position and its colour mirror describe the same evaluation from
        # the mover's point of view: Bullet stores +55 for White to move and
        # -55 for the mirrored Black-to-move board, and both must convert to
        # +55 after the side-to-move flip.
        board = chess.Board("r1bqkbnr/pppp1ppp/2n5/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R w KQkq - 0 3")
        mirror = board.mirror()
        self.assertEqual(board.turn, chess.WHITE)
        self.assertEqual(mirror.turn, chess.BLACK)
        white_row = f"{board.fen()} | 55 | 0.5"
        mirror_row = f"{mirror.fen()} | -55 | 0.5"
        self.assertEqual(convert_line(white_row).split(";")[1], "55")
        self.assertEqual(convert_line(mirror_row).split(";")[1], "55")

    def test_blank_and_malformed_rows_are_skipped(self) -> None:
        self.assertIsNone(convert_line(""))
        self.assertIsNone(convert_line("   "))
        self.assertIsNone(convert_line("not a bullet row"))

    def test_convert_file_writes_side_to_move_scores(self) -> None:
        rows = [
            f"{STARTPOS} w KQkq - 0 1 | 10 | 1.0",
            f"{STARTPOS} b KQkq - 0 1 | 10 | 1.0",
            "",
        ]
        with tempfile.TemporaryDirectory() as tmp:
            source = pathlib.Path(tmp) / "source.txt"
            destination = pathlib.Path(tmp) / "corpus.txt"
            source.write_text("\n".join(rows) + "\n", encoding="utf-8")
            written = convert_file(source, destination, progress_every=0)
            self.assertEqual(written, 2)
            self.assertEqual(
                destination.read_text(encoding="utf-8").splitlines(),
                [f"{STARTPOS} w KQkq - 0 1;10", f"{STARTPOS} b KQkq - 0 1;-10"],
            )


if __name__ == "__main__":
    unittest.main()
