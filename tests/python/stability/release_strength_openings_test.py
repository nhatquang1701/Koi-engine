"""The strength opening sample is fixed before any release match is played."""

from __future__ import annotations

import pathlib
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools" / "stability"))
sys.path.insert(0, str(ROOT / "tools" / "measurement"))

import koi_chess as chess  # noqa: E402
from sample_release_openings import sample_openings  # noqa: E402


class ReleaseStrengthOpeningsTest(unittest.TestCase):
    def test_iid_sample_is_deterministic_legal_and_has_160_named_draws(self):
        curated = ROOT / "tests" / "data" / "openings" / "openings-curated-32.txt"
        first = sample_openings(curated, sample_count=160, seed=20260926)
        second = sample_openings(curated, sample_count=160, seed=20260926)
        self.assertEqual(first, second)
        self.assertEqual(len(first), 160)
        self.assertEqual(len({name for name, _ in first}), 160)
        roots = set()
        sample_numbers = set()
        for name, moves in first:
            root, _, sample = name.rpartition("-sample-")
            self.assertIn(int(sample), range(1, 161))
            sample_numbers.add(int(sample))
            roots.add(root)
            board = chess.Board()
            for text in moves:
                move = chess.Move.from_uci(text)
                self.assertTrue(board.is_legal(move), f"{name}: {text}")
                board.push(move)
        self.assertGreaterEqual(len(roots), 25)
        self.assertEqual(sample_numbers, set(range(1, 161)))


if __name__ == "__main__":
    unittest.main()
