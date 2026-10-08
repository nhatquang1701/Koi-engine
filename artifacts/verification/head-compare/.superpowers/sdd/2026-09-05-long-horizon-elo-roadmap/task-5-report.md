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

## Fix round 1 - review findings

Review round 1 identified two Important gaps: the release evidence depended on
uncommitted external artifacts without a checked-in reproduction path, and the
benchmark process test did not exercise an explicit Threads 4 case. Both are
addressed without changing engine or UCI behavior.

### TDD and harness changes

The pre-implementation acceptance check for the new runner failed as expected:

```text
if (-not (Test-Path .\tools\task5_release_verify.ps1)) { throw ... }
Exception: task5_release_verify.ps1 is not yet present
```

`tests/koi_bench_process_test.ps1` now runs two explicit
`--threads 4 --speed 100` benchmark invocations when the host supports four
threads, compares normalized move/score rows, checks the Threads 4 profile, and
uses the explicit maximum-thread fallback when fewer than four threads are
available. It also checks timed profile `timed=true`, top-level `hash_state`,
and per-position hash-state consistency. The focused test against the fresh
Release binary passed with exit code 0 and empty stdout/stderr:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tests\koi_bench_process_test.ps1 `
  -BenchPath C:\Koi-results\task5-review1-rerun\build-release\koi-bench.exe
```

The checked-in `tools/task5_release_verify.ps1` is a portable PowerShell runner.
It rejects repository-local output directories, configures and builds fresh
external Debug and Release trees, runs CTest, validates tactical benchmark
profiles and normalized rows, runs timed and optional benchmark modes, captures
the UCI smoke transcript and replay report, and runs the 24-ply replay-validated
Lucas-style process scenario. All generated output is under its supplied
external `-OutputDirectory`.

### Fresh independent runner command and output

The runner was executed from an x64 Visual Studio developer shell with CMake
3.31.6-msvc6, Ninja, and MSVC `cl`:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\task5_release_verify.ps1 `
  -CMakePath "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" `
  -OutputDirectory C:\Koi-results\task5-review1-rerun
```

The first fresh runner attempt recorded a Debug UCI process timeout under the
initial machine load; its external CTest log preserved the failure. A second
fresh run from a new output root completed all gates. The runner's concrete
stdout was:

```text
verification_root=C:\Koi-results\task5-review1-rerun
cmake=cmake version 3.31.6-msvc6 generator=Ninja compiler=cl
Debug configure/build/CTest=PASS; Release configure/build/CTest=PASS
UCI smoke lines=26 bestmove=1 stderr=0
Replay legal=1 result=1/2-1/2 termination=rule draw
Threads=1 rows=64 matches=64 profile_threads=1
Threads=2 rows=64 matches=64 profile_threads=2
Threads=4 rows=64 matches=64 profile_threads=4
Timed Threads=4 rows=64 profile_timed=True
Optional rows=128 profile_positions=128
Lucas-style plies=24 hash=512 threads=4 speed=100 replay_legal=all process_status=clean
Lucas JSON=C:\Koi-results\task5-review1-rerun\lucas-style-24ply\koi-uci-match-20260905-082512-603.json
Stockfish CPL/match data: unavailable; no Elo claim
Lucas Chess GUI: unavailable; manual GUI gate not claimed
```

The captured CTest summaries were:

```text
Debug:   100% tests passed, 0 tests failed out of 17; Total Test time 95.08 s
Release: 100% tests passed, 0 tests failed out of 17; Total Test time 50.59 s
```

The runner preserved separate `ctest-Debug.txt` and `ctest-Release.txt` logs,
zero-byte stderr logs, `bench-threads-1/2/4.txt` plus JSON profiles,
`bench-timed.json`, `bench-optional.json`, `uci-smoke.txt`, `replay.txt`, and
the Lucas JSON/PGN under the external output root. The explicit Threads 4
process-test result and the independent runner result are both fresh. No
Stockfish executable/CPL or match data was available, and Lucas Chess GUI was
not installed, so the report continues to make no Elo/strength claim and does
not claim the manual GUI gate.
