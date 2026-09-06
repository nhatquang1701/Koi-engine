# Koi measurement tools

These scripts are measurement-only helpers. They do not add a dependency to the
C++ engine, and generated JSON/PGN reports default to the machine temporary
directory (`%TEMP%\koi-results` on Windows) rather than this checkout. Keep PGN
files, Stockfish executables, and licensed books outside the repository as well.

## Fixed-depth performance gate

`task5_perf_gate.ps1` compares two explicitly supplied `koi-bench` executables
using repeated cold-hash, fixed-depth, timed runs of the 64-position strength
suite. It reports the median total elapsed time for each executable and fails
only when the candidate median is more than 5% slower. Profiles and logs must be
written outside the checkout, and this is intentionally a standalone
measurement command rather than a CTest threshold:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\task5_perf_gate.ps1 `
  -BaselineExecutable C:\Koi\baseline\koi-bench.exe `
  -CandidateExecutable C:\Koi\candidate\koi-bench.exe `
  -Runs 5 -Threads 4 -Speed 100 `
  -OutputDirectory C:\Koi-results\task5-perf
```

Install the pinned PGN dependency from the repository root:

```powershell
python -m pip install -r .\tools\requirements-elo-oracle.txt
python -m unittest .\tests\elo_oracle_test.py -v
```

## PGN extraction and oracle analysis

`elo_oracle.py` reads standard SAN PGN mainlines. Comments, NAGs, and recursive
variations are ignored by the mainline walk; castling, promotion, FEN setup
headers, side-to-move, move number, sequential ply, pre-move FEN, SAN, and UCI
move are retained in each position record.

Extract positions without starting either engine:

```powershell
python .\tools\elo_oracle.py --pgn C:\Koi-inputs\games.pgn --extract-only
```

The command prints the generated `report <path>` and writes a `koi-elo-oracle`
JSON report. Use `--output C:\Koi-results\extraction.json` when a stable path is
needed. For Stockfish comparison, provide both executable paths and an explicit
external output path:

```powershell
python .\tools\elo_oracle.py `
  --pgn C:\Koi-inputs\games.pgn `
  --koi C:\Koi\koi-engine.exe `
  --stockfish C:\Engines\stockfish.exe `
  --output C:\Koi-results\elo-oracle.json `
  --movetime-ms 250 --threads 4
```

The report records engine identity, handshake/options, input SHA-256, root FEN,
actual SAN/UCI move, Koi suggestion, Stockfish scores, timings, and separate CPL
for the actual and suggested move. Scores are normalized to White's perspective;
mate is represented as +/-100000 cp. Do not turn a CPL report into an Elo claim
without a comparable baseline and color-balanced match evidence.

## Licensed-book audit

Install the pinned CC0 opening book beside a specific engine executable with
the one-time installer:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\install_book.ps1 `
  -EnginePath C:\Koi\koi-engine.exe
```

`-EnginePath` is mandatory and must identify an existing local `.exe` file.
The installer downloads the pinned `lichess_1900_rapid_2026-05.bin` asset from
the `books-2026-05-v1` release, verifies SHA-256
`56abc70e5291b4338356009d380e565fd85eab8067f6bf34927b5807ff231370`, and
installs it as `book.bin` in the executable's directory. It refuses to replace
a different existing book unless `-Force` is supplied. Temporary downloads are
cleaned up, and Koi has no runtime network dependency.

`book_audit.py` is the named entrypoint for the existing separate book-audit mode.
It keeps book hits and book-move CPL out of ordinary search metrics and uses
`OwnBook=true`, `BookDepth=16`, and `BookRandom=false`:

```powershell
python .\tools\book_audit.py `
  --pgn C:\Koi-inputs\games.pgn `
  --koi C:\Koi\koi-engine.exe `
  --stockfish C:\Engines\stockfish.exe `
  --book C:\LicensedBooks\book.bin
```

Use `--output C:\Koi-results\book-audit.json` for a stable external path. The
book is an external licensed input; do not commit or redistribute it.

## Stockfish matches

`stockfish_match.py` is a safe Python entrypoint for the existing Windows
PowerShell UCI match harness. It uses `subprocess.run` with an argument list and
never invokes a shell. `--help` only parses arguments and does not start engines.
The default output directory is `%TEMP%\koi-results\matches`.

```powershell
python .\tools\stockfish_match.py --help
python .\tools\stockfish_match.py `
  --koi C:\Koi\koi-engine.exe `
  --stockfish C:\Engines\stockfish.exe `
  --opening-file .\tests\data\elo-openings.txt `
  --time-control 1+0 --games 20 --koi-color white `
  --own-book false --threads 4 --output-directory C:\Koi-results\match-1p0-white
```

For a reproducible Stockfish strength anchor, use Python's `--opponent-elo`
alias `--stockfish-elo`, or invoke the PowerShell harness directly with
`-OpponentElo` alias `-StockfishElo`. An explicit value is clamped to
1320..3190, enables `UCI_LimitStrength`, and sends `UCI_Elo` only to the
opponent. Omit the strength option to leave strength limiting disabled.

`--timeout-ms` / `-TimeoutMilliseconds` is a protocol read, readiness, and
shutdown bound. In a clocked match, the `--time-control` / `-TimeControl`
chess clock remains the deadline for `bestmove`.

Use `--depth`, `--movetime-ms`, or `--nodes` for fixed limits, and use
`--fen-file name | six-field FEN` through the existing harness for named FEN
positions. Run balanced conditions with Koi White and Koi Black, and record the
exact engine paths, options, time control, opening/FEN input, and generated JSON
and PGN together in the external results directory.

All three tools fail with an actionable stderr message and nonzero exit code for
missing inputs or engine/configuration errors. The C++ engine remains independent
of Python and `python-chess`.

## Rough local Elo estimation

`elo_estimate.py` is standard-library-only and consumes the existing
`koi-uci-match-v2` JSON/PGN reports. The primary measurement is no-book at 1+0
with Koi `Hash=512`, `Threads=4`, and `Speed=100`, using
`tests/data/elo-openings-32.txt` once in each Koi color for every anchor. Two
anchors produce 128 initial games (32 openings x 2 colors x 2 anchors); adaptive
64-game batches can extend the schedule through 320 games. The fit is Koi's
perspective with 2,000 paired-opening bootstrap samples and a documented anchor
bracket, not a universal rating claim.

The required external anchor manifest has this shape:

```json
{
  "schema": "koi-elo-anchor-manifest-v1",
  "stockfish": {
    "path": "C:\\Engines\\stockfish.exe",
    "elos": [1400, 1600, 1800],
    "rating_source": "Stockfish 19 UCI_LimitStrength"
  },
  "lower_anchors": []
}
```

`stockfish.path` must be an existing file matching `--stockfish`; `elos` must
contain at least two unique Stockfish `UCI_Elo` values in 1320..3190; and
`rating_source` is required. A lower anchor entry requires `id`, an existing
`path`, positive `rating`, and `rating_source`, and is required to bracket a
`--prior-elo` below the Stockfish floor. All anchors must bracket the prior.

Use this exact no-book dry-run CLI from the repository root:

```powershell
python .\tools\elo_estimate.py `
  --koi C:\Koi\out\release\koi-engine.exe `
  --replay C:\Koi\out\release\koi-replay.exe `
  --stockfish C:\Engines\stockfish.exe `
  --anchors C:\Koi-inputs\anchors.json `
  --openings .\tests\data\elo-openings-32.txt `
  --time-control 1+0 `
  --threads 4 --hash 512 --speed 100 `
  --min-games 128 --max-games 320 `
  --prior-elo 1600 --mode no-book `
  --output C:\Koi-results\rough-elo-no-book.json `
  --dry-run
```

Dry-run validates all inputs and writes the full deterministic schedule without
starting Koi, replay, Stockfish, or PowerShell. Remove `--dry-run` only when the
user-supplied executable, anchor, corpus, and external output paths are ready.
Raw JSON and matching PGN files are preserved below the external report's
`<stem>-artifacts` directory; do not commit them or place them under the
checkout.

Book mode is a separate run, with a separate report and external licensed book:

```powershell
python .\tools\elo_estimate.py `
  --koi C:\Koi\out\release\koi-engine.exe `
  --replay C:\Koi\out\release\koi-replay.exe `
  --stockfish C:\Engines\stockfish.exe `
  --anchors C:\Koi-inputs\anchors.json `
  --openings .\tests\data\elo-openings-32.txt `
  --time-control 1+0 `
  --threads 4 --hash 512 --speed 100 `
  --min-games 128 --max-games 320 `
  --prior-elo 1600 --mode book --book C:\LicensedBooks\book.bin `
  --output C:\Koi-results\rough-elo-book.json
```

Book mode records `OwnBook=true`, `BookFile`, `BookDepth=16`, and
`BookRandom=false`; it must not be merged with the no-book result. CTest covers
the estimator CLI logic, match-option forwarding, and all 32 corpus lines, but
real matches are not a CI job or a fixed Elo gate because runtime depends on the
recorded hardware, engine options, external executables, and time control. Any
reported value must be labeled “local Stockfish-equivalent Elo at recorded
hardware/options/time control” and must not be presented as a universal Elo
claim.
