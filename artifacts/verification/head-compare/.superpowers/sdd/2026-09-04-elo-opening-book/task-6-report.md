# Task 6 verification report: Elo and opening book baseline

Date: 2026-09-04
Base commit: `817d5b7cd7899b917137c8c7a375ebdf3f324abe`
External evidence directory: `%TEMP%\koi-task6-20260904`

## Build configurations

Fresh build directories were created outside the repository:

- `%TEMP%\koi-task6-20260904\debug`
- `%TEMP%\koi-task6-20260904\release`

The requested Visual Studio x64 environment was loaded with:

```powershell
cmd /c ""C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64"
```

The first `cmake` found on `PATH` was `C:\MinGW\bin\cmake.exe`, version 3.27.1,
which stopped at `CMakeLists.txt:1` because the project requires CMake 3.31 or
newer. The installed `C:\msys64\ucrt64\bin\cmake.exe` is CMake 4.4.2 and was
used instead. It selected MSVC 19.44.35227.0 at
`Hostx64/x64/cl.exe`; both configurations use the project C++26
`/std:c++latest` setting.

```powershell
& "C:\msys64\ucrt64\bin\cmake.exe" -S "C:\Users\ntATh\AI test\Koi engine" -B "$env:TEMP\koi-task6-20260904\debug" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=cl
& "C:\msys64\ucrt64\bin\cmake.exe" -S "C:\Users\ntATh\AI test\Koi engine" -B "$env:TEMP\koi-task6-20260904\release" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=cl
& "C:\msys64\ucrt64\bin\cmake.exe" --build "$env:TEMP\koi-task6-20260904\debug" --parallel 4
& "C:\msys64\ucrt64\bin\cmake.exe" --build "$env:TEMP\koi-task6-20260904\release" --parallel 4
```

Result: both builds completed all 90 Ninja steps and produced `koi-engine.exe`.

## Registered tests

Commands:

```powershell
& "C:\msys64\ucrt64\bin\ctest.exe" --test-dir "$env:TEMP\koi-task6-20260904\debug" -C Debug --output-on-failure
& "C:\msys64\ucrt64\bin\ctest.exe" --test-dir "$env:TEMP\koi-task6-20260904\release" -C Release --output-on-failure
```

Results:

| Configuration | Registered/passed | Real time |
| --- | ---: | ---: |
| Debug | 14/14 | 73.24 s |
| Release | 14/14 | 32.18 s |

The targets were `koi_core_tests`, `koi_rules_tests`, `opening_book_tests`,
`uci_controller_tests`, `koi_search_tests`, `search_ordering_tests`,
`static_exchange_tests`, `koi_strength_tests`, `perft_tests`, `koi_replay_tests`,
`koi_engine_process`, `koi_benchmark_process`, `koi_uci_match_process`, and
`windows_ci_configuration`.

`opening_book_tests` covers reference Polyglot keys, castling/en-passant/promotion
decoding, weighted legal selection with a nonzero deterministic seed, and unavailable,
malformed, disabled, depth-exhausted, and illegal-only fallback. `koi_strength_tests`
is the 64-case tactical hard gate. `uci_controller_tests` covers book normal play,
fallback/bypass, analysis/tutor MultiPV, ponder lifecycle, duplicate/stale bestmove
suppression, and quit/EOF cleanup.

## Benchmark and deterministic-thread evidence

All benchmark commands ran from the Release build and wrote their stdout/JSON to the
external evidence directory:

```powershell
& "$env:TEMP\koi-task6-20260904\release\koi-bench.exe"
& "$env:TEMP\koi-task6-20260904\release\koi-bench.exe" --threads 1 --speed 100 --profile-json "$env:TEMP\koi-task6-20260904\hard-t1.json"
& "$env:TEMP\koi-task6-20260904\release\koi-bench.exe" --threads 2 --speed 100 --profile-json "$env:TEMP\koi-task6-20260904\hard-t2.json"
& "$env:TEMP\koi-task6-20260904\release\koi-bench.exe" --threads 4 --speed 100 --profile-json "$env:TEMP\koi-task6-20260904\hard-t4.json"
& "$env:TEMP\koi-task6-20260904\release\koi-bench.exe" --optional --profile-json "$env:TEMP\koi-task6-20260904\optional.json"
& "$env:TEMP\koi-task6-20260904\release\koi-bench.exe" --threads 2 --speed 50 --timed --profile-json "$env:TEMP\koi-task6-20260904\timed-t2-s50.json"
```

| Run | Result |
| --- | --- |
| Default hard gate | exit 0, no stderr |
| Hard gate, Threads 1/2/4, Speed 100 | exit 0 each; 64/64 `match 1` rows each |
| Fixed-depth deterministic comparison | Threads 1 vs. 2 and 1 vs. 4 had identical ID, score, and PV rows |
| Optional corpus | exit 0, no stderr; 128 rows, 34 diagnostic `match 1` rows |
| Timed hard gate, Threads 2, Speed 50 | exit 0, no stderr; 64 rows; 181,322 visited nodes; 155 ms summed position time; about 1.17M visited nodes/s aggregate |

Untimed JSON intentionally stores `nps: 0`; timed NPS varies with load, timing
granularity, hash state, threads, and speed. No approved historical NPS baseline
was supplied, so the 20% floor cannot be evaluated from this machine alone.

## UCI transcript evidence

The complete process harness was rerun directly:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tests\uci_process_test.ps1 -EnginePath "$env:TEMP\koi-task6-20260904\release\koi-engine.exe"
```

Result: exit 0 and empty stderr. Its transcript checks the exact UCI handshake,
`isready`, book hit using a temporary executable-relative Polyglot book, missing-book
fallback, `UCI_AnalyseMode`/MultiPV, ponder and `ponderhit`, `stop`, `quit`, and EOF.
The controller unit target was also rerun directly with exit 0.

The following separate sample command returned exit 0 and empty stderr; stdout
contained only UCI protocol lines, including the advertised options, `uciok`,
`readyok`, and one legal `bestmove a2a3`:

```powershell
@('uci','isready','position startpos','go depth 2','stop','quit') | & "$env:TEMP\koi-task6-20260904\release\koi-engine.exe"
```

## External verification limitations

`Get-Command stockfish` and `Get-Command Stockfish` returned no executable, and
there was no supplied UCI opponent. Therefore no color-balanced 40-game-per-opening
measurements at 1+0 or 5+3 were run and no results were invented. `-Games` is the
number of games per selected opening, so the reproducible 40-game recipe is 20 Koi
White plus 20 Koi Black games for each clock and book condition. When an opponent
and licensed book are supplied, run all of these commands and retain every JSON/PGN
outside the repository:

```powershell
.\tools\uci_match.ps1 -KoiPath "$env:TEMP\koi-task6-20260904\release\koi-engine.exe" -OpponentPath C:\Engines\stockfish.exe -OpeningFile .\tests\data\elo-openings.txt -TimeControl 1+0 -Games 20 -KoiColor white -KoiRandomSeed 1 -KoiOwnBook false -OutputDirectory C:\Koi-results\no-book-1p0-white
.\tools\uci_match.ps1 -KoiPath "$env:TEMP\koi-task6-20260904\release\koi-engine.exe" -OpponentPath C:\Engines\stockfish.exe -OpeningFile .\tests\data\elo-openings.txt -TimeControl 1+0 -Games 20 -KoiColor black -KoiRandomSeed 1 -KoiOwnBook false -OutputDirectory C:\Koi-results\no-book-1p0-black
.\tools\uci_match.ps1 -KoiPath "$env:TEMP\koi-task6-20260904\release\koi-engine.exe" -OpponentPath C:\Engines\stockfish.exe -OpeningFile .\tests\data\elo-openings.txt -TimeControl 1+0 -Games 20 -KoiColor white -KoiRandomSeed 1 -KoiOwnBook true -KoiBookFile C:\LicensedBooks\book.bin -KoiBookDepth 16 -OutputDirectory C:\Koi-results\book-1p0-white
.\tools\uci_match.ps1 -KoiPath "$env:TEMP\koi-task6-20260904\release\koi-engine.exe" -OpponentPath C:\Engines\stockfish.exe -OpeningFile .\tests\data\elo-openings.txt -TimeControl 1+0 -Games 20 -KoiColor black -KoiRandomSeed 1 -KoiOwnBook true -KoiBookFile C:\LicensedBooks\book.bin -KoiBookDepth 16 -OutputDirectory C:\Koi-results\book-1p0-black
.\tools\uci_match.ps1 -KoiPath "$env:TEMP\koi-task6-20260904\release\koi-engine.exe" -OpponentPath C:\Engines\stockfish.exe -OpeningFile .\tests\data\elo-openings.txt -TimeControl 5+3 -Games 20 -KoiColor white -KoiRandomSeed 1 -KoiOwnBook false -OutputDirectory C:\Koi-results\no-book-5p3-white
.\tools\uci_match.ps1 -KoiPath "$env:TEMP\koi-task6-20260904\release\koi-engine.exe" -OpponentPath C:\Engines\stockfish.exe -OpeningFile .\tests\data\elo-openings.txt -TimeControl 5+3 -Games 20 -KoiColor black -KoiRandomSeed 1 -KoiOwnBook false -OutputDirectory C:\Koi-results\no-book-5p3-black
.\tools\uci_match.ps1 -KoiPath "$env:TEMP\koi-task6-20260904\release\koi-engine.exe" -OpponentPath C:\Engines\stockfish.exe -OpeningFile .\tests\data\elo-openings.txt -TimeControl 5+3 -Games 20 -KoiColor white -KoiRandomSeed 1 -KoiOwnBook true -KoiBookFile C:\LicensedBooks\book.bin -KoiBookDepth 16 -OutputDirectory C:\Koi-results\book-5p3-white
.\tools\uci_match.ps1 -KoiPath "$env:TEMP\koi-task6-20260904\release\koi-engine.exe" -OpponentPath C:\Engines\stockfish.exe -OpeningFile .\tests\data\elo-openings.txt -TimeControl 5+3 -Games 20 -KoiColor black -KoiRandomSeed 1 -KoiOwnBook true -KoiBookFile C:\LicensedBooks\book.bin -KoiBookDepth 16 -OutputDirectory C:\Koi-results\book-5p3-black
```

No Lucas Chess executable or accessible installation was found, so manual GUI
registration was not claimed. The user can register
`%TEMP%\koi-task6-20260904\release\koi-engine.exe` in Lucas Chess and use:

```text
Hash 512
Threads 4
Speed 100
OwnBook true
BookFile book.bin
BookDepth 16
```

Place `book.bin` beside the release executable, then play a short standard game and
confirm a legal move, responsive stop/new-game handling, and no duplicate bestmove.

## Changed files

- `README.md` — added the measured local baseline, book behavior/defaults/bypass,
  threaded determinism, timing caveat, and external-verification limits.
- `.superpowers/sdd/2026-09-04-elo-opening-book/task-6-report.md` — this report.

`CMakeLists.txt` was not changed: the existing 14 registered targets already cover
the requested test categories.

## Fix round 1 details

Documentation-only reviewer fixes were applied from current commit `982fd3a`:

- README no longer says opening books remain deferred; it points to the implemented
  book defaults, placement, fallback, and bypass documentation.
- README now limits the unavailable-opponent statement to the recorded
  `Get-Command stockfish`/`Stockfish` checks and the absence of a supplied UCI
  opponent. The 40-game matrix and Lucas GUI registration remain unverified because
  no opponent, licensed book, or Lucas installation was available.
- The Visual Studio environment setup snippet now uses an explicit `cmd /c` wrapper
  so it is directly runnable from PowerShell.

No engine code, tests, UCI behavior, or CMake configuration was changed.
