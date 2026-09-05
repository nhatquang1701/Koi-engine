# Task 1 Report - UCI Compatibility Controls and Hidden Diagnostics

Date: 2026-09-05
Branch: `koi-engine-v1`
Workspace: `C:\Users\ntATh\AI test\Koi engine`

## Scope and audit

The exact requirements in `task-1-brief.md` were read before implementation work. The inherited working diff was retained and audited across:

- `src/koi/search_types.hpp`: Task 1 option fields and the deterministic strength-profile hook.
- `src/koi/search_service.cpp`: option normalization, hook invocation, and timing-option snapshot handoff.
- `src/koi/time_manager.hpp/.cpp`: Slow Mover -> Speed -> Move Overhead timing order, minimum safe budget, and preservation of untimed modes.
- `src/koi/uci_controller.hpp/.cpp`: multi-word `setoption` parsing, public handshake options, WDL output, hidden debug diagnostics, rotation, locking, and search lifecycle handling.
- `tests/koi_search_tests.cpp`, `tests/uci_controller_tests.cpp`, and `tests/uci_process_test.ps1`: behavior and process coverage.

Lucas/UCI behavior, book defaults and handling, `Threads=1` deterministic behavior, `searchmoves`, MultiPV, ponder, completion callbacks, and stdout/stderr boundaries were checked against the existing code and regression suite. No third-party dependency or public `chess.hpp` exposure was added.

## TDD evidence

The handoff ledger records that the worktree was clean before the predecessor’s Task 1 implementation. The inherited Task 1 tests were already present in the takeover diff; their behavior coverage was preserved. Because the predecessor’s stopped session cannot be replayed, its exact historical command timestamps are not independently reconstructable. I additionally captured a complete RED/GREEN cycle for the real defect found during takeover.

### Existing takeover baseline

Command, run against the inherited implementation before the correction:

```powershell
cmake --build out\long-horizon-debug --config Debug --parallel
ctest --test-dir out\long-horizon-debug -C Debug -R "koi_search_tests|uci_controller_tests|koi_engine_process" --output-on-failure
```

Output: `uci_controller_tests`, `koi_search_tests`, and `koi_engine_process` passed; `100% tests passed, 0 tests failed out of 3`.

### RED - new regression test before production correction

Added `test_explicit_depth_and_nodes_remain_untimed_with_clock_fields` to `tests/koi_search_tests.cpp`, then ran under the VS 2022 x64 developer environment:

```powershell
& 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\Launch-VsDevShell.ps1' -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
cmake --build out\long-horizon-debug --config Debug --parallel
ctest --test-dir out\long-horizon-debug -C Debug -R '^koi_search_tests$' --output-on-failure
```

Expected failure was observed:

```text
FAIL explicit limits remain untimed: explicit depth must remain untimed even when time fields are also present
0% tests passed, 1 tests failed out of 1
```

Root cause: `TimeManager` checked `movetime` before considering the presence of explicit depth/node limits.

### GREEN - minimal correction

Added the explicit `depth`/`nodes` early return to `TimeManager`, plus the self-contained `<algorithm>` include required by the controller. Then ran:

```powershell
& 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\Launch-VsDevShell.ps1' -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
cmake --build out\long-horizon-debug --config Debug --parallel
ctest --test-dir out\long-horizon-debug -C Debug -R "koi_search_tests|uci_controller_tests|koi_engine_process" --output-on-failure
```

Output: all three focused targets passed; `100% tests passed, 0 tests failed out of 3`.

## Implementation result

- Advertises the exact appended options: `UCI_ShowWDL`, `Move Overhead`, `Slow Mover`, `UCI_LimitStrength`, and `UCI_Elo`, with the required defaults/ranges.
- Accepts valid values and ignores malformed/out-of-range values without corrupting state. Multi-word option names and paths are joined correctly, including existing book paths.
- Snapshots compatibility options into `SearchOptions` at search start. Strength limiting is inert by default and uses a deterministic hook point when enabled; no random weakening was introduced.
- Applies timing controls in the required order and preserves the existing safety margin and node limit. Explicit depth, nodes, ponder, and infinite searches remain untimed, including when other time fields are present.
- Emits legal deterministic `wdl W D L` fields only when enabled, with deterministic mate conversion (`1000 0 0` / `0 0 1000`).
- Implements hidden `Debug`/`DebugFile` controls, executable-relative default/relative paths, mutex-protected best-effort logging, 8 MiB rotation, and three retained backups without writing diagnostics to UCI stdout or normal stderr.
- Preserves exactly-one completion behavior when option changes cancel and join active searches.

## Fresh VS x64 build verification

The first fresh configure attempt correctly exposed an environment issue: the VS developer shell resolved CMake 3.27.1 while the project requires 3.31+. No source failure occurred. The fresh verification was rerun with the installed CMake 4.4.2 executable while retaining the VS x64 compiler (`MSVC 19.44.35228.0`, `Hostx64\x64\cl.exe`):

```powershell
& 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\Launch-VsDevShell.ps1' -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
& 'C:\msys64\ucrt64\bin\cmake.exe' -S . -B out\task1-final-debug2 -G Ninja -DCMAKE_BUILD_TYPE=Debug
& 'C:\msys64\ucrt64\bin\cmake.exe' --build out\task1-final-debug2 --parallel
& 'C:\msys64\ucrt64\bin\cmake.exe' -S . -B out\task1-final-release2 -G Ninja -DCMAKE_BUILD_TYPE=Release
& 'C:\msys64\ucrt64\bin\cmake.exe' --build out\task1-final-release2 --parallel
```

Both configure/build sequences completed successfully (`90/90` build steps in each tree).

## Final verification

Focused fresh Debug verification:

```powershell
ctest --test-dir out\task1-final-debug2 -C Debug -R "koi_search_tests|uci_controller_tests|koi_engine_process" --output-on-failure
```

Result: `100% tests passed, 0 tests failed out of 3` (25.81 seconds).

Full Debug suite:

```powershell
ctest --test-dir out\task1-final-debug2 -C Debug --output-on-failure
```

Result: `100% tests passed, 0 tests failed out of 15` (82.85 seconds).

Full Release suite:

```powershell
ctest --test-dir out\task1-final-release2 -C Release --output-on-failure
```

Result: `100% tests passed, 0 tests failed out of 15` (38.18 seconds).

Final `git diff --check` reported no whitespace errors. The self-review found no unresolved functional concerns. The only environmental note is that fresh configuration requires CMake 3.31+ to be selected explicitly on this machine.
