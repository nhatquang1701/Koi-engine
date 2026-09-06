# Task 4 report — integration and verification

## Scope

Task 4 changed only:

- `CMakeLists.txt`
- `README.md`
- `tools/README.md`
- this report

No C++ source, estimator implementation, match implementation, or corpus file
was changed. Existing dirty changes in the shared checkout were preserved.

## Integration

When Python 3 is available, CTest now registers these standard-library-only
tests using the existing `python -m unittest ... -v` pattern:

- `elo_estimate_python`
- `stockfish_match_python`
- `elo_openings_python`

The corpus test has an explicit
`KOI_REPLAY_PATH=$<TARGET_FILE:koi_replay>` environment value. The existing
optional `elo_oracle_python` registration remains conditional on `python-chess`.

The READMEs document the exact no-book 1+0 CLI, manifest schema and anchor
requirements, dry-run behavior, separate licensed-book mode, external raw JSON
and PGN artifacts, adaptive schedule, CI caveat, and the required label:
“local Stockfish-equivalent Elo at recorded hardware/options/time control”; the
documentation explicitly says this is not a universal Elo claim.

## Verification

### Focused Python tests

Command:

```text
python -m unittest tests.elo_estimate_test tests.stockfish_match_test tests.elo_openings_test -v
```

Exit code: `0`

Output:

```text
Ran 16 tests in 4.897s

OK
report C:\Users\ntATh\AppData\Local\Temp\tmpv4j_0k9q\external-report.json
report C:\Users\ntATh\AppData\Local\Temp\tmpwe4nbaem\external-report.json
report C:\Users\ntATh\AppData\Local\Temp\tmpgqdf0819\external-report.json
```

### Complete Python discovery

Command:

```text
python -m unittest discover -s tests -p '*_test.py' -v
```

Exit code: `0`

Output summary:

```text
Ran 30 tests in 8.648s

OK
```

### Syntax and whitespace checks

Commands:

```text
python -m py_compile tools\elo_estimate.py tools\stockfish_match.py tests\elo_estimate_test.py tests\stockfish_match_test.py tests\elo_openings_test.py
git diff --check -- CMakeLists.txt README.md tools/README.md
```

Both commands exited `0`. `py_compile` produced no output. `git diff --check`
produced only Git's existing LF-to-CRLF normalization warnings for the three
modified tracked files and no whitespace errors.

### Static CMake/README checks

The check asserted the three CTest names and Python commands, the explicit
`KOI_REPLAY_PATH=$<TARGET_FILE:koi_replay>` value, preservation of the optional
oracle registration, both estimator CLIs, manifest requirements, external JSON/
PGN handling, CI caveat, and the required local label.

Command:

```powershell
$cmake=Get-Content -Raw CMakeLists.txt; $readme=Get-Content -Raw README.md; $tools=Get-Content -Raw tools/README.md; $checks=[ordered]@{
  'elo_estimate CTest' = $cmake.Contains('NAME elo_estimate_python') -and $cmake.Contains('tests/elo_estimate_test.py');
  'stockfish options CTest' = $cmake.Contains('NAME stockfish_match_python') -and $cmake.Contains('tests/stockfish_match_test.py');
  'opening corpus CTest' = $cmake.Contains('NAME elo_openings_python') -and $cmake.Contains('tests/elo_openings_test.py');
  'replay target environment' = $cmake.Contains('KOI_REPLAY_PATH=$<TARGET_FILE:koi_replay>');
  'optional oracle preserved' = $cmake.Contains('NAME elo_oracle_python') -and $cmake.Contains('import chess') -and $cmake.Contains('requirements-elo-oracle.txt');
  'exact no-book CLI' = $tools.Contains('--time-control 1+0') -and $tools.Contains('--threads 4 --hash 512 --speed 100') -and $tools.Contains('--mode no-book') -and $tools.Contains('--dry-run');
  'anchor manifest requirements' = $tools.Contains('koi-elo-anchor-manifest-v1') -and $tools.Contains('1320..3190') -and $tools.Contains('lower_anchors');
  'book mode separate' = $tools.Contains('--mode book --book') -and $tools.Contains('separate report');
  'external raw artifacts' = $tools.Contains('Raw JSON and matching PGN') -and $tools.Contains('<stem>-artifacts');
  'schedule CI caveat' = $tools.Contains('not a CI job or a fixed Elo gate');
  'required local label' = $readme.Contains('local Stockfish-equivalent Elo at recorded hardware/options/time control') -and $tools.Contains('local Stockfish-equivalent Elo at recorded');
}; $checks.GetEnumerator() | ForEach-Object { Write-Output (('{0}={1}' -f $_.Key, $_.Value).ToUpperInvariant()) }; if (($checks.Values | Where-Object { -not $_ }).Count -gt 0) { exit 1 }; Write-Output 'STATIC_CHECKS=PASS'
```

Exit code: `0`

Output:

```text
ELO_ESTIMATE CTEST=TRUE
STOCKFISH OPTIONS CTEST=TRUE
OPENING CORPUS CTEST=TRUE
REPLAY TARGET ENVIRONMENT=TRUE
OPTIONAL ORACLE PRESERVED=TRUE
EXACT NO-BOOK CLI=TRUE
ANCHOR MANIFEST REQUIREMENTS=TRUE
BOOK MODE SEPARATE=TRUE
EXTERNAL RAW ARTIFACTS=TRUE
SCHEDULE CI CAVEAT=TRUE
REQUIRED LOCAL LABEL=TRUE
STATIC_CHECKS=PASS
```

### Estimator dry-run

The run used the exact documented no-book command shape, an existing local Koi
and `koi-replay` executable, the checked-in 32-opening corpus, and temporary
external fake Stockfish/manifest inputs. Because `--dry-run` was supplied, no
engine or PowerShell match was launched.

Command:

```text
python .\tools\elo_estimate.py --koi .\out\task4-release-vs\koi-engine.exe --replay .\out\task4-release-vs\koi-replay.exe --stockfish C:\Users\ntATh\AppData\Local\Temp\koi-task4-dryrun-69d3efc074d54f7ab1c30000a86b1310\stockfish.exe --anchors C:\Users\ntATh\AppData\Local\Temp\koi-task4-dryrun-69d3efc074d54f7ab1c30000a86b1310\anchors.json --openings .\tests\data\elo-openings-32.txt --time-control 1+0 --threads 4 --hash 512 --speed 100 --min-games 128 --max-games 320 --prior-elo 1600 --mode no-book --output C:\Users\ntATh\AppData\Local\Temp\koi-task4-dryrun-69d3efc074d54f7ab1c30000a86b1310\rough-elo-no-book.json --dry-run
```

Exit code: `0`

Output:

```text
report C:\Users\ntATh\AppData\Local\Temp\koi-task4-dryrun-69d3efc074d54f7ab1c30000a86b1310\rough-elo-no-book.json
DRY_RUN_EXIT=0
DRY_RUN_SCHEMA=koi-rough-elo-estimate-v1
DRY_RUN_BATCHES=5
DRY_RUN_RESULTS_GAMES=0
DRY_RUN_MODE=no-book
DRY_RUN_ACTIVE_OPTIONS={"Hash":512,"OwnBook":false,"Speed":100,"Threads":4}
```

### CMake limitation

Commands:

```text
cmake --version
cmake -S . -B C:\Koi-results\task4-configure -G Ninja -DCMAKE_BUILD_TYPE=Release
```

`cmake --version` exited `0` and reported:

```text
cmake version 3.27.1
```

The configure command exited `1` before compiler detection:

```text
CMake Error at CMakeLists.txt:1 (cmake_minimum_required):
  CMake 3.31 or higher is required.  You are running version 3.27.1


-- Configuring incomplete, errors occurred!
```

Therefore a fresh Release configure/build and CTest run could not be executed in
this environment. No real engine match was run because user-supplied Stockfish,
anchor paths, and an external results location were not provided.

## Review fix round 1 — fresh CMake and CTest verification

The earlier CMake 3.27.1 failure was specific to the default `cmake` on `PATH`.
It is superseded by fresh verification using the Visual Studio Build Tools CMake
3.31.6 executable at:

```text
C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe
```

The commands were run from a proper x64 `VsDevCmd` environment.

### CMake version

Command:

```text
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --version
```

Exit code: `0`

Output:

```text
cmake version 3.31.6
```

### Release build

Command:

```text
cmake --build out\roadmap-release2 --config Release --parallel 4
```

Exit code: `0`

Output summary: `98/98` build steps completed successfully.

### CTest

Command:

```text
ctest --test-dir out\roadmap-release2 -C Release --output-on-failure
```

Exit code: `0`

Output summary:

```text
100% tests passed, 22/22 tests passed, total time 116.57s
```

The required Release build and CTest verification is now complete; the previous
default-CMake limitation is no longer a blocker.
