import hashlib
import json
import os
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

import tools.measurement.elo_oracle as elo_oracle


REPOSITORY_ROOT = Path(__file__).resolve().parents[3]
ORACLE_SCRIPT = REPOSITORY_ROOT / "tools" / "measurement" / "elo_oracle.py"
MATCH_SCRIPT = REPOSITORY_ROOT / "tools" / "measurement" / "stockfish_match.py"
BOOK_AUDIT_SCRIPT = REPOSITORY_ROOT / "tools" / "measurement" / "book_audit.py"


FAKE_ENGINE_SOURCE = textwrap.dedent(
    """
    import sys

    role = sys.argv[1]
    log_path = sys.argv[2]
    own_book = False
    side_to_move = "w"

    def emit(line):
        print(line, flush=True)

    for raw_line in sys.stdin:
        line = raw_line.strip()
        with open(log_path, "a", encoding="utf-8") as log:
            log.write(line + "\\n")
        if line == "uci":
            emit("id name Fake Koi 1.1.0" if role == "koi" else "id name Fake Stockfish 17.1")
            emit("id author Task 1 test")
            emit("option name Threads type spin default 1 min 1 max 64")
            if role == "koi":
                emit("option name OwnBook type check default true")
                emit("option name BookFile type string default book.bin")
                emit("option name BookDepth type spin default 16 min 0 max 40")
                emit("option name BookRandom type check default false")
                emit("option name Speed type spin default 100 min 1 max 100")
            emit("uciok")
        elif line == "isready":
            emit("readyok")
        elif line.startswith("setoption name OwnBook value "):
            own_book = line.endswith("true")
        elif line.startswith("position fen "):
            side_to_move = line.split()[3]
        elif line.startswith("go "):
            if role == "koi" and own_book:
                emit("info string book move e2e4 depth 7")
            if role == "stockfish":
                score = 100 if side_to_move == "w" else -100
                emit(f"info depth 1 score cp {score} nodes 10 nps 20 time 1 pv e2e4")
            emit("bestmove e2e4")
        elif line == "quit":
            break
    """
)


def write_fake_engine(temporary_path: Path, role: str) -> tuple[Path, Path]:
    script_path = temporary_path / f"fake {role} engine.py"
    log_path = temporary_path / f"{role} commands.log"
    if os.name == "nt":
        script_path.write_text(FAKE_ENGINE_SOURCE, encoding="utf-8")
        command_path = temporary_path / f"fake {role} engine.cmd"
        command_path.write_text(
            f'@"{sys.executable}" "{script_path}" {role} "{log_path}"\n',
            encoding="utf-8",
        )
    else:
        # POSIX executes the command directly, so a shell wrapper mirrors the
        # Windows .cmd shim and passes the role and log path as arguments.
        script_path.write_text(FAKE_ENGINE_SOURCE, encoding="utf-8")
        command_path = temporary_path / f"fake {role} engine.sh"
        command_path.write_text(
            "#!/bin/sh\n" f'exec "{sys.executable}" "{script_path}" {role} "{log_path}"\n',
            encoding="utf-8",
        )
        command_path.chmod(0o755)
    return command_path, log_path


class EloOracleExtractionTests(unittest.TestCase):
    def test_parse_stockfish_terminal_none_bestmove_as_no_move(self):
        self.assertEqual(
            elo_oracle.parse_bestmove_line("bestmove (none)"),
            ("0000", None),
        )

    def test_normalize_score_uses_white_perspective_and_fixed_mate_scale(self):
        self.assertEqual(elo_oracle.normalize_score("cp", 37, "white"), 37)
        self.assertEqual(elo_oracle.normalize_score("cp", 37, "black"), -37)
        self.assertEqual(elo_oracle.normalize_score("mate", 4, "white"), 100000)
        self.assertEqual(elo_oracle.normalize_score("mate", -2, "black"), 100000)

    def test_cpl_is_loss_from_the_moving_side(self):
        self.assertEqual(elo_oracle.calculate_cpl(120, 80, "white"), 40)
        self.assertEqual(elo_oracle.calculate_cpl(-120, -80, "black"), 40)
        self.assertEqual(elo_oracle.calculate_cpl(80, 120, "white"), 0)

    def test_analysis_requires_external_engine_paths(self):
        with tempfile.TemporaryDirectory(prefix="koi oracle ") as temporary_directory:
            temporary_path = Path(temporary_directory)
            pgn_path = temporary_path / "fixture.pgn"
            report_path = temporary_path / "report.json"
            pgn_path.write_text('[Result "*"]\n\n1. e4 *\n', encoding="utf-8")

            completed = subprocess.run(
                [
                    sys.executable,
                    str(ORACLE_SCRIPT),
                    "--pgn",
                    str(pgn_path),
                    "--output",
                    str(report_path),
                ],
                capture_output=True,
                text=True,
                check=False,
            )

        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("--koi", completed.stderr)
        self.assertIn("--stockfish", completed.stderr)

    def test_cli_rejects_threads_other_than_the_approved_four(self):
        with tempfile.TemporaryDirectory(prefix="koi oracle ") as temporary_directory:
            temporary_path = Path(temporary_directory)
            pgn_path = temporary_path / "fixture.pgn"
            report_path = temporary_path / "report.json"
            pgn_path.write_text('[Result "*"]\n\n1. e4 *\n', encoding="utf-8")

            completed = subprocess.run(
                [
                    sys.executable,
                    str(ORACLE_SCRIPT),
                    "--pgn",
                    str(pgn_path),
                    "--output",
                    str(report_path),
                    "--extract-only",
                    "--threads",
                    "1",
                ],
                capture_output=True,
                text=True,
                check=False,
            )

        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("exactly 4", completed.stderr)

    def test_analysis_records_separate_cpl_values_and_engine_options(self):
        with tempfile.TemporaryDirectory(prefix="koi oracle ") as temporary_directory:
            temporary_path = Path(temporary_directory)
            pgn_path = temporary_path / "fixture game.pgn"
            report_path = temporary_path / "analysis report.json"
            pgn_path.write_text('[Result "*"]\n\n1. e4 *\n', encoding="utf-8")
            koi_path, koi_log_path = write_fake_engine(temporary_path, "koi")
            stockfish_path, _stockfish_log_path = write_fake_engine(temporary_path, "stockfish")

            completed = subprocess.run(
                [
                    sys.executable,
                    str(ORACLE_SCRIPT),
                    "--pgn",
                    str(pgn_path),
                    "--koi",
                    str(koi_path),
                    "--stockfish",
                    str(stockfish_path),
                    "--output",
                    str(report_path),
                    "--movetime-ms",
                    "1",
                    "--threads",
                    "4",
                ],
                capture_output=True,
                text=True,
                check=False,
            )

            self.assertEqual(completed.returncode, 0, completed.stderr)
            report = json.loads(report_path.read_text(encoding="utf-8"))
            koi_commands = koi_log_path.read_text(encoding="utf-8")
            expected_koi_sha256 = hashlib.sha256(koi_path.read_bytes()).hexdigest()

        position = report["games"][0]["positions"][0]
        self.assertEqual(report["mode"], "analysis")
        self.assertFalse(report["search_metrics"]["includes_book_hits"])
        self.assertEqual(report["search_metrics"]["positions"], 1)
        self.assertEqual(position["search"]["actual_game_move"]["cpl"], 0)
        self.assertEqual(position["search"]["koi_suggested_move"]["cpl"], 0)
        self.assertEqual(position["search"]["koi_suggested_move"]["uci"], "e2e4")
        self.assertEqual(position["search"]["timings_ms"]["koi_search"] >= 0, True)
        self.assertEqual(report["engines"]["koi"]["identity"]["name"], "Fake Koi 1.1.0")
        self.assertEqual(report["engines"]["koi"]["version"], "1.1.0")
        self.assertEqual(
            report["engines"]["koi"]["hashes"]["executable_sha256"],
            expected_koi_sha256,
        )
        self.assertEqual(report["engines"]["koi"]["options"]["OwnBook"], False)
        self.assertEqual(report["engines"]["koi"]["options"]["Threads"], 4)
        self.assertEqual(report["engines"]["koi"]["options"]["Speed"], 100)
        self.assertIn("setoption name OwnBook value false", koi_commands)
        self.assertIn("setoption name Threads value 4", koi_commands)
        self.assertIn("setoption name Speed value 100", koi_commands)
        self.assertIn("go movetime 1", koi_commands)
        self.assertEqual(position["search"]["actual_game_move"]["classification"], "best")
        self.assertEqual(position["search"]["koi_suggested_move"]["classification"], "best")
        self.assertEqual(report["measurement"]["network"]["state"], "disabled")
        self.assertEqual(report["measurement"]["book"]["state"], "disabled")
        self.assertEqual(report["measurement"]["tablebase"]["state"], "disabled")
        self.assertGreaterEqual(report["hardware"]["cpu_count"], 1)
        self.assertIn("executable_sha256", report["engines"]["stockfish"]["hashes"])

    def test_extract_preserves_annotations_on_mainline_positions(self):
        pgn = '[Result "*"]\n\n1. e4 $1 {[%eval +0.25] [%clk 0:05:00] coach note} e5 *\n'

        games = elo_oracle.extract_games(pgn)

        annotations = games[0]["positions"][0]["annotations"]
        self.assertEqual(annotations["comment"], "[%eval +0.25] [%clk 0:05:00] coach note")
        self.assertEqual(annotations["nags"], [1])
        self.assertEqual(annotations["eval"], "+0.25")
        self.assertEqual(annotations["clock"], "0:05:00")

    def test_classify_cpl_uses_stable_forensic_bands(self):
        self.assertEqual(elo_oracle.classify_cpl(0), "best")
        self.assertEqual(elo_oracle.classify_cpl(49), "good")
        self.assertEqual(elo_oracle.classify_cpl(50), "inaccuracy")
        self.assertEqual(elo_oracle.classify_cpl(99), "inaccuracy")
        self.assertEqual(elo_oracle.classify_cpl(100), "mistake")
        self.assertEqual(elo_oracle.classify_cpl(299), "mistake")
        self.assertEqual(elo_oracle.classify_cpl(300), "blunder")
        self.assertIsNone(elo_oracle.classify_cpl(None))

    def test_book_audit_keeps_book_hits_and_cpl_outside_search_metrics(self):
        with tempfile.TemporaryDirectory(prefix="koi oracle ") as temporary_directory:
            temporary_path = Path(temporary_directory)
            pgn_path = temporary_path / "fixture game.pgn"
            book_path = temporary_path / "licensed book.bin"
            report_path = temporary_path / "book audit report.json"
            pgn_path.write_text('[Result "*"]\n\n1. e4 *\n', encoding="utf-8")
            book_path.write_bytes(b"synthetic test book")
            koi_path, koi_log_path = write_fake_engine(temporary_path, "koi")
            stockfish_path, _stockfish_log_path = write_fake_engine(temporary_path, "stockfish")

            completed = subprocess.run(
                [
                    sys.executable,
                    str(ORACLE_SCRIPT),
                    "--pgn",
                    str(pgn_path),
                    "--koi",
                    str(koi_path),
                    "--stockfish",
                    str(stockfish_path),
                    "--output",
                    str(report_path),
                    "--movetime-ms",
                    "1",
                    "--threads",
                    "4",
                    "--book",
                    str(book_path),
                ],
                capture_output=True,
                text=True,
                check=False,
            )

            self.assertEqual(completed.returncode, 0, completed.stderr)
            report = json.loads(report_path.read_text(encoding="utf-8"))
            koi_commands = koi_log_path.read_text(encoding="utf-8")

        position = report["games"][0]["positions"][0]["book_audit"]
        self.assertEqual(report["mode"], "book-audit")
        self.assertIsNone(report["search_metrics"])
        self.assertEqual(report["book_audit"]["book_hits"], 1)
        self.assertEqual(report["book_audit"]["settings"]["BookDepth"], 16)
        self.assertFalse(report["book_audit"]["settings"]["BookRandom"])
        self.assertTrue(position["book_used"])
        self.assertEqual(position["book_move"], "e2e4")
        self.assertEqual(position["book_ply"], 7)
        self.assertEqual(position["stockfish_cpl"], 0)
        self.assertEqual(position["actual_game_move_stockfish_cpl"], 0)
        self.assertIn("setoption name OwnBook value true", koi_commands)
        self.assertIn("setoption name BookDepth value 16", koi_commands)
        self.assertIn("setoption name BookRandom value false", koi_commands)
        self.assertIn(f"setoption name BookFile value {Path(book_path).resolve()}", koi_commands)

    def test_extract_only_records_mainline_san_positions_and_metadata(self):
        pgn = """[Event \"Extraction\"]
[Site \"Test\"]
[Date \"2026.09.04\"]
[Round \"1\"]
[White \"White\"]
[Black \"Black\"]
[Result \"*\"]

1. e4 {king pawn} (1. d4 d5) e5 2. Nf3 Nc6 3. Bc4 Nf6 4. O-O Be7 *

[Event \"Promotion\"]
[SetUp \"1\"]
[FEN \"7k/P7/8/8/8/8/8/6K1 w - - 0 1\"]
[Result \"*\"]

1. a8=Q+ *

[Event \"Black FEN\"]
[SetUp \"1\"]
[FEN \"7k/8/8/8/8/8/8/7K b - - 7 23\"]
[Result \"*\"]

23... Kg8 *
"""

        with tempfile.TemporaryDirectory(prefix="koi oracle ") as temporary_directory:
            temporary_path = Path(temporary_directory)
            pgn_path = temporary_path / "fixture game.pgn"
            report_path = temporary_path / "extraction report.json"
            pgn_path.write_text(pgn, encoding="utf-8")
            expected_pgn_sha256 = hashlib.sha256(pgn_path.read_bytes()).hexdigest()

            completed = subprocess.run(
                [
                    sys.executable,
                    str(ORACLE_SCRIPT),
                    "--pgn",
                    str(pgn_path),
                    "--output",
                    str(report_path),
                    "--extract-only",
                    "--koi",
                    str(temporary_path / "missing koi.exe"),
                    "--stockfish",
                    str(temporary_path / "missing stockfish.exe"),
                    "--book",
                    str(temporary_path / "missing book.bin"),
                ],
                capture_output=True,
                text=True,
                check=False,
            )

            self.assertEqual(completed.returncode, 0, completed.stderr)
            report = json.loads(report_path.read_text(encoding="utf-8"))

        self.assertEqual(report["schema"], "koi-elo-oracle")
        self.assertEqual(report["schema_version"], 1)
        self.assertEqual(report["mode"], "extract-only")
        self.assertEqual(
            report["source"]["pgn_sha256"],
            expected_pgn_sha256,
        )
        self.assertRegex(report["created_at"], r"^20\d\d-\d\d-\d\dT")

        self.assertEqual(len(report["games"]), 3)
        first_game = report["games"][0]
        self.assertEqual(len(first_game["positions"]), 8)
        self.assertEqual(first_game["positions"][0]["fen"], "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1")
        self.assertEqual(first_game["positions"][0]["ply"], 1)
        self.assertEqual(first_game["positions"][0]["side"], "white")
        self.assertEqual(first_game["positions"][0]["actual_move_uci"], "e2e4")
        self.assertEqual(first_game["positions"][0]["actual_move_san"], "e4")

        castling_position = next(
            position
            for position in first_game["positions"]
            if position["actual_move_san"] == "O-O"
        )
        self.assertEqual(castling_position["ply"], 7)
        self.assertEqual(castling_position["actual_move_uci"], "e1g1")
        self.assertNotIn("d2d4", [position["actual_move_uci"] for position in first_game["positions"]])

        promotion_position = report["games"][1]["positions"][0]
        self.assertEqual(promotion_position["fen"], "7k/P7/8/8/8/8/8/6K1 w - - 0 1")
        self.assertEqual(promotion_position["side"], "white")
        self.assertEqual(promotion_position["actual_move_uci"], "a7a8q")
        self.assertEqual(promotion_position["actual_move_san"], "a8=Q+")

        black_fen_position = report["games"][2]["positions"][0]
        self.assertEqual(black_fen_position["ply"], 1)
        self.assertEqual(black_fen_position["move_number"], 23)
        self.assertEqual(black_fen_position["side"], "black")
        self.assertEqual(black_fen_position["fen"], "7k/8/8/8/8/8/8/7K b - - 7 23")
        self.assertEqual(black_fen_position["actual_move_uci"], "h8g8")
        self.assertEqual(black_fen_position["actual_move_san"], "Kg8")

    def test_match_entrypoint_preserves_zero_random_seed_and_shell_free_arguments(self):
        import tools.measurement.stockfish_match as stockfish_match

        args = stockfish_match._build_parser().parse_args(
            [
                "--koi",
                "C:\\Koi Engine\\koi.exe",
                "--stockfish",
                "C:\\Engines\\stockfish.exe",
                "--random-seed",
                "0",
                "--movetime-ms",
                "25",
            ]
        )
        command = stockfish_match.build_command(args, powershell="pwsh-test")

        self.assertEqual(command[0], "pwsh-test")
        self.assertIn("-KoiRandomSeed", command)
        self.assertEqual(command[command.index("-KoiRandomSeed") + 1], "0")
        self.assertIn("-MovetimeMs", command)
        self.assertNotIn("-Depth", command)
        self.assertTrue(args.output_directory.resolve().is_relative_to((REPOSITORY_ROOT / "artifacts").resolve()))

    def test_extract_only_defaults_to_a_repository_artifact_and_announces_path(self):
        with tempfile.TemporaryDirectory(prefix="koi oracle ") as temporary_directory:
            temporary_path = Path(temporary_directory)
            pgn_path = temporary_path / "fixture.pgn"
            pgn_path.write_text('[Result "*"]\n\n1. e4 *\n', encoding="utf-8")

            completed = subprocess.run(
                [
                    sys.executable,
                    str(ORACLE_SCRIPT),
                    "--pgn",
                    str(pgn_path),
                    "--extract-only",
                ],
                capture_output=True,
                text=True,
                check=False,
            )

        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertEqual(completed.stderr, "")
        report_lines = [line for line in completed.stdout.splitlines() if line.startswith("report ")]
        self.assertEqual(len(report_lines), 1, completed.stdout)
        report_path = Path(report_lines[0].split(" ", 1)[1]).resolve()
        self.assertTrue(report_path.is_relative_to((REPOSITORY_ROOT / "artifacts").resolve()))
        self.assertTrue(report_path.is_file())
        self.assertEqual(json.loads(report_path.read_text(encoding="utf-8"))["mode"], "extract-only")
        report_path.unlink(missing_ok=True)

    def test_named_tool_entrypoints_have_safe_help_without_starting_engines(self):
        for script, required_text in (
            (MATCH_SCRIPT, "--koi"),
            (BOOK_AUDIT_SCRIPT, "--book"),
        ):
            completed = subprocess.run(
                [sys.executable, str(script), "--help"],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertEqual(completed.stderr, "")
            self.assertIn(required_text, completed.stdout)

    def test_book_audit_entrypoint_forwards_to_the_existing_report_schema(self):
        with tempfile.TemporaryDirectory(prefix="koi oracle ") as temporary_directory:
            temporary_path = Path(temporary_directory)
            pgn_path = temporary_path / "fixture game.pgn"
            book_path = temporary_path / "licensed book.bin"
            report_path = temporary_path / "book audit report.json"
            pgn_path.write_text('[Result "*"]\n\n1. e4 *\n', encoding="utf-8")
            book_path.write_bytes(b"synthetic test book")
            koi_path, _koi_log_path = write_fake_engine(temporary_path, "koi")
            stockfish_path, _stockfish_log_path = write_fake_engine(temporary_path, "stockfish")

            completed = subprocess.run(
                [
                    sys.executable,
                    str(BOOK_AUDIT_SCRIPT),
                    "--pgn",
                    str(pgn_path),
                    "--koi",
                    str(koi_path),
                    "--stockfish",
                    str(stockfish_path),
                    "--book",
                    str(book_path),
                    "--output",
                    str(report_path),
                    "--movetime-ms",
                    "1",
                    "--threads",
                    "4",
                ],
                capture_output=True,
                text=True,
                check=False,
            )

            self.assertEqual(completed.returncode, 0, completed.stderr)
            report = json.loads(report_path.read_text(encoding="utf-8"))

        self.assertEqual(report["schema"], "koi-elo-oracle")
        self.assertEqual(report["mode"], "book-audit")

    def test_book_audit_entrypoint_defaults_to_a_repository_artifact(self):
        with tempfile.TemporaryDirectory(prefix="koi oracle ") as temporary_directory:
            temporary_path = Path(temporary_directory)
            pgn_path = temporary_path / "fixture.pgn"
            book_path = temporary_path / "licensed book.bin"
            pgn_path.write_text('[Result "*"]\n\n1. e4 *\n', encoding="utf-8")
            book_path.write_bytes(b"synthetic test book")
            koi_path, _koi_log_path = write_fake_engine(temporary_path, "koi")
            stockfish_path, _stockfish_log_path = write_fake_engine(temporary_path, "stockfish")

            completed = subprocess.run(
                [
                    sys.executable,
                    str(BOOK_AUDIT_SCRIPT),
                    "--pgn",
                    str(pgn_path),
                    "--koi",
                    str(koi_path),
                    "--stockfish",
                    str(stockfish_path),
                    "--book",
                    str(book_path),
                    "--movetime-ms",
                    "1",
                    "--threads",
                    "4",
                ],
                capture_output=True,
                text=True,
                check=False,
            )

        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertEqual(completed.stderr, "")
        report_lines = [line for line in completed.stdout.splitlines() if line.startswith("report ")]
        self.assertEqual(len(report_lines), 1, completed.stdout)
        report_path = Path(report_lines[0].split(" ", 1)[1]).resolve()
        self.assertTrue(report_path.is_relative_to((REPOSITORY_ROOT / "artifacts").resolve()))
        self.assertTrue(report_path.is_file())
        self.assertEqual(json.loads(report_path.read_text(encoding="utf-8"))["mode"], "book-audit")

        report_path.unlink(missing_ok=True)


if __name__ == "__main__":
    unittest.main()
