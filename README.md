# Koi Engine v1

Koi Engine v1 is a Windows x64 UCI chess engine for standard chess. It is
written in C++26 and uses deterministic iterative-deepening alpha-beta search
with a classical evaluator and a persistent transposition table. Search runs on
a cancellable outer worker; `Threads > 1` enables deterministic root-parallel
work while the UCI command loop remains responsive.

## Architecture

The engine is deliberately layered so the chess rules implementation remains a
private dependency. Public Koi rules types (`Move`, `GameState`, `Position`)
never expose `chess.hpp`; `GameState` converts to the vendored chess-library
only in its implementation. `ClassicalEvaluator`, time management, the
transposition table, and the `SearchService` build on those Koi
types. The UCI controller owns the current position and worker lifecycle, and
is the only layer that writes protocol output.

Search ordering is also an internal search concern: TT best moves are tried
first, followed by MVV-LVA captures/promotions, two killer moves, and quiet-move
history. Stable UCI-coordinate tie-breaking keeps repeated searches
deterministic. There is no public rule API for these policies; `Threads` and
`Speed` are UCI controls over the search runtime.

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
.\out\release-vs\koi-bench.exe --threads 4 --speed 100 --timed
.\out\release-vs\koi-bench.exe --optional --profile-json optional-strength.json
.\out\release-vs\koi-replay.exe startpos moves e2e4 e7e5 g1f3
```

`koi-perft` counts legal nodes from the standard starting position at the given
non-negative depth. `koi-bench` runs the 64-position fixed-depth tactical hard
gate and writes only its deterministic benchmark report to stdout by default.
`--optional` instead selects the 128-position optional strength corpus and labels
that suite in both its text and JSON-profile output. `--threads` and `--speed`
select a benchmark configuration; `--timed` adds wall-clock timing fields.
Untimed JSON profiles use the stable `Koi Engine 1.0` build identity and record
`nps` as unmeasured (`0`); `--timed` adds `elapsed_ms` and measured NPS. It is a
separate process and never writes to the UCI engine's stdout.

`koi-replay` is a separate rules-boundary tool for replaying coordinate moves without
exposing the vendored chess library. Give it `startpos` or `fen <six-field FEN>`, then
an optional `moves` list. Its stable stdout reports `legal`, `result`, `termination`,
and the final six-field `fen`; an illegal move leaves the reported position at the
last legal state. It is useful for reproducing a match-ply or validating a UCI log.

For a reproducible local match against Stockfish or another UCI engine, use the
optional PowerShell harness:

```powershell
.\tools\uci_match.ps1 `
  -KoiPath .\out\release-vs\koi-engine.exe `
  -OpponentPath C:\Engines\stockfish.exe `
  -Depth 6 -Threads 4 -Speed 100 -Hash 512 `
  -OutputDirectory .\match-results
```

The harness writes a `koi-uci-match-v2` JSON report plus matching `.pgn`. It records
both engines' UCI handshakes and options; every ply's actual root FEN, exact
`position` and `go` commands, engine label, returned move, replay legality, timing,
parsed final info/PV, all info lines, and raw `bestmove`; and each game's adjudicated
result, winner, termination, and process status. Moves are replay-validated before
they are appended, so illegal moves and non-terminal `0000` replies end only that game
without contaminating later positions. PGN Result headers match the adjudicated result
(`*` for max-ply games) and retain the `MoveFormat` header. Use `-MovetimeMs` or
`-Nodes` instead of `-Depth` for those limits. To run a named FEN suite, pass
`-FenFile` containing one entry per line in the form `name | six-field FEN` (blank
lines and `#` comments are ignored). The replay executable is expected beside
`koi-engine.exe` (or can be supplied as `-ReplayPath`).

For paired Elo measurements, supply the checked-in eight-opening suite (or another
file in the same `name | uci move uci move` format) and a chess clock:

```powershell
.\tools\uci_match.ps1 `
  -KoiPath .\out\release-vs\koi-engine.exe `
  -OpponentPath C:\Engines\stockfish.exe `
  -OpeningFile .\tests\data\elo-openings.txt `
  -TimeControl 1+0 -KoiColor black `
  -KoiRandomSeed 1 -KoiOwnBook false `
  -OutputDirectory .\match-results
```

`-TimeControl` accepts only `<minutes>+<increment>` (for example `1+0` or `5+3`)
and sends `wtime`, `btime`, `winc`, and `binc` on every `go`; elapsed time is deducted
from the moving side before its increment is added. Opening sequences are replayed and
rejected before either engine starts a game. The JSON configuration records the clock
and Koi `RandomSeed`, `OwnBook`, `BookFile`, and `BookDepth` values. Per-ply
`book_used` and `book_move` record an `info string book move <uci> depth <ply>` marker
separately from normal search PV data. The harness sends Koi's book options without
requiring a book reader or any Jack dependency; Koi versions predating book support
ignore those UCI options safely.

## Strength regression suite

The deterministic `Threads=1`, `Speed=100` reference path includes a fixed-depth
64-case tactical hard gate and a separate 128-case optional positional corpus. The
hard gate covers mates, checks, evasions, forks, pins, poisoned captures, promotions,
defensive choices, and pawn-race motifs, plus the retained legacy queen-capture case.
Each fixture records a stable ID, distinct legal FEN, depth,
category, and explicit accepted-move allowlist; multi-solution positions do not depend
on one arbitrary root tie-break.

Run the hard gate and the complete Release suite from an x64 Visual Studio developer
shell:

```powershell
.\out\release-vs\koi_strength_tests.exe
ctest --test-dir out\release-vs -C Release --output-on-failure
```

The optional corpus is retained for local tuning and is deliberately not an Elo or NPS
CI threshold. NNUE, tablebases, and chess variants remain deferred; opening-book
defaults, placement, fallback, and bypass behavior are documented below. This engine
continues to evaluate standard chess with its classical evaluator.

### Local verification baseline (2026-09-04)

Fresh Ninja builds were configured outside the checkout with Visual Studio 2022
MSVC 19.44.35227.0 targeting x64, `/std:c++latest`, and CMake 4.4.2. Both Debug
and Release builds completed. All 14 registered CTest targets passed in each
configuration: Debug in 73.24 seconds and Release in 32.18 seconds. This includes
the process-level UCI transcript and benchmark tests, the book unit tests, and the
64-case tactical suite.

The Release hard gate was run directly at `Threads` 1, 2, and 4 (`Speed 100`): all
three runs accepted 64/64 fixtures. Threads 1 and 2/4 had identical fixed-depth
move and score rows, which is the deterministic-threading check; node counts and
wall time are intentionally not expected to match. The optional 128-position
corpus also completed locally (34 accepted fixture moves; it is diagnostic rather
than a pass/fail Elo gate). A timed Release run at `Threads 2`, `Speed 50` visited
181,322 nodes plus quiescence nodes over 155 ms (about 1.17M visited nodes/s when
aggregated). NPS is sensitive to CPU load, timer granularity, thread count, hash
warmth, and speed settings; untimed profiles deliberately report NPS as zero. No
approved historical NPS baseline was available locally, so a 20% performance-floor
comparison has not been claimed.

The automated UCI harness verified the handshake, analysis-mode and tutor `MultiPV`
output, ponder/`ponderhit`, book hit and missing-book fallback, `stop`, `quit`, and
input EOF. A supplied executable-relative Polyglot book gives a legal weighted
choice that repeats for a nonzero seed; normal play defaults to `OwnBook=true`,
`BookFile=book.bin`, and `BookDepth=16`. Put `book.bin` beside `koi-engine.exe`.
A missing, malformed, unusable, disabled, or depth-exhausted book falls through to
normal search and never prevents startup. Analysis mode, `go infinite`, `go ponder`,
and `searchmoves` deliberately bypass the book.

The recorded `Get-Command stockfish` and `Get-Command Stockfish` checks returned no
executable, and no supplied UCI opponent was available, so no 40-game paired
1+0/5+3 measurements were fabricated. To perform them, supply an opponent and run
the paired command in [Developer tools](#developer-tools) once with
`-KoiOwnBook false`, then again with the licensed `book.bin` and `-KoiOwnBook true`;
retain the JSON and PGN output outside the checkout. Lucas Chess was not installed
or accessible in this environment, so manual registration was not performed. Use
the Release `koi-engine.exe` and the Lucas settings listed below (`Hash 512`,
`Threads 4`, `Speed 100`, `OwnBook true`, `BookFile book.bin`, `BookDepth 16`) to
complete that GUI check.

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
`RandomSeed`, `Hash`, `Threads`, `Speed`, `UCI_AnalyseMode`, `MultiPV`, `Ponder`,
and `Clear Hash` option declarations, `uciok`, `readyok`,
zero or more valid `info` lines, and one legal coordinate-notation `bestmove`
from the starting position (for example, `bestmove e2e4`).

## Supported UCI behavior

- `uci` reports the engine identity, the compatibility `RandomSeed` option,
  `Hash` (default 16 MB, range 1–4096 MB), `Threads` (default 1, capped at
  `min(64, hardware_concurrency)`), `Speed` (1–100, default 100), and the
  `Clear Hash` button.
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
- `setoption name Threads value <N>` selects the deterministic root worker
  count. `setoption name Speed value <1..100>` scales only movetime and
  clock-derived budgets; explicit depth, node, and infinite searches are
  unchanged. Changing either option stops and joins the active search before
  the new snapshot is used by the next `go` command.
- `go` accepts `depth`, `nodes`, `movetime`, `wtime`, `btime`, `winc`, `binc`,
  `movestogo`, and `infinite`. Malformed limit values are ignored. A bare `go`
  uses a 250 ms move-time fallback, scaled by `Speed`. If a clock is supplied
  only for the non-moving side, Koi uses the same bounded fallback so malformed
  or asymmetric GUI commands cannot leave the engine searching indefinitely.
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

Recommended starting settings for a machine with sufficient memory are:

```text
setoption name Hash value 512
setoption name Threads value 4
setoption name Speed value 100
```

The hash is one shared total table and is not multiplied by the thread count.
Reduce `Hash` or `Threads` if other applications need the memory or CPU.

### Lucas Chess workflows

Koi enables its Polyglot opening book by default. Place `book.bin` beside
`koi-engine.exe`; a relative `BookFile` is resolved from that executable
directory, not Lucas Chess's working directory. The relevant UCI options are:

```text
setoption name OwnBook value true
setoption name BookFile value book.bin
setoption name BookDepth value 16
```

`BookDepth 0` leaves the book unlimited; values from `1` through `40` limit
the exclusive root ply depth. On a hit Koi writes
`info string book move <uci> depth <ply>` followed by that one legal
`bestmove`. A missing or invalid book silently falls back to search.

For normal play, set the position supplied by Lucas Chess and use its clock
limits, for example `go wtime 60000 btime 60000`. Koi returns one final legal
`bestmove` for that search.

For continuous analysis, enable analysis mode and start an infinite search:

```text
setoption name UCI_AnalyseMode value true
go infinite
```

Send `stop` when analysis is no longer needed. `go infinite` is analysis mode:
it continues until stopped rather than completing from a time or depth limit.

For tutor-style analysis, request multiple principal variations:

```text
setoption name MultiPV value 3
go depth 12
```

Koi emits one `info` line per principal variation, with `multipv 1` as the
best-ranked line. Use `go ... searchmoves e2e4` (with any legal coordinate
moves required) to restrict the legal root moves considered by that search.
Opening-book selection is intentionally disabled for `UCI_AnalyseMode`,
`go infinite`, `go ponder`, and any `go ... searchmoves ...` command, so those
Lucas Chess tutor and analysis workflows always use search results.

For ponder support, enable it before Lucas Chess begins pondering:

```text
setoption name Ponder value true
go ponder wtime 60000 btime 60000
ponderhit
```

In v1, `ponderhit` safely restarts the search from the saved root instead of
retaining speculative ponder work. Send `stop` if the expected move did not
arrive or the GUI cancels the ponder search.

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
