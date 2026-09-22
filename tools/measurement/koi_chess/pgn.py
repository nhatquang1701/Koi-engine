"""Minimal PGN reading for the koi_chess compatibility library.

Only the surfaces Koi's tooling used are implemented: :func:`read_game` returns
a :class:`Game` exposing ``headers``, ``errors`` and ``mainline()``; mainline
:class:`Node` objects carry ``move``, ``comment`` and ``nags``; recursive
annotation variations are skipped; and ``Game.board()`` returns a copy of the
setup position (honouring a ``FEN`` header the way python-chess does).
"""

from __future__ import annotations

import re
import weakref

from .board import Board
from .core import Move

__all__ = ["Game", "Node", "read_game"]

DEFAULT_HEADER_TAGS = ("Event", "Site", "Date", "Round", "White", "Black", "Result")
DEFAULT_HEADER_VALUES = {
    "Event": "?",
    "Site": "?",
    "Date": "????.??.??",
    "Round": "?",
    "White": "?",
    "Black": "?",
    "Result": "*",
}
RESULT_TOKENS = frozenset({"1-0", "0-1", "1/2-1/2", "*"})
_DELIMITERS = "{}();$"
_HEADER_RE = re.compile(r'^\[([^"\]\s]+)\s+"(.*)"\]\s*$')


class Node:
    """A mainline move with its annotations, mirroring ``chess.pgn.Node``."""

    __slots__ = ("parent", "variations", "move", "comment", "nags", "starting_comment")

    def __init__(self, parent, move):
        self.parent = parent
        self.variations = []
        self.move = move
        self.comment = ""
        self.nags = []
        self.starting_comment = ""

    def mainline(self):
        """Yield the nodes along the mainline below this node."""
        node = self
        while node.variations:
            node = node.variations[0]
            yield node

    def board(self):
        """Return a copy of the board at this node."""
        moves = []
        node = self
        while node.parent is not None:
            if node.move is not None:
                moves.append(node.move)
            node = node.parent
        board = node._board.copy() if isinstance(node, Game) else Board()
        for move in reversed(moves):
            board.push(move)
        return board

    def __repr__(self) -> str:
        move = self.move.uci() if self.move is not None else None
        return f"<Node {move!r}>"


class Game(Node):
    """A single PGN game, mirroring the ``chess.pgn.Game`` surface Koi used."""

    __slots__ = ("headers", "errors", "_board")

    def __init__(self):
        super().__init__(None, None)
        self.headers = {tag: DEFAULT_HEADER_VALUES[tag] for tag in DEFAULT_HEADER_TAGS}
        self.errors = []
        self._board = Board()

    def board(self):
        """Return a copy of the game's setup position."""
        return self._board.copy()

    def __repr__(self) -> str:
        return f"<Game {self.headers.get('Event', '?')!r}>"


def _strip_eol(line: str) -> str:
    return line.rstrip("\r\n")


def _brace_depth_change(text: str, depth: int) -> int:
    """Return the comment-nesting depth after scanning one movetext line."""
    index = 0
    in_comment = False
    while index < len(text):
        char = text[index]
        if in_comment:
            if char == "}":
                in_comment = False
                depth -= 1
        elif char == "{":
            in_comment = True
            depth += 1
        elif char == "}":
            depth = max(0, depth - 1)
        elif char == ";":
            break
        index += 1
    return depth


def _tokenize_movetext(text: str):
    """Split movetext into move/result tokens while skipping variations."""
    tokens = []
    index = 0
    length = len(text)
    while index < length:
        char = text[index]
        if char.isspace():
            index += 1
        elif char == "{":
            end = text.find("}", index + 1)
            if end < 0:
                end = length
            comment = text[index + 1 : end]
            if tokens and "san" in tokens[-1]:
                existing = tokens[-1]["comment"]
                tokens[-1]["comment"] = f"{existing} {comment}".strip() if existing else comment
            index = end + 1
        elif char == ";":
            end = text.find("\n", index + 1)
            index = length if end < 0 else end + 1
        elif char == "(":
            depth = 1
            cursor = index + 1
            while cursor < length and depth:
                if text[cursor] == "(":
                    depth += 1
                elif text[cursor] == ")":
                    depth -= 1
                cursor += 1
            index = cursor
        elif char == "$":
            cursor = index + 1
            while cursor < length and text[cursor].isdigit():
                cursor += 1
            if tokens and "san" in tokens[-1] and cursor > index + 1:
                tokens[-1]["nags"].append(int(text[index + 1 : cursor]))
            index = cursor
        else:
            start = index
            cursor = index
            while (
                cursor < length
                and not text[cursor].isspace()
                and text[cursor] not in _DELIMITERS
            ):
                cursor += 1
            if cursor == index:
                # A stray delimiter such as a closing parenthesis.
                index += 1
                continue
            token = text[index:cursor]
            index = cursor
            if token in RESULT_TOKENS:
                tokens.append({"result": token, "offset": start})
                continue
            digits_end = 0
            while digits_end < len(token) and token[digits_end].isdigit():
                digits_end += 1
            if digits_end:
                dots_end = digits_end
                while dots_end < len(token) and token[dots_end] == ".":
                    dots_end += 1
                if dots_end == len(token):
                    continue  # A pure move number such as "23...".
                san = token[dots_end:]
            else:
                san = token
            tokens.append({"san": san, "comment": "", "nags": []})
    return tokens


class _GameReader:
    """A line reader with a single-line pushback, kept per stream."""

    __slots__ = ("stream", "pending")

    def __init__(self, stream):
        self.stream = stream
        self.pending = None

    def readline(self):
        if self.pending is not None:
            line = self.pending
            self.pending = None
            return line
        return self.stream.readline()

    def unread(self, line):
        self.pending = line


_READERS = weakref.WeakKeyDictionary()


def _reader_for(stream):
    try:
        reader = _READERS.get(stream)
    except TypeError:
        # Streams that cannot be weakly referenced still work for a single game.
        return _GameReader(stream)
    if reader is None:
        reader = _GameReader(stream)
        _READERS[stream] = reader
    return reader


def read_game(stream):
    """Read the next game from a text stream, or return ``None`` at EOF."""
    reader = _reader_for(stream)
    line = reader.readline()
    while line:
        stripped = _strip_eol(line)
        if not stripped.strip() or stripped.lstrip().startswith("%"):
            line = reader.readline()
            continue
        break
    if not line:
        return None

    game = Game()
    explicit_headers = set()
    if _strip_eol(line).lstrip().startswith("["):
        while line:
            stripped = _strip_eol(line).strip()
            if not stripped.startswith("["):
                break
            match = _HEADER_RE.match(stripped)
            if match:
                key, value = match.group(1), match.group(2)
                game.headers[key] = value
                explicit_headers.add(key)
            line = reader.readline()
        while line:
            stripped = _strip_eol(line)
            if stripped.strip() and not stripped.lstrip().startswith("%"):
                break
            line = reader.readline()
        if line and _strip_eol(line).lstrip().startswith("["):
            # The next game's header block begins immediately: leave it alone.
            reader.unread(line)
            line = ""

    movetext_parts = []
    brace_depth = 0
    while line:
        content = _strip_eol(line)
        if not content.strip() and brace_depth == 0:
            break
        if not movetext_parts and content.lstrip().startswith("%"):
            line = reader.readline()
            continue
        movetext_parts.append(content)
        brace_depth = _brace_depth_change(content, brace_depth)
        line = reader.readline()

    movetext_text = "\n".join(movetext_parts)
    tokens = _tokenize_movetext(movetext_text)
    result_token = None
    result_index = None
    for index, token in enumerate(tokens):
        if "result" in token:
            result_token = token
            result_index = index
            break
    if result_token is not None:
        if "Result" not in explicit_headers:
            game.headers["Result"] = result_token["result"]
        # python-chess keeps parsing tokens that follow the result on the same
        # line but discards the remaining lines up to the blank separator.
        offset = result_token.get("offset")
        tail = ""
        if offset is not None:
            line_end = movetext_text.find("\n", offset)
            tail_start = offset + len(result_token["result"])
            tail = movetext_text[tail_start : line_end if line_end >= 0 else len(movetext_text)]
        tail_tokens = [token for token in _tokenize_movetext(tail) if "san" in token]
        tokens = tokens[:result_index] + tail_tokens

    fen = game.headers.get("FEN")
    if fen is not None:
        try:
            game._board = Board(str(fen))
        except ValueError as error:
            game.errors.append(error)
            return game

    board = game._board.copy()
    node = game
    for token in tokens:
        san = token.get("san")
        if san is None:
            continue
        try:
            move = board.parse_san(san)
        except ValueError as error:
            game.errors.append(error)
            break
        board.push(move)
        child = Node(node, move)
        child.comment = token["comment"]
        child.nags = list(token["nags"])
        node.variations.append(child)
        node = child
    return game
