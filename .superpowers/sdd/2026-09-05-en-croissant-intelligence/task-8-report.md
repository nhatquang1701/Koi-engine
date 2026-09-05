# Task 8 Report - Full Release Validation and En Croissant Compatibility

Date: 2026-09-06
Base: `dd66bce`
Scope: release validation, process compatibility, external Stockfish oracle measurement, and final release guidance

## Release validation

Fresh external build artifacts were produced under:

`C:\Users\ntATh\AppData\Local\Temp\koi-task8-final3-b49cb5c096684457b1f999ca6aae3541`

- CMake: `3.31.6-msvc6`
- Compiler: MSVC `19.44.35228.0`, x64 host/target
- CMake project standard: C++26 required, with extensions disabled
- Debug: configure, build, and CTest passed
- Release: configure, build, and CTest passed
- Debug CTest: 18/18 passed, 405.96 seconds
- Release CTest: 18/18 passed, 155.54 seconds
- Current-working-tree Release CTest rerun: 18/18 passed, 151.89 seconds

The release harness covered perft, FEN/rules, search, evaluator/strength,
tablebase lifecycle, book safety, UCI process behavior, En Croissant-style
process behavior, benchmark process output, match replay, Windows
configuration, and Python tooling.

## Thread, benchmark, and process gates

The fresh Release validation produced:

- Threads 1: 64/64 tactical benchmark rows matched the expected move and score.
- Threads 2: 64/64 rows matched the Threads 1 reference.
- Threads 4: 64/64 rows matched the Threads 1 reference.
- Optional strength profile: 128 positions completed.
- Timed profile: completed with protocol-valid rows and timing fields.
- UCI smoke: one legal coordinate `bestmove`, `uciok`, `readyok`, and empty stderr.
- En Croissant-style process play: 24 legal plies, configured with Hash 512,
  Threads 4, Speed 100, and clean shutdown for both engine processes.
- Replay: legal repetition classified as `1/2-1/2` with `rule draw` termination.

The accepted Task 5 fixed-depth performance gate remains below the 5% ceiling.
Using the three external baseline/candidate profiles in
`C:\Users\ntATh\AppData\Local\Temp\koi-task5-perf-gate-708eacca1de549a3bd4d0db3599f9fe5`,
the median total was 1457 ms for the baseline and 1471 ms for the candidate,
or a 0.96% candidate regression. Move, score, and completed-depth parity was
preserved.

## Stockfish 19 oracle measurement

The supplied Stockfish executable was available at:

`C:\Users\ntATh\AI test\Koi engine\third_party\Stockfish 19\stockfish-windows-x86-64-universal\stockfish\stockfish-windows-x86-64-universal.exe`

The three supplied standard-SAN PGNs were extracted successfully in
engine-free mode. External extraction reports are in:

- `C:\Users\ntATh\AppData\Local\Temp\koi-task8-oracle-20260905\extract-stockfish-2.json` - 58 positions
- `C:\Users\ntATh\AppData\Local\Temp\koi-task8-oracle-20260905\extract-koi-koi.json` - 33 positions
- `C:\Users\ntATh\AppData\Local\Temp\koi-task8-oracle-20260905\extract-stockfish.json` - 80 positions

The completed oracle reports used the fresh x64 Release Koi executable,
Stockfish 19, `OwnBook=false`, `Threads=4`, `Speed=100`, and 250 ms per
position. Reports are outside the repository:

- `C:\Users\ntATh\AppData\Local\Temp\koi-task8-oracle-20260905\oracle-stockfish-2-retry.json`
- `C:\Users\ntATh\AppData\Local\Temp\koi-task8-oracle-20260905\oracle-koi-koi.json`
- `C:\Users\ntATh\AppData\Local\Temp\koi-task8-oracle-20260905\oracle-stockfish.json`

Measured Stockfish-evaluated CPL was:

| PGN | Positions | Recorded game move mean / p95 | Koi suggested move mean / p95 | Recorded blunders >=100 cp | Koi blunders >=100 cp | Result |
|---|---:|---:|---:|---:|---:|---|
| Koi - Stockfish 19 (2) | 58 | 3430.93 / 309 | 5167.81 / 98591 | 6 | 15 | 0-1 |
| Koi - Koi | 33 | 33.82 / 94 | 43.97 / 122 | 0 | 4 | 1/2-1/2 |
| Koi - Stockfish 19 | 80 | 1263.67 / 184 | 3799.55 / 573 | 6 | 19 | 0-1 |
| Combined | 171 | 1761.43 / 184 | 3538.88 / 573 | 12 | 38 | mixed |

The oracle normalizes mate scores to +/-100,000 cp. The very large means in
the two Stockfish games include terminal mate positions and should not be
interpreted as a direct Elo estimate. This is a reproducible baseline, not an
Elo claim; the corpus is small, selected, and not a color-balanced match.

During the first oracle pass, Stockfish returned `bestmove (none)` on the
checkmated resulting position after the final PGN move. The measurement parser
now normalizes that legal terminal form to `0000`, preserving the existing
report representation. The regression test
`test_parse_stockfish_terminal_none_bestmove_as_no_move` was run red before
the change and green after it. The complete Python oracle suite then passed:
`Ran 13 tests ... OK`.

## Opening book status and placement

No licensed `book.bin` was found in the repository or supplied inputs, so the
separate book audit was not run and no book result is claimed. Do not download
or add an unverified book. Put the user-supplied licensed Polyglot book at:

`<directory containing koi-engine.exe>\book.bin`

Then use `BookFile=book.bin` (or configure an explicitly licensed absolute
path). Koi resolves a relative book path beside the executable. The release
defaults remain `OwnBook=true`, `BookDepth=16`, and `BookRandom=false`; a
missing or malformed book safely falls back to search.

## Remaining limitations

- The actual En Croissant GUI was not installed in the validation environment;
  the standard-UCI process transcript and 24-ply En Croissant-style harness
  passed, but a manual GUI registration gate is not claimed.
- No Elo increase is claimed. A larger color-balanced Koi-versus-Stockfish
  match at 1+0 and 5+3 is still required for practical strength evidence.
- No licensed book audit is claimed until the user supplies `book.bin`.
- NNUE, opening-book generation, variants, and tablebase data packaging remain
  outside this release scope.

## Evidence paths

- Release validation: `C:\Users\ntATh\AppData\Local\Temp\koi-task8-final3-b49cb5c096684457b1f999ca6aae3541`
- Fixed-depth performance gate: `C:\Users\ntATh\AppData\Local\Temp\koi-task5-perf-gate-708eacca1de549a3bd4d0db3599f9fe5`
- Stockfish oracle reports: `C:\Users\ntATh\AppData\Local\Temp\koi-task8-oracle-20260905`
