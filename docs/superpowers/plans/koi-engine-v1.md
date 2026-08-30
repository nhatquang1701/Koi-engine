# Koi Engine v1: Lucas Chess UCI Random-Move Baseline

## Summary

Create a Windows x64 chess engine in C++26 that Lucas Chess recognizes as a UCI engine. The engine will generate legal moves through vendored `Disservin/chess-library`, then select one uniformly at random as a placeholder strategy.

## Key Changes

- Set up CMake + Ninja with `CXX_STANDARD 26`; require a C++26-capable compiler.
- Vendor a pinned `chess.hpp` and preserve its MIT license.
- Separate UCI parsing, position management, the `MoveChooser` interface, and random move selection.
- Implement `uci`, `isready`, `ucinewgame`, `position`, `go`, `stop`, `setoption`, and `quit`.
- Support standard chess, including castling, en passant, promotion, checkmate, stalemate, and FEN.
- Use coordinate notation and return `bestmove 0000` when no legal moves exist.
- Keep stdout protocol-clean; diagnostics use stderr or valid `info string` messages.
- `RandomSeed 0` uses runtime randomness; nonzero seeds are repeatable.

## Test Plan

- Unit-test FEN loading and sequential UCI move application.
- Test castling, en passant, promotion, checkmate, and stalemate.
- Confirm every selected move is legal and seeded sequences are repeatable.
- Test malformed commands without crashes or hangs.
- Add a process-level UCI transcript test for the handshake and lifecycle.
- Build a Release executable and manually add it to Lucas Chess.

## Assumptions

- Engine name: `Koi Engine`.
- Author string: `Koi Engine contributors`.
- Time-control fields are accepted but ignored; v1 moves immediately.
- Chess960 and nonstandard variants are out of scope.

