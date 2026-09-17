# Koi UCI Controller Improvement Verification

**Date:** 2026-09-16
**Plan:** `docs/superpowers/plans/2026-09-16-uci-controller-improvement.md`
**Scope:** Phase 1 robustness quick wins, Phase 2 table-driven option registry,
Phase 3 in-place ponderhit continuation, plus README and plan updates.

## Environment

- Windows x64, MSVC 14.44.35207 (Visual Studio 2022 Community), CMake/Ninja.
- Trees: `build\release` (Release) and `build\debug` (Debug), previously configured with the same toolset.
- Commands were run from a shell initialized with `vcvars64.bat`; the recorded form is the repository-standard configure/build/test sequence.

## Changed files

| File | Change |
| --- | --- |
| `src/koi/uci_controller.cpp` | `ucinewgame` hash clear + debug event, `debug on\|off`, `kBareGoFallback`, book completion guard, `kUciOptions` registry for handshake and `setoption`, `hashfull` in `info`, in-place `ponderhit`. |
| `src/koi/transposition_table.hpp/.cpp` | `hashfull_permill()` with a bounded 1000-slot sample. |
| `src/koi/search_service.hpp/.cpp` | `hashfull_permill()` forwarding; `SearchHandle::request_ponderhit`. |
| `src/koi/detail/search_session.hpp/.cpp` | Ponderhit hand-off: `request_ponderhit`, `take_ponderhit_limits`, `ponderhit_requested`, `wait_until_stopped_or_ponderhit`. |
| `src/koi/time_manager.hpp/.cpp` | `initialize(...)` extraction and `reconfigure(...)`. |
| `src/koi/detail/search_runner.cpp` | Mutable limit copy; consumes a pending conversion at both iteration-loop tops; terminal ponder roots wake on ponderhit. |
| `tests/unit/runtime/uci_controller_tests.cpp` | `ucinewgame` hash test, `debug on\|off` test, registry drift test, ponder continuation lifecycle test, `hashfull` parsing. |
| `tests/unit/search/koi_search_tests.cpp` | In-place conversion test, transposition-table hashfull test. |
| `tests/support/UciSession.psm1`, `tests/integration/uci/en_croissant_uci_test.ps1` | Strict `info` regexes accept `hashfull`. |
| `README.md` | UCI behavior, timing, hidden diagnostics, and Lucas Chess ponder sections. |

## Release evidence

Build:

```powershell
cmake --build build\release --config Release
```

Result: success; all targets linked including `koi-engine.exe`. The only warnings were the five pre-existing `C4834` `[[nodiscard]]` warnings in `koi_search_tests.cpp`.

Full suite:

```powershell
ctest --test-dir build\release -C Release --output-on-failure
```

Result: **45/45 passed in 445.20 seconds (0 failures).** Notable entries:

| Test | Result | Time |
| --- | --- | --- |
| `uci_controller_tests` | Passed | 1.62 s |
| `completion_gate_tests` | Passed | 0.02 s |
| `koi_search_tests` | Passed | 251.77 s |
| `time_manager_tests` | Passed | 0.03 s |
| `koi_engine_process` | Passed | 1.74 s |
| `koi_engine_en_croissant_process` | Passed | 0.79 s |
| `koi_engine_time_safety_process` | Passed | 11.32 s |
| `koi_benchmark_process` | Passed | 43.79 s |
| `koi_uci_match_clock` | Passed | 68.22 s |
| `cutechess_stability_smoke` | Passed | 27.79 s |

Behaviour-specific evidence:

- **Byte-identical handshake:** `test_uci_handshake_has_identity_and_supported_options_in_order` compares the complete `uci` handshake output to a literal, including the dynamic `Threads` maximum; it passes with the registry-driven writer. `test_handshake_option_table_is_unique_and_well_formed` additionally proves the advertised options are unique and well formed (25 at the time of that change; the fixture-driven contract test added on 2026-09-17 now owns the exact list) and that setting every option to its default stays silent.
- **`ucinewgame` clears the hash:** `test_ucinewgame_clears_the_transposition_table_and_records_it` reads the Debug JSONL log and requires `"event":"ucinewgame"` with `"hash_cleared":true` after cancelling an active search.
- **`debug on|off`:** `test_debug_command_toggles_hidden_diagnostics` proves commands after `debug off` are not logged, the earlier `position` command was logged, and no default `koi-debug.log` is created.
- **`info ... hashfull`:** the strict parser in `uci_controller_tests.cpp`, plus the PowerShell regexes in `tests/support/UciSession.psm1` and `en_croissant_uci_test.ps1`, accept `hashfull` between `nps` and `time` with a 0..1000 bound. `test_transposition_table_reports_hashfull_occupancy` proves 0 for a fresh table, 1000 after filling every cluster, and 0 again after `Clear Hash`.
- **Ponder continuation:** `test_ponderhit_continues_the_ponder_search_in_place` asserts exactly one `bestmove` (legal in the root), exactly one `search start generation` Debug entry, and a `"continued":true` ponderhit event; `test_ponderhit_converts_a_running_ponder_search_in_place` proves the runner honours the converted depth limit and completes once with `completed && !cancelled && !failed`. `koi_engine_process`, `koi_engine_en_croissant_process`, and `koi_engine_time_safety_process` all remain green, covering stop-after-ponderhit and immediate-ponderhit flows.

## Debug evidence

Build:

```powershell
cmake --build build\debug --config Debug
```

Result: success; all targets linked.

Touched suites:

| Test | Result | Time |
| --- | --- | --- |
| `uci_controller_tests` | Passed | 9.76 s |
| `time_manager_tests` | Passed | 0.35 s |
| `completion_gate_tests` | Passed | 0.55 s |
| `koi_engine_process` | Passed | 4.76 s |

The full Debug `koi_search_tests` run reaches the repository's generic 600-second CTest timeout. This is the previously documented Debug build-configuration limitation (the 2026-09-12 stage 6 record notes the `koi_search_tests` family is configuration-sensitive under Debug and that Debug is substantially slower than Release), not a new failure. The newly added and directly affected cases were run individually under Debug and pass:

```powershell
$env:KOI_TEST_FILTER='transposition table hashfull'; .\build\debug\koi_search_tests.exe
$env:KOI_TEST_FILTER='ponderhit in-place conversion'; .\build\debug\koi_search_tests.exe
$env:KOI_TEST_FILTER='ponder time and node limits'; .\build\debug\koi_search_tests.exe
```

Result: `PASS transposition table hashfull`, `PASS ponderhit in-place conversion`, `PASS ponder time and node limits`, exit code 0 for each.

## Known limitations

- `hashfull` is a bounded 1000-slot approximation across all stripes, matching the field's purpose; it is not an exact occupancy count.
- Pre-loop heuristics and the tablebase eligibility gate in `search_runner` are computed from the original ponder limits and are not recomputed after a ponderhit conversion.
- Debug `koi_search_tests` exceeds its 600-second CTest budget, as already recorded for the pre-change baseline.

No commit was made; the changes remain in the working tree.
