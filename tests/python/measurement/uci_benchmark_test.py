import hashlib
import importlib.util
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[3]
MODULE_PATH = ROOT / "tools" / "measurement" / "uci_benchmark.py"
SPEC = importlib.util.spec_from_file_location("uci_benchmark", MODULE_PATH)
uci_benchmark = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(uci_benchmark)


FAKE_ENGINE = r'''import sys
import os

print("id name Fake UCI Engine", flush=True)
print("id author Koi benchmark test", flush=True)
for line in sys.stdin:
    command = line.strip()
    event_log = os.environ.get("FAKE_UCI_EVENT_LOG")
    if event_log:
        with open(event_log, "a", encoding="utf-8") as stream:
            stream.write(command + "\n")
    if command == "uci":
        print("option name Hash type spin default 16 min 1 max 65536", flush=True)
        print("option name Threads type spin default 1 min 1 max 512", flush=True)
        print("uciok", flush=True)
    elif command == "isready":
        print("readyok", flush=True)
    elif command.startswith("go depth "):
        print("info depth 1 time 2 nodes 10 nps 5000", flush=True)
        print("info depth 2 time 5 nodes 30 nps 6000", flush=True)
        print("info depth 2 currmove e2e4 currmovenumber 1", flush=True)
        print("bestmove e2e4", flush=True)
    elif command == "quit":
        break
'''


class UciBenchmarkReportTest(unittest.TestCase):
    def test_report_captures_sequential_runs_and_provenance_from_fake_engines(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            temp = Path(temp_dir)
            engine_path = temp / "fake_uci_engine.py"
            engine_path.write_text(FAKE_ENGINE, encoding="utf-8")
            corpus_path = temp / "positions.fen"
            corpus_bytes = (
                b"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1\n"
                b"rnbqkbnr/pppp1ppp/8/4p3/4P3/8/PPPP1PPP/RNBQKBNR w KQkq e6 0 2\n"
            )
            corpus_path.write_bytes(corpus_bytes)
            output_path = temp / "report.json"
            event_log = temp / "uci-events.log"

            with mock.patch.dict(os.environ, {"FAKE_UCI_EVENT_LOG": str(event_log)}):
                report = uci_benchmark.run_benchmark(
                    engines=[("koi", [sys.executable, str(engine_path)]),
                             ("stockfish", [sys.executable, str(engine_path)])],
                    corpus_path=corpus_path,
                    source_revision="abc1234",
                    output_path=output_path,
                )

            self.assertEqual(report["schema"], "koi-uci-benchmark-v1")
            self.assertEqual(report["source_revision"], "abc1234")
            self.assertEqual(report["corpus"]["sha256"], hashlib.sha256(corpus_bytes).hexdigest())
            self.assertEqual(report["settings"], {"hash_mb": 512, "depth": 12, "threads": [1, 4]})
            self.assertEqual([run["engine"] for run in report["runs"]],
                             ["koi", "koi", "stockfish", "stockfish"])
            self.assertEqual([run["options"]["Threads"] for run in report["runs"]], [1, 4, 1, 4])

            run = report["runs"][0]
            self.assertEqual(run["engine_id"], {"name": "Fake UCI Engine", "author": "Koi benchmark test"})
            executable_path = Path(run["executable"]["path"])
            self.assertEqual(run["executable"]["sha256"], hashlib.sha256(executable_path.read_bytes()).hexdigest())
            self.assertEqual(run["completed_depth"], 2)
            self.assertEqual(run["positions"][0]["bestmove"], "e2e4")
            self.assertEqual(run["positions"][0]["depth_rows"], [
                {"depth": 1, "nodes": 10, "nps": 5000, "time_ms": 2},
                {"depth": 2, "nodes": 30, "nps": 6000, "time_ms": 5},
            ])
            self.assertIn("host", report)
            self.assertIn("cpu", report["host"])
            self.assertIn("os", report["host"])
            self.assertIn("ram_bytes", report["host"])
            self.assertIn("peak_rss_bytes", run)
            self.assertEqual(run["advertised_options"], ["Hash", "Threads"])
            events = event_log.read_text(encoding="utf-8").splitlines()
            self.assertEqual(events[:7], [
                "uci", "setoption name Hash value 512", "setoption name Threads value 1",
                "isready", "ucinewgame", "isready",
                "position fen rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
            ])
            self.assertEqual(events[7], "go depth 12")
            self.assertEqual(events[8:12], [
                "ucinewgame", "isready",
                "position fen rnbqkbnr/pppp1ppp/8/4p3/4P3/8/PPPP1PPP/RNBQKBNR w KQkq e6 0 2",
                "go depth 12",
            ])
            self.assertEqual(json.loads(output_path.read_text(encoding="utf-8")), report)

    def test_refuses_to_search_when_engine_does_not_advertise_required_options(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            temp = Path(temp_dir)
            engine_path = temp / "missing_threads.py"
            engine_path.write_text(FAKE_ENGINE.replace(
                '        print("option name Threads type spin default 1 min 1 max 512", flush=True)\n',
                "",
            ), encoding="utf-8")
            corpus_path = temp / "one.fen"
            corpus_path.write_text("8/8/8/8/8/8/8/K6k w - - 0 1\n", encoding="utf-8")
            event_log = temp / "events.log"

            with mock.patch.dict(os.environ, {"FAKE_UCI_EVENT_LOG": str(event_log)}):
                with self.assertRaisesRegex(RuntimeError, "does not advertise required UCI option.*Threads"):
                    uci_benchmark.run_benchmark(
                        engines=[("koi", [sys.executable, str(engine_path)])],
                        corpus_path=corpus_path,
                        source_revision="abc1234",
                    )

            self.assertNotIn("go depth", event_log.read_text(encoding="utf-8"))

    def test_rejects_an_empty_fen_corpus_before_starting_an_engine(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            corpus_path = Path(temp_dir) / "empty.fen"
            corpus_path.write_text("# only a comment\n\n", encoding="utf-8")

            with self.assertRaisesRegex(ValueError, "no FEN positions"):
                uci_benchmark.run_benchmark(
                    engines=[("koi", [sys.executable, "must-not-be-launched.py"])],
                    corpus_path=corpus_path,
                    source_revision="abc1234",
                )


if __name__ == "__main__":
    unittest.main()
