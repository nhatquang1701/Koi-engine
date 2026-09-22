"""Minimal UCI engine client for koi_chess.

This module covers the subset of :mod:`chess.engine` used by Koi's
training-data tooling: ``SimpleEngine.popen_uci``, ``configure``, ``analyse``,
``play`` and ``Limit(depth=...)``.  Scores are reported relative to the side
to move and wrapped in :class:`PovScore` so ``info["score"].pov(board.turn)``
behaves like python-chess.
"""

from __future__ import annotations

import queue
import subprocess
import sys
import threading
from collections import deque

from .core import BLACK, WHITE, Move

__all__ = [
    "Cp",
    "EngineError",
    "EngineTerminatedError",
    "Limit",
    "Mate",
    "PlayResult",
    "PovScore",
    "Score",
    "SimpleEngine",
]

_TERMINATION_TOKENS = frozenset({"0000", "(none)", "none"})

_INFO_INTEGER_KEYS = frozenset(
    {
        "depth",
        "seldepth",
        "time",
        "nodes",
        "multipv",
        "currmovenumber",
        "hashfull",
        "nps",
        "tbhits",
        "sbhits",
        "cpuload",
    }
)

_INFO_FLAG_KEYS = frozenset({"lowerbound", "upperbound"})


class EngineError(Exception):
    """Raised when the engine cannot be started or violates the UCI protocol."""


class EngineTerminatedError(EngineError):
    """Raised when the engine process exits while a command is outstanding."""


class Score:
    """Base class for engine scores."""

    def is_mate(self) -> bool:
        return False

    def mate(self):
        return None

    def score(self, *, mate_score=None):
        raise NotImplementedError


class Cp(Score):
    """A score in centipawns, relative to the side to move."""

    __slots__ = ("cp",)

    def __init__(self, cp):
        if isinstance(cp, float) and cp.is_integer():
            cp = int(cp)
        self.cp = cp

    def is_mate(self) -> bool:
        return False

    def mate(self):
        return None

    def score(self, *, mate_score=None):
        return self.cp

    def __neg__(self):
        return Cp(-self.cp)

    def __eq__(self, other):
        return isinstance(other, Cp) and self.cp == other.cp

    def __hash__(self):
        return hash((Cp, self.cp))

    def __str__(self):
        return f"+{self.cp:d}" if self.cp > 0 else str(self.cp)

    def __repr__(self):
        return f"Cp({self})"


class Mate(Score):
    """A mate score in moves, positive when the side to move mates."""

    __slots__ = ("moves",)

    def __init__(self, moves):
        self.moves = int(moves)

    def is_mate(self) -> bool:
        return True

    def mate(self):
        return self.moves

    def score(self, *, mate_score=None):
        if mate_score is None:
            return None
        if self.moves > 0:
            return mate_score - self.moves
        return -mate_score - self.moves

    def __neg__(self):
        return Mate(-self.moves)

    def __eq__(self, other):
        return isinstance(other, Mate) and self.moves == other.moves

    def __hash__(self):
        return hash((Mate, self.moves))

    def __str__(self):
        return f"#+{self.moves}" if self.moves > 0 else f"#-{abs(self.moves)}"

    def __repr__(self):
        return f"Mate({self.moves})"


class PovScore:
    """A score together with the color it is relative to."""

    __slots__ = ("relative", "turn")

    def __init__(self, relative, turn):
        self.relative = relative
        self.turn = turn

    def white(self):
        return self.pov(WHITE)

    def black(self):
        return self.pov(BLACK)

    def pov(self, color):
        return self.relative if self.turn == color else -self.relative

    def is_mate(self) -> bool:
        return self.relative.is_mate()

    def score(self, *, mate_score=None):
        return self.relative.score(mate_score=mate_score)

    def __neg__(self):
        return PovScore(-self.relative, self.turn)

    def __eq__(self, other):
        if isinstance(other, PovScore):
            return self.white() == other.white()
        return NotImplemented

    def __repr__(self):
        return f"PovScore({self.relative!r}, {'WHITE' if self.turn else 'BLACK'})"


class PlayResult:
    """The result of :meth:`SimpleEngine.play`."""

    __slots__ = ("move", "ponder")

    def __init__(self, move=None, ponder=None):
        self.move = move
        self.ponder = ponder

    def __repr__(self):
        return f"PlayResult(move={self.move!r}, ponder={self.ponder!r})"


class Limit:
    """Search limits translated into a ``go`` command."""

    __slots__ = ("time", "inc", "depth", "nodes", "movetime", "mate", "infinite")

    def __init__(
        self, *, time=None, inc=None, depth=None, nodes=None, movetime=None, mate=None, infinite=False
    ):
        self.time = time
        self.inc = inc
        self.depth = depth
        self.nodes = nodes
        self.movetime = movetime
        self.mate = mate
        self.infinite = infinite

    def go_command(self) -> str:
        parts = ["go"]
        if self.infinite:
            parts.append("infinite")
        if self.depth is not None:
            parts += ["depth", str(int(self.depth))]
        if self.nodes is not None:
            parts += ["nodes", str(int(self.nodes))]
        if self.movetime is not None:
            parts += ["movetime", str(int(self.movetime))]
        if self.time is not None:
            parts += ["time", str(int(self.time))]
        if self.inc is not None:
            parts += ["inc", str(int(self.inc))]
        if self.mate is not None:
            parts += ["mate", str(int(self.mate))]
        if len(parts) == 1:
            raise EngineError("the search limit does not specify depth, nodes, time or movetime")
        return " ".join(parts)

    def __repr__(self):
        fields = ", ".join(
            f"{name}={getattr(self, name)!r}"
            for name in self.__slots__
            if getattr(self, name) not in (None, False)
        )
        return f"Limit({fields})"


def _format_option_value(value) -> str | None:
    if value is None:
        return None
    if isinstance(value, bool):
        return "true" if value else "false"
    return str(value)


def _parse_int(token):
    try:
        return int(token)
    except ValueError:
        return float(token)


class SimpleEngine:
    """A blocking UCI engine session."""

    def __init__(self, process):
        self._process = process
        self._stdout: queue.Queue = queue.Queue()
        self._stderr: deque = deque(maxlen=200)
        self._closed = False
        self._lock = threading.Lock()
        self._reader_thread = threading.Thread(
            target=self._pump_stdout, name="koi-chess-engine-stdout", daemon=True
        )
        self._stderr_thread = threading.Thread(
            target=self._pump_stderr, name="koi-chess-engine-stderr", daemon=True
        )
        self._reader_thread.start()
        self._stderr_thread.start()

    # -- construction ----------------------------------------------------

    @classmethod
    def popen_uci(cls, command, *, timeout: float = 10.0, debug: bool = False, **popen_kwargs):
        if isinstance(command, (str, bytes)) or hasattr(command, "__fspath__"):
            args = [str(command)]
        else:
            args = [str(part) for part in command]
        if sys.platform == "win32":
            popen_kwargs.setdefault("creationflags", subprocess.CREATE_NO_WINDOW)
        try:
            process = subprocess.Popen(
                args,
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                universal_newlines=True,
                encoding="utf-8",
                errors="replace",
                **popen_kwargs,
            )
        except OSError as error:
            raise EngineError(f"failed to start the engine: {error}") from error
        engine = cls(process)
        try:
            engine._send("uci")
            engine._wait_for_line(lambda line: line.strip() == "uciok", timeout=timeout)
            engine._send("isready")
            engine._wait_for_line(lambda line: line.strip() == "readyok", timeout=timeout)
        except Exception:
            engine.quit()
            raise
        return engine

    # -- process plumbing ------------------------------------------------

    def _pump_stdout(self):
        try:
            for line in self._process.stdout:
                self._stdout.put(line.rstrip("\r\n"))
        except Exception:
            pass
        finally:
            self._stdout.put(None)

    def _pump_stderr(self):
        try:
            for line in self._process.stderr:
                self._stderr.append(line.rstrip("\r\n"))
        except Exception:
            pass

    def _diagnostics(self) -> str:
        if not self._stderr:
            return ""
        return " | ".join(self._stderr)

    def _send(self, command: str):
        if self._closed:
            raise EngineError("the engine has been closed")
        try:
            self._process.stdin.write(command + "\n")
            self._process.stdin.flush()
        except (BrokenPipeError, ValueError, OSError) as error:
            raise EngineTerminatedError("engine process terminated") from error

    def _read_line(self, timeout=None) -> str:
        try:
            line = self._stdout.get(timeout=timeout)
        except queue.Empty:
            raise EngineError(f"timed out waiting for the engine, diagnostics: {self._diagnostics()}")
        if line is None:
            details = self._diagnostics()
            message = "engine process terminated"
            if details:
                message += f", diagnostics: {details}"
            raise EngineTerminatedError(message)
        return line

    def _wait_for_line(self, predicate, *, timeout=None) -> str:
        while True:
            line = self._read_line(timeout=timeout)
            if predicate(line):
                return line

    # -- python-chess style API ------------------------------------------

    def configure(self, options: dict):
        if not options:
            return
        for name, value in options.items():
            formatted = _format_option_value(value)
            if formatted is None:
                self._send(f"setoption name {name}")
            else:
                self._send(f"setoption name {name} value {formatted}")
        self._send("isready")
        self._wait_for_line(lambda line: line.strip() == "readyok")

    def setoption(self, name: str, value=None):
        self.configure({name: value})

    def _send_position(self, board):
        fen = board.fen()
        self._send(f"position fen {fen}")

    def analyse(self, board, limit: Limit, multipv=None) -> dict:
        if multipv is not None:
            self.setoption("MultiPV", multipv)
        with self._lock:
            self._send_position(board)
            self._send(limit.go_command())
            return self._read_search_result(board)

    def play(self, board, limit: Limit) -> PlayResult:
        with self._lock:
            self._send_position(board)
            self._send(limit.go_command())
            info, bestmove, ponder = self._read_search_result(board, return_bestmove=True)
        return PlayResult(move=bestmove, ponder=ponder)

    def _read_search_result(self, board, return_bestmove: bool = False):
        last_info: dict = {}
        bestmove_token = None
        ponder_token = None
        while True:
            line = self._read_line()
            tokens = line.split()
            if not tokens:
                continue
            if tokens[0] == "bestmove":
                bestmove_token = tokens[1] if len(tokens) > 1 else None
                if len(tokens) > 3 and tokens[2] == "ponder":
                    ponder_token = tokens[3]
                break
            if tokens[0] != "info":
                continue
            parsed = _parse_info_line(tokens)
            if parsed is None:
                continue
            last_info = parsed
        if return_bestmove:
            return (
                last_info,
                _move_or_none(bestmove_token),
                _move_or_none(ponder_token),
            )
        info = dict(last_info)
        pv = info.pop("pv", None)
        info["pv"] = [move for move in (pv or []) if move is not None]
        score = info.get("score")
        if score is not None:
            info["score"] = PovScore(score, board.turn)
        return info

    def quit(self):
        if self._closed:
            return
        self._closed = True
        process = self._process
        if process.poll() is None:
            try:
                process.stdin.write("quit\n")
                process.stdin.flush()
            except Exception:
                pass
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        for stream in (process.stdin, process.stdout, process.stderr):
            try:
                if stream is not None:
                    stream.close()
            except Exception:
                pass
        for thread in (self._reader_thread, self._stderr_thread):
            if thread is not threading.current_thread():
                thread.join(timeout=2)

    close = quit

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_value, traceback):
        self.quit()
        return False

    def __del__(self):  # pragma: no cover - defensive cleanup
        try:
            self.quit()
        except Exception:
            pass


def _move_or_none(token):
    if token is None or token in _TERMINATION_TOKENS:
        return None
    try:
        return Move.from_uci(token)
    except ValueError:
        return None


def _parse_info_line(tokens):
    info: dict = {}
    score = None
    pv: list = []
    index = 1
    while index < len(tokens):
        token = tokens[index]
        if token == "pv":
            pv = [_move_or_none(move) for move in tokens[index + 1 :]]
            index = len(tokens)
        elif token == "score" and index + 2 < len(tokens):
            kind = tokens[index + 1]
            if kind == "cp":
                score = Cp(_parse_int(tokens[index + 2]))
            elif kind == "mate":
                score = Mate(_parse_int(tokens[index + 2]))
            index += 3
        elif token in _INFO_INTEGER_KEYS and index + 1 < len(tokens):
            info[token] = _parse_int(tokens[index + 1])
            index += 2
        elif token in _INFO_FLAG_KEYS:
            info[token] = True
            index += 1
        elif token == "string" and index + 1 < len(tokens):
            info["string"] = " ".join(tokens[index + 1 :])
            index = len(tokens)
        else:
            index += 1
    if score is None and not pv and not info:
        return None
    if score is not None:
        info["score"] = score
    info["pv"] = pv
    return info
