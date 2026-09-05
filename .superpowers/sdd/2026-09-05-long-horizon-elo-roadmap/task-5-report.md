# Task 5 Report - Final Benchmark, Documentation, and Release Verification

## Scope and baseline

Task 5 was completed against the accepted baseline HEAD `9e11795` (`fix:
correct Syzygy 50-move scoring`). The change is limited to the benchmark,
benchmark process assertions, and release documentation:

- `tools/koi_bench.cpp` now always identifies the benchmark configuration,
  labels the shared hash state as `cold` or `warm`, and records `hash_state` and
  `timed` in profile JSON while retaining the existing `warm_hash` field.
- `tests/koi_bench_process_test.ps1` verifies explicit deterministic
  `--threads 1 --speed 100`, cold/warm labels, profile schema fields, stable
  untimed JSON, and optional timed output without imposing timing thresholds.
- `README.md` documents koi-bench, Lucas play/analysis/tutor/MultiPV/ponder/book
  workflows, timing controls, WDL, UCI strength controls, Syzygy, hidden
  diagnostics, configuration, external `book.bin` handling, third-party
  licenses, release packaging, and the fresh verification results.

No engine source, search behavior, UCI parsing, UCI option semantics, book
defaults, Syzygy implementation, or generated `tests/__pycache__/` artifact was
changed. The build remains C++26 Windows x64 portable Release-compatible.

## TDD evidence

The benchmark process test was changed first to require the new observable
contract. Running the existing benchmark before the production change failed
with the expected missing-config failure:

```text
koi-bench must begin with its benchmark header: Koi benchmark
position mate_in_one ... match 1
```

The minimal benchmark change then added the stable configuration line and JSON
fields. A focused MSVC rebuild of `koi_bench` succeeded, followed by:

```text
powershell -NoProfile -ExecutionPolicy Bypass -File .\tests\koi_bench_process_test.ps1 \
  -BenchPath .\out\release-vs\koi-bench.exe
```

The focused process test exited 0 with no output or stderr. It covered repeated
untimed output, explicit threads/speed, timed rows, cold/warm profiles, stable
untimed JSON, multi-move PVs, and the 128-position optional suite.

## Fresh Debug and Release gates

Builds were configured in external directories with Visual Studio 2022 MSVC
19.44.35228.0, x64 developer environment, `/std:c++latest`, Ninja, and CMake
4.4.2. The configured commands were:

```powershell
cmake -S . -B C:\Koi-builds\task5-debug -G Ninja `
  -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=cl
cmake --build C:\Koi-builds\task5-debug --config Debug -- -j2
ctest --test-dir C:\Koi-builds\task5-debug -C Debug --output-on-failure

cmake -S . -B C:\Koi-builds\task5-release -G Ninja `
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=cl
cmake --build C:\Koi-builds\task5-release --config Release -- -j2
ctest --test-dir C:\Koi-builds\task5-release -C Release --output-on-failure
```

Results:

```text
Debug:   build PASS; 17/17 CTest tests passed; total test time 84.74 s
Release: build PASS; 17/17 CTest tests passed; total test time 43.44 s
```

The 17-test CTest result includes core/rules/search/ordering/SEE/strength/perft,
book and Syzygy tests, replay tests, UCI and benchmark process tests, UCI match
process tests, Windows configuration checks, and the available Python tests.

## Benchmark and deterministic threading evidence

Fresh Release benchmark artifacts were written outside the repository under
`C:\Koi-results\task5`:

- `tactical-threads-1.txt`, `tactical-threads-2.txt`, and
  `tactical-threads-4.txt`, each with `--speed 100` and a profile JSON;
- `tactical-threads-4-timed.txt` for opt-in timing;
- `optional-strength.txt` and `optional-strength.json`.

Results:

```text
Threads=1 rows=64 matches=64
Threads=2 rows=64 matches=64
Threads=4 rows=64 matches=64
Stable move/score rows: Threads 1 = Threads 2 = Threads 4
Timed rows=64
Optional rows=128 matches=32 profile=hash_state cold timed false
```

The timed Threads 4 run visited 183,807 nodes plus quiescence nodes over a
157 ms aggregate row-time sum, approximately 1.17M visited nodes/second. This
is an observation only; no machine-dependent NPS threshold was introduced.
Untimed profiles carry stable build identity `Koi Engine 1.0`, `timed=false`,
`nps=0`, and explicit `hash_state`; timed profiles carry `elapsed_ms` and NPS.

## UCI, replay, and Lucas-style process evidence

The fresh Release direct smoke transcript was saved to
`C:\Koi-results\task5\uci-smoke.txt`. It produced 26 lines, 21 option
declarations, one legal coordinate `bestmove`, and zero stderr bytes. Fresh CTest
process coverage additionally exercised the exact public handshake, WDL and
timing controls, analysis/tutor MultiPV, ponder/`ponderhit`, book hit and
fallback behavior, stop/quit/EOF, hidden diagnostics routing, and clean stdout.

A direct replay report was saved to
`C:\Koi-results\task5\replay-24-ply.txt`:

```text
legal 1
result 1/2-1/2
termination rule draw
```

The fresh Lucas-style process scenario used the Release executable and the
existing replay-validated match harness with `Hash=512`, `Threads=4`,
`Speed=100`, Koi versus Koi, depth 2, and a 24-ply maximum. Its external JSON
and PGN are under the unique `C:\Koi-results\task5\lucas-style-24ply-*`
directory. The report contained 24 plies, every replay legality flag was true,
and both processes reported clean shutdown. The harness recorded exact UCI
commands, root FENs, returned moves, info/PV data, and replay results.

Lucas Chess was not installed or accessible for GUI automation. The README
therefore gives the manual registration and short-game acceptance steps without
claiming that GUI gate passed.

## Release/package and measurement constraints

`book.bin` remains an external licensed Polyglot asset: it is not committed,
embedded, or required for startup. The README identifies the required retained
license files:

- `third_party/chess-library/LICENSE`
- `third_party/fathom/LICENSE`

No tablebase data, benchmark profiles, match JSON/PGN, or debug logs were added
to the repository. `git diff --check` passed for the tracked changes, and no
`src/` file is modified.

No Stockfish executable, fresh Stockfish CPL corpus, or comparable match data
was available. Consequently this report makes no Elo, CPL, or playing-strength
claim.

## Concerns and deferred items

- Wall-clock timing and NPS vary with CPU load, timer granularity, thread count,
  hash warmth, and speed settings; they are recorded diagnostically only.
- The optional strength corpus remains diagnostic and is not an Elo gate.
- A real Syzygy data fixture was not available, so end-to-end enabled tablebase
  move ranking remains covered by the existing safe-disabled/fallback tests and
  the implementation's prior Task 4 review evidence.
- Manual Lucas Chess GUI registration/play remains the only environment-gated
  check not performed here.
- The unrelated generated `tests/__pycache__/` directory remains untouched.

## Commit

The focused commit subject is `docs: finalize Task 5 release verification`.
The report was force-added because `.superpowers/sdd/` is ignored by the general
workspace rule, while the unrelated generated `tests/__pycache__/` remains
untracked and untouched.
