# Task 4 Report: Ponder lifecycle and `ponderhit` restart

## Status

Implemented the Lucas Chess UCI ponder lifecycle. `go ponder` now runs speculatively without depth, node, movetime, or clock completion; `stop` retains one current result; and `ponderhit` suppresses the speculative generation before starting a normal replacement search from the saved root and limits.

## Changed files

- `src/koi/time_manager.cpp`
  - Treats `SearchLimits::ponder` as unbounded for time and node termination.
- `src/koi/search_service.cpp`
  - Keeps both single-threaded and root-worker iterative loops alive for ponder searches.
- `src/koi/uci_controller.hpp`
  - Adds ponder root/limit state and lifecycle helpers.
- `src/koi/uci_controller.cpp`
  - Handles `ponderhit`, snapshots/restarts ponder searches, clears replaced snapshots, and keeps the post-hit search's single final completion.
- `tests/koi_search_tests.cpp`
  - Adds the service ponder lifecycle test.
- `tests/uci_controller_tests.cpp`
  - Adds `ponderhit`, ponder-stop, and ponder-`searchmoves` controller transcripts.

## TDD evidence

The first bare PowerShell command could not compile because this shell lacks the MSVC developer environment (`fatal error C1083: Cannot open include file: 'algorithm'`). CTest then executed stale binaries. As documented by Task 3, all authoritative commands below initialize `VsDevCmd.bat` and execute the brief's build and CTest commands unchanged.

### RED

```powershell
cmd.exe /d /c 'call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul && cmake --build out\debug-vs --config Debug --target koi_search_tests uci_controller_tests && ctest --test-dir out\debug-vs -C Debug -R "koi_search_tests|uci_controller_tests" --output-on-failure'
```

```text
FAIL ponderhit restart lifecycle: ponderhit must emit exactly one legal bestmove from its restarted search
FAIL ponder search lifecycle: ponder search must remain running instead of completing its depth limit
0% tests passed, 2 tests failed out of 2
```

### GREEN

```powershell
cmd.exe /d /c 'call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul && cmake --build out\debug-vs --config Debug --target koi_search_tests uci_controller_tests && ctest --test-dir out\debug-vs -C Debug -R "koi_search_tests|uci_controller_tests" --output-on-failure'
```

```text
100% tests passed, 0 tests failed out of 2
Total Test time (real) = 13.50 sec
```

### Existing cancellation and infinite regressions

```powershell
$env:KOI_TEST_FILTER = 'threaded cancellation'; .\out\debug-vs\koi_search_tests.exe
$env:KOI_TEST_FILTER = 'infinite search lifecycle'; .\out\debug-vs\koi_search_tests.exe
$env:KOI_TEST_FILTER = 'ponder search lifecycle'; .\out\debug-vs\koi_search_tests.exe
```

```text
PASS threaded cancellation
PASS infinite search lifecycle
PASS ponder search lifecycle
```

### Direct UCI transcript

```text
position startpos
go ponder depth 2
ponderhit
isready
quit

readyok
bestmove a2a3
```

## Concerns

- No test failed or hung after the GREEN run, and no `koi*` search process remained running.
- `src/koi/time_manager.cpp` and `src/koi/time_manager.hpp` already contained uncommitted roadmap work (speed scaling and `node_limit`) before Task 4. The Task 4 change shares those files, so a path-scoped commit must include that mixed-file state to remain buildable; unrelated paths will not be staged.
- The direct PowerShell form of the brief's command requires the Visual Studio developer environment on this machine. The command wrapper above provides that environment without changing the underlying focused build/CTest invocation.

## Fix round 1: shutdown suppression and terminal ponder parking

### Scope

- Removed the `restarted_from_ponder_` exception. Both `quit` and EOF now suppress every active search generation, including a normal replacement started by `ponderhit`.
- Added a stop-notified wait for ponder roots that have no legal move after filtering or are drawn by rule. They stay alive without spinning and report their existing no-move result only after `stop` or `ponderhit` cancels them.
- Added controller and service regressions for restarted-search shutdown suppression, idle `ponderhit`, terminal ponder `0000`, terminal/drawn/empty-filter ponder parking, and node/movetime/clock suppression.

### TDD evidence

RED (new tests before production edits):

```powershell
cmd.exe /d /c 'call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul && cmake --build out\debug-vs --config Debug --target koi_search_tests uci_controller_tests && ctest --test-dir out\debug-vs -C Debug -R "koi_search_tests|uci_controller_tests" --output-on-failure'
```

```text
FAIL ponderhit shutdown suppression: quit must suppress a ponderhit replacement search's late bestmove
FAIL ponder terminal lifecycle: ponder checkmate root must remain running until stop
0% tests passed, 2 tests failed out of 2
```

GREEN (the focused brief command, with the required MSVC developer-environment wrapper):

```powershell
cmd.exe /d /c 'call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul && cmake --build out\debug-vs --config Debug --target koi_search_tests uci_controller_tests && ctest --test-dir out\debug-vs -C Debug -R "koi_search_tests|uci_controller_tests" --output-on-failure'
```

```text
100% tests passed, 0 tests failed out of 2
Total Test time (real) = 17.88 sec
```

Targeted cancellation checks:

```powershell
$env:KOI_TEST_FILTER = 'ponder terminal lifecycle'; .\out\debug-vs\koi_search_tests.exe
$env:KOI_TEST_FILTER = 'ponder search lifecycle'; .\out\debug-vs\koi_search_tests.exe
$env:KOI_TEST_FILTER = 'threaded cancellation'; .\out\debug-vs\koi_search_tests.exe
$env:KOI_TEST_FILTER = 'ponder ignores time and nodes'; .\out\debug-vs\koi_search_tests.exe
```

```text
PASS ponder terminal lifecycle
PASS ponder search lifecycle
PASS threaded cancellation
PASS ponder ignores time and nodes
```

### Concerns

- The focused tests and targeted cancellation filters leave no `koi*` process running.
- Existing unrelated roadmap changes remain preserved and unstaged.
