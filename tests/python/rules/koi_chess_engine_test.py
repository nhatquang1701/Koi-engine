"""Unit tests for koi_chess.engine.

The tests use a tiny scripted UCI engine (a Python script spawned with the
current interpreter) so they run without Stockfish.  A separate differential
harness compared the client against python-chess and the vendored Stockfish.
"""

import os
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools" / "measurement"))

import koi_chess as chess  # noqa: E402
from koi_chess import engine  # noqa: E402

FAKE_ENGINE_SOURCE = '''\
import os
import sys

log_path = sys.argv[1] if len(sys.argv) > 1 else None
scenario = os.environ.get("FAKE_SCENARIO", "cp")


def log(line):
    if log_path:
        with open(log_path, "a", encoding="utf-8") as handle:
            handle.write(line + "\\n")


for line in sys.stdin:
    line = line.strip()
    log(line)
    if line == "uci":
        print("id name Fake Koi Engine", flush=True)
        print("option name Threads type spin default 1 min 1 max 64", flush=True)
        print("option name Hash type spin default 16 min 1 max 1024", flush=True)
        print("option name MultiPV type spin default 1 min 1 max 8", flush=True)
        print("option name UCI_ShowWDL type check default false", flush=True)
        print("uciok", flush=True)
    elif line == "isready":
        print("readyok", flush=True)
    elif line.startswith("setoption"):
        pass
    elif line.startswith("go"):
        if scenario == "terminate":
            sys.exit(0)
        elif scenario == "mate":
            print("info depth 5 seldepth 7 score mate 2 pv g1d4 h8g8", flush=True)
            print("bestmove g1d4", flush=True)
        elif scenario == "negative":
            print("info depth 4 score cp -30 nodes 900 pv e7e5", flush=True)
            print("bestmove e7e5", flush=True)
        elif scenario == "nomove":
            print("info depth 1 score cp 0", flush=True)
            print("bestmove 0000", flush=True)
        else:
            print("info depth 3 seldepth 5 score cp 42 nodes 1234 pv e2e4 e7e5 g1f3", flush=True)
            print("bestmove e2e4 ponder e7e5", flush=True)
    elif line == "quit":
        break
'''

BLACK_TO_MOVE_FEN = "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq - 0 1"


class FakeEngineMixin(unittest.TestCase):
    def setUp(self):
        self._temp = tempfile.TemporaryDirectory()
        self.addCleanup(self._temp.cleanup)
        self.log_path = Path(self._temp.name) / "commands.log"
        self.script_path = Path(self._temp.name) / "fake_engine.py"
        self.script_path.write_text(FAKE_ENGINE_SOURCE, encoding="utf-8")

    def spawn(self, scenario="cp"):
        environment = dict(os.environ)
        environment["FAKE_SCENARIO"] = scenario
        engine_instance = engine.SimpleEngine.popen_uci(
            [sys.executable, str(self.script_path), str(self.log_path)],
            env=environment,
        )
        self.addCleanup(engine_instance.quit)
        return engine_instance

    def commands(self):
        if not self.log_path.exists():
            return []
        return self.log_path.read_text(encoding="utf-8").splitlines()


class ScoreTests(unittest.TestCase):
    def test_cp_score(self):
        score = engine.Cp(42)
        self.assertEqual(score.cp, 42)
        self.assertEqual(score.score(), 42)
        self.assertFalse(score.is_mate())
        self.assertIsNone(score.mate())

    def test_mate_score(self):
        score = engine.Mate(2)
        self.assertEqual(score.moves, 2)
        self.assertEqual(score.mate(), 2)
        self.assertTrue(score.is_mate())
        self.assertIsNone(score.score())
        self.assertEqual(score.score(mate_score=100000), 99998)
        self.assertEqual(engine.Mate(-2).score(mate_score=100000), -99998)

    def test_pov_score_negation(self):
        score = engine.PovScore(engine.Cp(10), chess.WHITE)
        self.assertEqual(score.pov(chess.WHITE).score(), 10)
        self.assertEqual(score.pov(chess.BLACK).score(), -10)
        self.assertEqual(score.white().score(), 10)
        self.assertEqual(score.black().score(), -10)
        mate = engine.PovScore(engine.Mate(2), chess.BLACK)
        self.assertTrue(mate.pov(chess.WHITE).is_mate())
        self.assertEqual(mate.pov(chess.WHITE).mate(), -2)


class LimitTests(unittest.TestCase):
    def test_depth_limit(self):
        self.assertEqual(engine.Limit(depth=9).go_command(), "go depth 9")

    def test_combined_limits(self):
        command = engine.Limit(depth=5, nodes=1000, movetime=250).go_command()
        self.assertEqual(command, "go depth 5 nodes 1000 movetime 250")

    def test_time_and_increment_limits(self):
        command = engine.Limit(time=10000, inc=100).go_command()
        self.assertEqual(command, "go time 10000 inc 100")

    def test_infinite_limit(self):
        self.assertEqual(engine.Limit(infinite=True).go_command(), "go infinite")

    def test_empty_limit_is_rejected(self):
        with self.assertRaises(engine.EngineError):
            engine.Limit().go_command()


class EngineSessionTests(FakeEngineMixin):
    def test_handshake_and_configure(self):
        session = self.spawn()
        session.configure({"Threads": 2, "Hash": 64, "UCI_ShowWDL": False})
        commands = self.commands()
        self.assertIn("uci", commands)
        self.assertIn("setoption name Threads value 2", commands)
        self.assertIn("setoption name Hash value 64", commands)
        self.assertIn("setoption name UCI_ShowWDL value false", commands)
        self.assertEqual(commands[-1], "isready")

    def test_analyse_reports_score_pv_and_depth(self):
        session = self.spawn("cp")
        info = session.analyse(chess.Board(), engine.Limit(depth=3))
        self.assertEqual(info["depth"], 3)
        self.assertEqual(info["seldepth"], 5)
        self.assertEqual(info["nodes"], 1234)
        self.assertFalse(info["score"].is_mate())
        self.assertEqual(info["score"].pov(chess.WHITE).score(), 42)
        self.assertEqual([move.uci() for move in info["pv"]], ["e2e4", "e7e5", "g1f3"])
        commands = self.commands()
        self.assertTrue(any(command.startswith("position fen ") for command in commands))
        self.assertIn("go depth 3", commands)

    def test_analyse_reports_mate_scores(self):
        session = self.spawn("mate")
        info = session.analyse(chess.Board(), engine.Limit(depth=5))
        score = info["score"].pov(chess.WHITE)
        self.assertTrue(score.is_mate())
        self.assertEqual(score.mate(), 2)
        self.assertIsNone(score.score())
        self.assertEqual([move.uci() for move in info["pv"]], ["g1d4", "h8g8"])

    def test_analyse_score_is_relative_to_the_side_to_move(self):
        session = self.spawn("negative")
        board = chess.Board(BLACK_TO_MOVE_FEN)
        info = session.analyse(board, engine.Limit(depth=4))
        self.assertEqual(info["score"].pov(chess.BLACK).score(), -30)
        self.assertEqual(info["score"].pov(chess.WHITE).score(), 30)
        self.assertTrue(any(command.startswith("position fen rnbqkbnr") for command in self.commands()))

    def test_analyse_multipv_sets_the_option(self):
        session = self.spawn()
        session.analyse(chess.Board(), engine.Limit(depth=2), multipv=3)
        self.assertIn("setoption name MultiPV value 3", self.commands())

    def test_play_returns_move_and_ponder(self):
        session = self.spawn("cp")
        result = session.play(chess.Board(), engine.Limit(depth=3))
        self.assertEqual(result.move.uci(), "e2e4")
        self.assertEqual(result.ponder.uci(), "e7e5")

    def test_play_returns_none_for_no_move(self):
        session = self.spawn("nomove")
        result = session.play(chess.Board(), engine.Limit(depth=1))
        self.assertIsNone(result.move)
        self.assertIsNone(result.ponder)

    def test_quit_is_idempotent(self):
        session = self.spawn()
        session.quit()
        session.quit()

    def test_context_manager_quits(self):
        with self.spawn() as session:
            self.assertIsInstance(session.analyse(chess.Board(), engine.Limit(depth=1)), dict)

    def test_missing_binary_raises_engine_error(self):
        with self.assertRaises(engine.EngineError):
            engine.SimpleEngine.popen_uci("koi-definitely-missing-engine.exe")

    def test_terminated_engine_raises_terminated_error(self):
        session = self.spawn("terminate")
        with self.assertRaises(engine.EngineTerminatedError):
            session.analyse(chess.Board(), engine.Limit(depth=1))

    def test_closed_engine_rejects_commands(self):
        session = self.spawn()
        session.quit()
        with self.assertRaises(engine.EngineError):
            session.analyse(chess.Board(), engine.Limit(depth=1))


if __name__ == "__main__":
    unittest.main()
