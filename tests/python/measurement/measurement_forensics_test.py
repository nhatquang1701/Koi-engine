import hashlib
import json
import os
import sqlite3
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPOSITORY_ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPOSITORY_ROOT / "tools" / "measurement"))

import koi_chess as chess  # noqa: E402
PGN_FORENSICS_SCRIPT = REPOSITORY_ROOT / "tools" / "measurement" / "pgn_forensics.py"
GIGABASE_SCRIPT = REPOSITORY_ROOT / "tools" / "measurement" / "gigabase_extract.py"
FIXTURE_PATH = REPOSITORY_ROOT / "tests" / "data" / "positions" / "measurement-forensics.json"


def run_script(script: Path, *arguments: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(script), *arguments],
        capture_output=True,
        text=True,
        check=False,
    )


class MeasurementForensicsTests(unittest.TestCase):
    def test_pgn_corpus_is_deterministic_and_preserves_schema(self):
        pgn = '[Event "Forensics"]\n[Result "*"]\n\n1. e4 $1 {[%eval +0.25] note} (1. d4 d5) e5 *\n'
        with tempfile.TemporaryDirectory(prefix="koi pgn forensics ") as temporary_directory:
            temporary_path = Path(temporary_directory)
            pgn_directory = temporary_path / "inputs"
            pgn_directory.mkdir()
            (pgn_directory / "z game.pgn").write_text(pgn, encoding="utf-8")
            (pgn_directory / "a game.pgn").write_text(pgn.replace("e4", "d4", 1), encoding="utf-8")
            report_paths = [temporary_path / "first.json", temporary_path / "second.json"]

            results = [
                run_script(
                    PGN_FORENSICS_SCRIPT,
                    "--pgn-dir",
                    str(pgn_directory),
                    "--output",
                    str(report_path),
                )
                for report_path in report_paths
            ]

            self.assertTrue(all(result.returncode == 0 for result in results), results)
            self.assertTrue(all(result.stderr == "" for result in results), results)
            reports = [json.loads(path.read_text(encoding="utf-8")) for path in report_paths]

        self.assertEqual(reports[0]["schema"], "koi-pgn-forensics-v1")
        self.assertEqual(reports[0]["content_sha256"], reports[1]["content_sha256"])
        self.assertEqual(reports[0]["files"], reports[1]["files"])
        self.assertEqual([entry["path"] for entry in reports[0]["files"]], ["a game.pgn", "z game.pgn"])
        self.assertEqual(reports[0]["game_count"], 2)
        self.assertEqual(reports[0]["position_count"], 4)
        position = reports[0]["files"][1]["games"][0]["positions"][0]
        self.assertEqual(position["fen"], "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1")
        self.assertEqual(position["actual_move_san"], "e4")
        self.assertEqual(position["annotations"]["nags"], [1])
        self.assertEqual(position["annotations"]["eval"], "+0.25")

    def test_pgn_corpus_rejects_repository_local_output(self):
        with tempfile.TemporaryDirectory(prefix="koi pgn forensics ") as temporary_directory:
            pgn_directory = Path(temporary_directory)
            (pgn_directory / "fixture.pgn").write_text('[Result "*"]\n\n1. e4 *\n', encoding="utf-8")
            output_path = REPOSITORY_ROOT / "tests" / "data" / "forensics-report.json"

            completed = run_script(
                PGN_FORENSICS_SCRIPT,
                "--pgn-dir",
                str(pgn_directory),
                "--output",
                str(output_path),
            )

        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("under artifacts", completed.stderr)
        self.assertFalse(output_path.exists())

    def test_gigabase_sampling_is_read_only_deterministic_and_bounded(self):
        with tempfile.TemporaryDirectory(prefix="koi gigabase ") as temporary_directory:
            temporary_path = Path(temporary_directory)
            database_path = temporary_path / "gigabase.sqlite"
            output_paths = [temporary_path / "run-one", temporary_path / "run-two"]
            connection = sqlite3.connect(database_path)
            try:
                connection.execute("CREATE TABLE games (id INTEGER PRIMARY KEY, pgn TEXT NOT NULL)")
                connection.executemany(
                    "INSERT INTO games(id, pgn) VALUES (?, ?)",
                    [(index, f'[Event "Game {index}"]\n\n1. e4 e5 2. Nf3 *\n') for index in range(1, 13)],
                )
                connection.commit()
            finally:
                connection.close()
            database_hash = hashlib.sha256(database_path.read_bytes()).hexdigest()

            results = [
                run_script(
                    GIGABASE_SCRIPT,
                    "--database",
                    str(database_path),
                    "--output-dir",
                    str(output_path),
                    "--seed",
                    "17",
                    "--max-games",
                    "6",
                    "--max-positions",
                    "8",
                )
                for output_path in output_paths
            ]

            self.assertTrue(all(result.returncode == 0 for result in results), results)
            self.assertTrue(all(result.stderr == "" for result in results), results)
            self.assertEqual(database_hash, hashlib.sha256(database_path.read_bytes()).hexdigest())
            summaries = [json.loads((path / "summary.json").read_text(encoding="utf-8")) for path in output_paths]
            manifests = [
                {
                    split: json.loads((path / f"{split}.json").read_text(encoding="utf-8"))
                    for split in ("train", "validation", "holdout")
                }
                for path in output_paths
            ]

        self.assertEqual(summaries[0]["schema"], "koi-gigabase-manifest-v1")
        self.assertTrue(summaries[0]["source"]["read_only"])
        self.assertTrue(summaries[0]["source"]["query_only"])
        self.assertEqual(summaries[0]["content_sha256"], summaries[1]["content_sha256"])
        self.assertEqual(
            [manifests[0][split]["content_sha256"] for split in ("train", "validation", "holdout")],
            [manifests[1][split]["content_sha256"] for split in ("train", "validation", "holdout")],
        )
        records = [record for manifest in manifests[0].values() for record in manifest["records"]]
        self.assertEqual(len(records), len({record["source_id"] for record in records}))
        self.assertLessEqual(len(records), 6)
        self.assertLessEqual(sum(record["position_count"] for record in records), 8)

    def test_gigabase_sampling_rejects_repository_local_output(self):
        with tempfile.TemporaryDirectory(prefix="koi gigabase ") as temporary_directory:
            database_path = Path(temporary_directory) / "gigabase.sqlite"
            connection = sqlite3.connect(database_path)
            try:
                connection.execute("CREATE TABLE games (id INTEGER PRIMARY KEY, pgn TEXT NOT NULL)")
                connection.execute("INSERT INTO games(id, pgn) VALUES (1, '1. e4 *')")
                connection.commit()
            finally:
                connection.close()
            output_path = REPOSITORY_ROOT / "tests" / "data" / "gigabase-report"

            completed = run_script(
                GIGABASE_SCRIPT,
                "--database",
                str(database_path),
                "--output-dir",
                str(output_path),
            )

        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("under artifacts", completed.stderr)
        self.assertFalse(output_path.exists())

    def test_forensic_fixture_categories_are_legal(self):
        fixture = json.loads(FIXTURE_PATH.read_text(encoding="utf-8"))
        expected_categories = {
            "opening_drift",
            "missed_development",
            "pawn_break",
            "queen_rook_shuffling",
            "poisoned_capture",
            "fork",
            "king_attack",
        }

        self.assertEqual(fixture["schema"], "koi-measurement-forensics-v1")
        self.assertEqual({record["category"] for record in fixture["positions"]}, expected_categories)
        self.assertEqual(len(fixture["positions"]), len(expected_categories))
        self.assertEqual(
            len({record["id"] for record in fixture["positions"]}),
            len(fixture["positions"]),
        )
        for record in fixture["positions"]:
            board = chess.Board(record["fen"])
            self.assertTrue(board.is_valid(), record["id"])
            self.assertEqual(record["side"], "white" if board.turn else "black")


if __name__ == "__main__":
    unittest.main()
