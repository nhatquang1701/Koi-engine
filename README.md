# Koi Engine v1

Koi Engine v1 is a Windows x64 UCI chess engine for standard chess. It is
written in C++26 and uses deterministic iterative-deepening alpha-beta search
with a classical evaluator and a persistent transposition table. Search runs on
a cancellable worker so the UCI command loop remains responsive.

## Architecture

The engine is deliberately layered so the chess rules implementation remains a
private dependency. Public Koi rules types (`Move`, `GameState`, `Position`)
never expose `chess.hpp`; `GameState` converts to the vendored chess-library
only in its implementation. `ClassicalEvaluator`, time management, the
transposition table, and the single-worker `SearchService` build on those Koi
types. The UCI controller owns the current position and worker lifecycle, and
is the only layer that writes protocol output.

Search ordering is also an internal search concern: TT best moves are tried
first, followed by MVV-LVA captures/promotions, two killer moves, and quiet-move
history. Stable UCI-coordinate tie-breaking keeps repeated searches
deterministic. There is no public rule API for these policies and no advertised
`Threads` option while search remains single-worker.

## Build prerequisites

- A C++26-capable x64 MSVC toolchain (the current CMake configuration selects
  Visual Studio's `/std:c++latest` compiler mode). Run CMake from an x64
  Native Tools Command Prompt or x64 Developer PowerShell for Visual Studio,
  so `cl.exe` is selected; the currently available MinGW/GCC/Clang toolchains
  do not meet the C++26 requirement.
- CMake 3.31 or newer.
- Ninja.

From an x64 Visual Studio developer shell in the repository root:

```powershell
cmake -S . -B out\release-vs -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=cl
cmake --build out\release-vs --config Release
ctest --test-dir out\release-vs -C Release --output-on-failure
```

For a Debug build, substitute `debug-vs` and `Debug` in those commands. The
resulting engine executable is `out\release-vs\koi-engine.exe`.

## Developer tools

From the configured build directory:

```powershell
.\out\release-vs\koi-perft.exe 4
.\out\release-vs\koi-bench.exe
```

`koi-perft` counts legal nodes from the standard starting position at the given
non-negative depth. `koi-bench` runs fixed depth-3 positions and writes only a
deterministic benchmark report to its own stdout; it is a separate process and
never writes to the UCI engine's stdout.

## UCI smoke test

Run this PowerShell transcript after building:

```powershell
$engine = (Resolve-Path .\out\release-vs\koi-engine.exe).Path
@(
    'uci'
    'isready'
    'position startpos'
    'go depth 2'
    'stop'
    'quit'
) | & $engine
```

The output should include, in order, the engine identification lines, the
`RandomSeed`, `Hash`, and `Clear Hash` option declarations, `uciok`, `readyok`,
zero or more valid `info` lines, and one legal coordinate-notation `bestmove`
from the starting position (for example, `bestmove e2e4`).

## Supported UCI behavior

- `uci` reports the engine identity, the compatibility `RandomSeed` option,
  `Hash` (default 16 MB, range 1–4096 MB), and the `Clear Hash` button. It does
  not advertise `Threads` while search is single-worker.
- `isready` responds immediately with `readyok`, including while searching.
- `ucinewgame` resets the position; `position startpos` and `position fen ...`
  set a position, optionally followed by legal UCI moves. Replacing the root
  cancels and joins the old search without leaking its result.
- `setoption name RandomSeed value 0` uses runtime randomness (`RandomSeed 0`).
  A nonzero seed remains available to the compatibility random chooser, but
  normal `go` search is deterministic and does not use it.
- `setoption name Hash value <MB>` resizes the persistent search hash, and
  `setoption name Clear Hash` clears it. Either command stops and joins an
  active search before changing the table.
- `go` accepts `depth`, `nodes`, `movetime`, `wtime`, `btime`, `winc`, `binc`,
  `movestogo`, and `infinite`. Malformed limit values are ignored. A bare `go`
  defaults to bounded depth 1.
- A depth limit is capped internally at 64 plies. `nodes`, `movetime`, and
  side-to-move clock limits stop search at their requested boundary; `infinite`
  continues until `stop`.
- Search may report completed iterations as UCI `info depth ... score ... nodes
  ... nps ... time ... pv ...` lines.
- `stop` cancels and joins the active worker and emits exactly one final legal
  `bestmove` for that search.
- A terminal position with no legal moves returns `bestmove 0000`.
- `quit` and input EOF cancel and join the worker without late protocol output.

## Register in Lucas Chess

1. Build the engine and resolve the path to `koi-engine.exe`.
2. In Lucas Chess, open the engine-management or configuration dialog and add
   the built executable as an external UCI engine.
3. Save the engine configuration, then select Koi Engine for play.

Menu labels can vary by Lucas Chess version. Use an absolute executable path,
or another path that remains valid when Lucas Chess starts the engine.

Before registering it, check the process transcript: stdout should contain
only valid UCI responses, with no logging or diagnostics mixed into it. For
the smoke test, also confirm that stderr is empty for a valid transcript. If
Lucas Chess cannot start the engine, verify the executable path, that the
Windows x64 build exists, and that the process can complete the `uci` / `isready`
handshake from PowerShell.

For final Lucas acceptance, play at least one short standard game after the
handshake succeeds. Confirm that the GUI receives a legal move after `go`,
remains responsive while the engine is thinking, and can stop or start a new
game without a duplicate `bestmove`. This repository automates the UCI process
transcript but cannot automate a locally installed Lucas Chess GUI.

## References

- [Lucas Chess](https://lucaschess.pythonanywhere.com/)
- [Universal Chess Interface (UCI) reference](https://www.shredderchess.com/chess-info/features/uci-universal-chess-interface.html)
- [Disservin/chess-library](https://github.com/Disservin/chess-library)
- [C++ compiler support for C++26](https://en.cppreference.com/w/cpp/compiler_support/26)
