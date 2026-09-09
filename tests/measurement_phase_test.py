import json
import sqlite3
import tempfile
import unittest
from pathlib import Path

from tools import elo_estimate, elo_oracle, gigabase_extract


OPENING_LINES = (
    "e4 e5 2. Nf3 Nc6", "d4 d5 2. c4 e6", "c4 e5 2. Nc3 Nf6",
    "Nf3 d5 2. g3 c5", "e4 c5 2. Nf3 d6", "e4 e6 2. d4 d5",
    "e4 c6 2. d4 d5", "d4 Nf6 2. c4 g6", "d4 Nf6 2. c4 e6",
    "c4 c5 2. Nc3 Nc6", "Nf3 Nf6 2. g3 g6", "e4 d5 2. exd5 Qxd5",
    "d4 d5 2. Nf3 Nf6", "e4 g6 2. d4 Bg7", "e4 b6 2. d4 Bb7",
    "d4 f5 2. e4 fxe4", "c4 Nf6 2. Nc3 e5", "g3 d5 2. Bg2 Nf6",
    "b3 d5 2. Bb2 Nf6", "f4 d5 2. Nf3 Nf6",
)


def _pgn(index: int, result: str = "1/2-1/2") -> str:
    return (
        f'[Event "GigaBase {index}"]\n'
        f'[Result "{result}"]\n\n'
        f"1. {OPENING_LINES[index - 1]} {result}\n"
    )


def _create_games_database(path: Path) -> None:
    connection = sqlite3.connect(path)
    try:
        connection.execute(
            "CREATE TABLE games (id INTEGER PRIMARY KEY, result TEXT NOT NULL, pgn TEXT NOT NULL)"
        )
        results = ("1-0", "0-1", "1/2-1/2")
        rows = [
            (index, results[index % len(results)], _pgn(index, results[index % len(results)]))
            for index in range(1, 21)
        ]
        connection.executemany("INSERT INTO games(id, result, pgn) VALUES (?, ?, ?)", rows)
        connection.execute(
            "INSERT INTO games(id, result, pgn) VALUES (?, ?, ?)",
            (999, rows[0][1], rows[0][2]),
        )
        connection.commit()
    finally:
        connection.close()


class GigaBaseMeasurementTests(unittest.TestCase):
    def test_gigabase_decodes_deduplicates_stratifies_and_is_reproducible(self):
        with tempfile.TemporaryDirectory(prefix="koi gigabase phase ") as temporary_directory:
            directory = Path(temporary_directory)
            database = directory / "games.sqlite"
            _create_games_database(database)

            first = gigabase_extract.sample_database(
                database, seed=240906, max_games=20, max_positions=200
            )
            second = gigabase_extract.sample_database(
                database, seed=240906, max_games=20, max_positions=200
            )

        self.assertTrue(first["source"]["query_only"])
        self.assertTrue(first["source"]["read_only"])
        self.assertEqual(first["sampling"]["deduplicated_games"], 20)
        self.assertEqual(first["sampling"]["selected_games"], 20)
        self.assertEqual(first["sampling"]["split_ratios"], {"train": 0.70, "validation": 0.15, "holdout": 0.15})
        self.assertEqual(
            {split: len(records) for split, records in first["splits"].items()},
            {"train": 14, "validation": 3, "holdout": 3},
        )
        records = [record for records in first["splits"].values() for record in records]
        self.assertEqual(len(records), len({record["deduplication_key"] for record in records}))
        self.assertEqual(
            first["sampling"]["selected_legal_positions"],
            sum(record["legal_position_count"] for record in records),
        )
        self.assertLessEqual(first["sampling"]["selected_legal_positions"], 200)
        self.assertTrue(all(record["decode"]["status"] == "decoded" for record in records))
        self.assertTrue(all(record["positions"] for record in records))
        self.assertEqual(
            first["sampling"]["content_sha256"], second["sampling"]["content_sha256"]
        )

    def test_gigabase_malformed_move_decoding_falls_back_without_breaking_caps(self):
        with tempfile.TemporaryDirectory(prefix="koi gigabase malformed ") as temporary_directory:
            directory = Path(temporary_directory)
            database = directory / "games.sqlite"
            connection = sqlite3.connect(database)
            try:
                connection.execute("CREATE TABLE games (id INTEGER PRIMARY KEY, pgn TEXT NOT NULL)")
                connection.executemany(
                    "INSERT INTO games(id, pgn) VALUES (?, ?)",
                    [(1, _pgn(1, "1-0")), (2, '[Result "1-0"]\n\n1. e4 e9 2. Nf3 *')],
                )
                connection.commit()
            finally:
                connection.close()

            sample = gigabase_extract.sample_database(
                database, seed=1, max_games=2, max_positions=10
            )

        records = sample["records"]
        malformed = next(record for record in records if record["source_id"] == "2")
        self.assertEqual(malformed["decode"]["status"], "fallback")
        self.assertTrue(malformed["decode"]["reason"])
        self.assertEqual(malformed["positions"], [])
        self.assertEqual(malformed["legal_position_count"], 0)
        self.assertLessEqual(sample["sampling"]["selected_legal_positions"], 10)

    def test_gigabase_caps_decoded_legal_positions_when_source_count_understates(self):
        with tempfile.TemporaryDirectory(prefix="koi gigabase understated count ") as temporary_directory:
            directory = Path(temporary_directory)
            database = directory / "games.sqlite"
            connection = sqlite3.connect(database)
            try:
                connection.execute(
                    "CREATE TABLE games (id INTEGER PRIMARY KEY, position_count INTEGER, pgn TEXT NOT NULL)"
                )
                connection.executemany(
                    "INSERT INTO games(id, position_count, pgn) VALUES (?, ?, ?)",
                    [
                        (1, 1, _pgn(1, "1-0")),
                        (2, 1, _pgn(2, "0-1")),
                        (3, 1, _pgn(3, "1/2-1/2")),
                    ],
                )
                connection.commit()
            finally:
                connection.close()

            sample = gigabase_extract.sample_database(
                database, seed=7, max_games=3, max_positions=3
            )

        self.assertLessEqual(sample["sampling"]["selected_legal_positions"], 3)
        self.assertEqual(
            sample["sampling"]["selected_legal_positions"],
            sum(record["legal_position_count"] for record in sample["records"]),
        )


class OracleMeasurementMetadataTests(unittest.TestCase):
    def test_extraction_report_declares_measurement_state_and_position_classification(self):
        pgn = '[Result "*"]\n\n1. e4 *\n'
        games = elo_oracle.extract_games(pgn)
        report = elo_oracle.build_extraction_report(
            Path("external-input.pgn"), pgn, games, movetime_ms=25, threads=4
        )

        self.assertEqual(report["measurement"]["network"]["state"], "disabled")
        self.assertEqual(report["measurement"]["book"]["state"], "disabled")
        self.assertEqual(report["measurement"]["tablebase"]["state"], "disabled")
        self.assertEqual(report["measurement"]["time_control"]["kind"], "movetime")
        self.assertGreaterEqual(report["hardware"]["cpu_count"], 1)
        classification = report["games"][0]["positions"][0]["position_classification"]
        self.assertEqual(classification["side_to_move"], "white")
        self.assertEqual(classification["phase"], "opening")
        self.assertGreater(classification["piece_count"], 0)


class EloScheduleMeasurementTests(unittest.TestCase):
    def test_schedule_export_import_preserves_paired_openings_for_before_after_runs(self):
        anchors = tuple(
            elo_estimate.Anchor(
                f"stockfish-{rating}", Path(f"C:/{rating}.exe"), rating, "test", rating
            )
            for rating in (1400, 1600, 1800)
        )
        openings = tuple(f"opening-{index:02d}" for index in range(1, 33))
        schedule = elo_estimate.plan_schedule(
            anchors, prior_elo=1500, target_games=320, opening_names=openings
        )

        with tempfile.TemporaryDirectory(prefix="koi elo schedule ") as temporary_directory:
            schedule_path = Path(temporary_directory) / "paired-schedule.json"
            exported = elo_estimate.export_schedule(
                schedule_path,
                schedule,
                prior_elo=1500,
                opening_names=openings,
                run_labels=("before", "after"),
            )
            imported = elo_estimate.import_schedule(
                schedule_path,
                anchors,
                expected_openings=openings,
                prior_elo=1500,
            )
            saved = json.loads(schedule_path.read_text(encoding="utf-8"))

        self.assertEqual(exported["schema"], "koi-elo-schedule-v1")
        self.assertEqual(saved["pairing"]["run_labels"], ["before", "after"])
        self.assertEqual(sum(batch.games for batch in schedule), 320)
        self.assertEqual(schedule, imported)
        self.assertEqual(
            [batch.opening_names for batch in imported], [openings] * len(imported)
        )


if __name__ == "__main__":
    unittest.main()
