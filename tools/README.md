# Koi measurement tools

These scripts are measurement-only helpers. They do not add a dependency to the
C++ engine, and generated JSON/PGN reports default to the machine temporary
directory (`%TEMP%\koi-results` on Windows) rather than this checkout. Keep PGN
files, Stockfish executables, and licensed books outside the repository as well.

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

Use `--depth`, `--movetime-ms`, or `--nodes` for fixed limits, and use
`--fen-file name | six-field FEN` through the existing harness for named FEN
positions. Run balanced conditions with Koi White and Koi Black, and record the
exact engine paths, options, time control, opening/FEN input, and generated JSON
and PGN together in the external results directory.

All three tools fail with an actionable stderr message and nonzero exit code for
missing inputs or engine/configuration errors. The C++ engine remains independent
of Python and `python-chess`.
