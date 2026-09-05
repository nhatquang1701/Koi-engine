# Koi Engine v1

Koi Engine v1 is a Windows x64 UCI chess engine for standard chess. It is
written in C++26 and is documented and process-tested against En Croissant as
the primary GUI workflow. It uses deterministic iterative-deepening alpha-beta search
with a classical evaluator and a persistent transposition table. Search runs on
a cancellable outer worker; `Threads > 1` enables deterministic speculative
root-parallel work with serial reference confirmation while the UCI command loop
remains responsive.

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

For an independently reproducible release gate, run the checked-in harness from
an x64 Visual Studio developer shell. It configures and builds fresh external
Debug and Release trees, runs all CTest/process tests, checks tactical
Threads 1/2/4 when the host supports them (with an explicit maximum-thread
fallback), and writes benchmark, UCI, replay, and Lucas-style artifacts outside
the checkout:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\task5_release_verify.ps1 `
  -CMakePath "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" `
  -OutputDirectory C:\Koi-results\task5-run
```

The harness rejects an output directory inside the repository and records the
exact Debug/Release configure, build, CTest, benchmark, transcript, replay, and
match command outputs under the supplied external directory. It makes no
Stockfish/CPL or Lucas GUI availability assumption.

## Developer tools

From the configured build directory:

```powershell
.\out\release-vs\koi-perft.exe 4
.\out\release-vs\koi-bench.exe
.\out\release-vs\koi-bench.exe --threads 4 --speed 100 --timed
.\out\release-vs\koi-bench.exe --optional --profile-json C:\Koi-results\optional-strength.json
.\out\release-vs\koi-replay.exe startpos moves e2e4 e7e5 g1f3
```

`koi-perft` counts legal nodes from the standard starting position at the given
non-negative depth. `koi-bench` runs the 64-position fixed-depth tactical hard
gate and writes only its deterministic benchmark report to stdout by default.
`--optional` instead selects the 128-position optional strength corpus and labels
that suite in both its text and JSON-profile output. `--threads N` and `--speed
1..100` are recorded in every report and use the same deterministic search
configuration as the corresponding engine controls. Every text report includes
`hash cold` or `hash warm`; `--warm-hash` reuses one search service across rows
and is useful for comparing warmed-table behavior. The default is cold.
`--timed` is opt-in and adds wall-clock `elapsed_ms` and measured NPS to text and
JSON; it is intentionally absent from the default CI-shaped output. Untimed JSON
profiles use the stable `Koi Engine 1.0` build identity, set `timed` to `false`,
and record `nps` as unmeasured (`0`). Each profile carries `hash_state` (`cold` or
`warm`) at the top level and on every position. It is a separate process and never
writes to the UCI engine's stdout.

`koi-replay` is a separate rules-boundary tool for replaying coordinate moves without
exposing the vendored chess library. Give it `startpos` or `fen <six-field FEN>`, then
an optional `moves` list. Its stable stdout reports `legal`, `result`, `termination`,
and the final six-field `fen`; an illegal move leaves the reported position at the
last legal state. It is useful for reproducing a match-ply or validating a UCI log.

The Stockfish position oracle is a measurement-only Python tool and does not add a
dependency to the C++ engine. Install its pinned dependency and run its test directly
from the repository root:

```powershell
python -m pip install -r .\tools\requirements-elo-oracle.txt
python -m unittest .\tests\elo_oracle_test.py -v
```

See [`tools/README.md`](tools/README.md) for the extract-only schema, the named
book-audit and match entrypoints, and the external-results workflow.

When Python 3 and `python-chess` are available at CMake configure time, the same test
is registered as `elo_oracle_python` in CTest; otherwise the C++ test suite is unchanged
and CMake reports that the optional test was skipped.

To analyze a supplied standard-SAN PGN with Stockfish as the position oracle, keep the
JSON output outside this checkout and provide the exact executable/version and PGN
provenance alongside the report:

```powershell
python .\tools\elo_oracle.py `
  --pgn C:\Koi-inputs\game.pgn `
  --koi .\out\release-vs\koi-engine.exe `
  --stockfish C:\Engines\stockfish.exe `
  --output C:\Koi-results\elo-oracle.json `
  --movetime-ms 250 --threads 4
```

Use `--extract-only` to test PGN/FEN extraction without either engine. Run the licensed
opening book as a separate audit so its `book_used`, `book_move`, and Stockfish CPL are
kept outside the normal search metrics:

```powershell
python .\tools\elo_oracle.py `
  --pgn C:\Koi-inputs\game.pgn `
  --koi .\out\release-vs\koi-engine.exe `
  --stockfish C:\Engines\stockfish.exe `
  --book C:\LicensedBooks\book.bin --book-audit `
  --output C:\Koi-results\book-audit.json `
  --movetime-ms 250 --threads 4
```

The PGN, Stockfish executable/version, and licensed `book.bin` are external inputs;
none are assumed to exist in this repository. Do not report an Elo or CPL improvement
until both a comparable baseline and an after-change report have been generated.

For a reproducible local match against Stockfish or another UCI engine, use the
optional PowerShell harness:

```powershell
.\tools\uci_match.ps1 `
  -KoiPath .\out\release-vs\koi-engine.exe `
  -OpponentPath C:\Engines\stockfish.exe `
  -Depth 6 -Threads 4 -Speed 100 -Hash 512 `
  -OutputDirectory C:\Koi-results\match-results
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

For color-balanced paired Elo measurements, supply the checked-in eight-opening
suite (or another file in the same `name | uci move uci move` format) and a chess
clock. `-Games` is the number of games run for each selected opening, so use 20 as
Koi White and 20 as Koi Black for 40 games per opening and condition. Run both
book-disabled and licensed-book conditions at both clocks:

```powershell
.\tools\uci_match.ps1 `
  -KoiPath .\out\release-vs\koi-engine.exe `
  -OpponentPath C:\Engines\stockfish.exe `
  -OpeningFile .\tests\data\elo-openings.txt `
  -TimeControl 1+0 -Games 20 -KoiColor white `
  -KoiRandomSeed 1 -KoiOwnBook false `
  -OutputDirectory C:\Koi-results\no-book-1p0-white
.\tools\uci_match.ps1 -KoiPath .\out\release-vs\koi-engine.exe -OpponentPath C:\Engines\stockfish.exe -OpeningFile .\tests\data\elo-openings.txt -TimeControl 1+0 -Games 20 -KoiColor black -KoiRandomSeed 1 -KoiOwnBook false -OutputDirectory C:\Koi-results\no-book-1p0-black
.\tools\uci_match.ps1 -KoiPath .\out\release-vs\koi-engine.exe -OpponentPath C:\Engines\stockfish.exe -OpeningFile .\tests\data\elo-openings.txt -TimeControl 1+0 -Games 20 -KoiColor white -KoiRandomSeed 1 -KoiOwnBook true -KoiBookFile C:\LicensedBooks\book.bin -KoiBookDepth 16 -OutputDirectory C:\Koi-results\book-1p0-white
.\tools\uci_match.ps1 -KoiPath .\out\release-vs\koi-engine.exe -OpponentPath C:\Engines\stockfish.exe -OpeningFile .\tests\data\elo-openings.txt -TimeControl 1+0 -Games 20 -KoiColor black -KoiRandomSeed 1 -KoiOwnBook true -KoiBookFile C:\LicensedBooks\book.bin -KoiBookDepth 16 -OutputDirectory C:\Koi-results\book-1p0-black
.\tools\uci_match.ps1 -KoiPath .\out\release-vs\koi-engine.exe -OpponentPath C:\Engines\stockfish.exe -OpeningFile .\tests\data\elo-openings.txt -TimeControl 5+3 -Games 20 -KoiColor white -KoiRandomSeed 1 -KoiOwnBook false -OutputDirectory C:\Koi-results\no-book-5p3-white
.\tools\uci_match.ps1 -KoiPath .\out\release-vs\koi-engine.exe -OpponentPath C:\Engines\stockfish.exe -OpeningFile .\tests\data\elo-openings.txt -TimeControl 5+3 -Games 20 -KoiColor black -KoiRandomSeed 1 -KoiOwnBook false -OutputDirectory C:\Koi-results\no-book-5p3-black
.\tools\uci_match.ps1 -KoiPath .\out\release-vs\koi-engine.exe -OpponentPath C:\Engines\stockfish.exe -OpeningFile .\tests\data\elo-openings.txt -TimeControl 5+3 -Games 20 -KoiColor white -KoiRandomSeed 1 -KoiOwnBook true -KoiBookFile C:\LicensedBooks\book.bin -KoiBookDepth 16 -OutputDirectory C:\Koi-results\book-5p3-white
.\tools\uci_match.ps1 -KoiPath .\out\release-vs\koi-engine.exe -OpponentPath C:\Engines\stockfish.exe -OpeningFile .\tests\data\elo-openings.txt -TimeControl 5+3 -Games 20 -KoiColor black -KoiRandomSeed 1 -KoiOwnBook true -KoiBookFile C:\LicensedBooks\book.bin -KoiBookDepth 16 -OutputDirectory C:\Koi-results\book-5p3-black
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

### Task 5 release verification (2026-09-05)

Fresh external Ninja builds used Visual Studio 2022 MSVC 19.44.35228.0 targeting
x64, `/std:c++latest`, and CMake 4.4.2. Debug and Release each configured and
built from scratch; all 17 registered CTest targets passed. Debug completed in
84.74 seconds and Release in 43.44 seconds. This includes all C++ tests, process
tests, replay validation, UCI transcripts, benchmark/profile checks, and optional
Python tests.

The fresh Release hard gate ran at `Threads` 1, 2, and 4 with `Speed 100`: each
reported 64/64 accepted tactical rows, and normalized move/score rows were
identical across all three runs. Node counts and elapsed times are intentionally
not required to match. The optional 128-position profile completed with 32
accepted rows and is diagnostic rather than a pass/fail Elo gate. A timed Release
run at `Threads 4`, `Speed 100` visited 183,807 nodes plus quiescence nodes over
157 ms (about 1.17M visited nodes/s aggregated). Timing is machine-sensitive;
untimed profiles report NPS as zero and identify `hash_state` as `cold` or `warm`.

The fresh UCI smoke transcript produced 26 lines, 21 option declarations, one
legal coordinate `bestmove`, and empty stderr. CTest and the direct Release
process checks covered the handshake, analysis/tutor `MultiPV`, ponder/`ponderhit`,
book hit and missing-book fallback, `stop`, `quit`, input EOF, and clean stdout.
The fresh Lucas-style process scenario completed 24 legal plies with `Hash=512`,
`Threads=4`, and `Speed=100`; both engine processes shut down cleanly. Direct
replay output classified the repeated knight sequence as a legal rule draw.

No Stockfish executable, fresh CPL corpus, or fresh color-balanced match data was
available in this environment. Therefore this release verification makes no Elo,
CPL, or playing-strength improvement claim. Lucas Chess itself was not installed
for GUI automation; use the Release executable and the settings below for the
remaining manual registration/play check.

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
`OwnBook`, `BookFile`, `BookDepth`, `BookRandom`, and `Clear Hash` option
declarations, `uciok`, `readyok`,
zero or more valid `info` lines, and one legal coordinate-notation `bestmove`
from the starting position (for example, `bestmove e2e4`).

## Supported UCI behavior

- `uci` reports the engine identity, the compatibility `RandomSeed` option,
  `Hash` (default 16 MB, range 1–4096 MB), `Threads` (default 1, capped at
  `min(64, hardware_concurrency)`), `Speed` (1–100, default 100), the opening
  book options, and the `Clear Hash` button.
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
- `setoption name BookRandom value false` (the default) selects the highest-
  weight legal Polyglot move, using deterministic coordinate ordering for equal
  weights. `BookRandom true` enables weighted random selection; `RandomSeed 0`
  is runtime-random only in that opt-in mode, while nonzero seeds remain
  repeatable.
- `setoption name BookSafety value true` (the default) runs a shallow forcing
  material probe before accepting a book move. A move that immediately hangs a
  valuable piece is rejected and normal search chooses the move. Set
  `BookSafetyDepth` from `0` through `3` to control the probe horizon; `0`
  disables the probe while retaining legal-move filtering. Safety never
  overrides analysis, MultiPV, ponder, infinite, or `searchmoves` book bypass.
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

### Timing controls

Koi separates search limits from time-allocation policy. Explicit `go depth`,
`go nodes`, and `go infinite` searches are not given an artificial time limit.
For `movetime` and clock searches, the requested budget is adjusted in this
order: `Slow Mover`, then `Speed`, then `Move Overhead` is subtracted, followed
by the existing safety margin and minimum safe budget. `Move Overhead` defaults
to 10 ms and accepts 0..5000; `Slow Mover` defaults to 100 and accepts 10..1000.
`Speed` defaults to 100 and accepts 1..100. These controls affect allocation,
not explicit depth or node limits, and changing one while searching cancels and
joins the old generation before the next search uses the new snapshot.

### WDL and strength controls

`UCI_ShowWDL` defaults to false. When enabled, ordinary `info` lines append a
deterministic `wdl W D L` triplet; it is omitted when disabled. `UCI_LimitStrength`
defaults to false and `UCI_Elo` defaults to 1320 with a 1320..3190 range. These
are Stockfish-compatible configuration controls. The current release keeps the
normal deterministic search path unchanged and does not add random weakening;
the strength hook is reserved for a later calibrated profile.

### Optional Syzygy tablebases

Syzygy support is optional and never requires tablebase files for build, startup,
or ordinary search. Set `SyzygyPath` to a directory containing licensed `.rtbw`
and `.rtbz` files. `SyzygyProbeLimit` accepts 0..5 pieces (default 5),
`SyzygyProbeDepth` accepts 1..100 (default 1), and `Syzygy50MoveRule` defaults to
true. An empty, missing, unreadable, malformed, over-limit, or unsupported
position safely falls back to normal search. Root WDL/DTZ selection is used only
for eligible single-PV play searches; analysis mode, `MultiPV > 1`, ponder, and
`searchmoves` retain their documented search paths. Successful probes may be
reported as `tbhits` in valid `info` lines. No tablebase data is distributed
with Koi.

### Hidden developer diagnostics

The unadvertised `Debug` check option and `DebugFile` string option are for local
diagnostics only. `Debug` defaults to false. With an empty `DebugFile`, Koi writes
`koi-debug.log` beside the executable; a relative path is also resolved beside
the executable, while an absolute path is used as supplied. Logs are best-effort,
rotate at 8 MiB, and retain three backups. Debug events never go to UCI stdout
or normal stderr, so a valid Lucas or automation transcript remains protocol
clean. Leave this option disabled for normal release use.

## Register in En Croissant (primary)

1. Build the Windows x64 Release target and resolve the absolute path to
   `out\release-vs\koi-engine.exe` (or the executable in your chosen build
   directory).
2. In En Croissant, add a UCI engine and select that `koi-engine.exe` path.
   Keep the engine's working directory beside the executable when configuring
   the engine so portable relative assets resolve predictably.
3. If using the opening book, place the user-supplied licensed `book.bin` in
   the same directory as `koi-engine.exe`. Do not add book data to this
   repository or redistribute it without its license.

Recommended starting options are:

```text
setoption name Hash value 512
setoption name Threads value 4
setoption name Speed value 100
setoption name OwnBook value true
setoption name BookFile value book.bin
setoption name BookDepth value 16
setoption name BookRandom value false
setoption name BookSafety value true
setoption name BookSafetyDepth value 2
```

For normal play, let En Croissant provide the position and clock limits. For
analysis, enable `UCI_AnalyseMode` and use `go infinite`; send `stop` when the
analysis view is closed. Tutor and MultiPV views should set `MultiPV` to the
number of variations requested (for example, `3`) and use a finite depth or
clock search. Analysis mode, `MultiPV > 1`, `go infinite`, ponder, and
`searchmoves` intentionally bypass the opening book so these views receive
search variations rather than a book move.

En Croissant uses the standard UCI protocol: the checked-in process transcript
covers the handshake, options, positions with moves, stopped searches,
MultiPV, infinite analysis, and clean quit with exactly one legal `bestmove`
per search. Any other standard UCI GUI can use the same executable and options.

## Register in Lucas Chess (generic UCI fallback)

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
setoption name BookRandom value false
setoption name BookSafety value true
setoption name BookSafetyDepth value 2
```

`BookDepth 0` leaves the book unlimited; values from `1` through `40` limit
the exclusive root ply depth. With `BookRandom false`, Koi chooses the
highest-weight legal move deterministically; equal weights use coordinate
ordering. Set `BookRandom true` only when weighted variety is wanted. On a hit Koi writes
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
Opening-book selection is intentionally disabled for `UCI_AnalyseMode`, `MultiPV`
values greater than one, `go infinite`, `go ponder`, and any `go ... searchmoves ...`
command, so those Lucas Chess tutor and analysis workflows always use search results.

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

## Configuration and release packaging

Koi has no required configuration file. Lucas Chess or another UCI GUI sends
the options at session start; the portable release defaults are `Hash=16`,
`Threads=1`, `Speed=100`, `OwnBook=true`, `BookFile=book.bin`, `BookDepth=16`,
`BookRandom=false`, `UCI_ShowWDL=false`, `Move Overhead=10`, `Slow Mover=100`,
`UCI_LimitStrength=false`, `UCI_Elo=1320`, and `Syzygy50MoveRule=true`.
For the recommended Lucas smoke scenario, use `Hash=512`, `Threads=4`, and
`Speed=100`, then keep the book and Syzygy paths explicitly configured if those
assets are available.

The Polyglot `book.bin` is an external licensed asset. Do not commit it, embed
it in the executable, or redistribute it as part of an unlicensed Koi archive.
For a release package, place the user-supplied `book.bin` beside `koi-engine.exe`
only when its license permits that distribution, and retain these repository
licenses with the package:

- `third_party/chess-library/LICENSE` for the vendored chess-library code.
- `third_party/fathom/LICENSE` for the optional Syzygy adapter code.

The release archive should also retain the README and the exact build identity.
Do not include generated benchmark profiles, match JSON/PGN, debug logs, or
tablebase data in the repository release commit; write those artifacts under an
external results directory such as `C:\Koi-results`. An Elo, CPL, or strength
claim requires fresh comparable Stockfish CPL or match data with the executable,
options, time control, and input provenance recorded alongside the report.

## References

- [Lucas Chess](https://lucaschess.pythonanywhere.com/)
- [Universal Chess Interface (UCI) reference](https://www.shredderchess.com/chess-info/features/uci-universal-chess-interface.html)
- [Disservin/chess-library](https://github.com/Disservin/chess-library)
- [C++ compiler support for C++26](https://en.cppreference.com/w/cpp/compiler_support/26)
