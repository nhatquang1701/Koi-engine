# Final review fix wave: tutor MultiPV book bypass

Date: 2026-09-04
Base commit: `173effbb153f1aa84ecd78a1af81024d29a5717c`

## Original findings

### Important: normal tutor MultiPV could take the opening-book path

With `OwnBook=true`, a matching Polyglot record, and `MultiPV>1`, a normal
depth-limited `go` was eligible for a direct book completion. The eligibility
check excluded analysis mode, infinite, ponder, and `searchmoves`, but omitted
the controller's `multi_pv_` setting. As a result, the controller could emit
only `info string book move ...` and `bestmove` instead of the requested ranked
search variations.

### Minor: external match recipe was not color-balanced

The documented 40-game external recipe did not explicitly run Koi as both
White and Black, and could therefore rely on the harness default color.

### Acknowledged/deferred non-blockers

- Polyglot tests still do not separately harden present-but-uncapturable
  en-passant keys or every non-queen promotion decode.
- Zugzwang and LMR coverage remains safety/statistics oriented rather than a
  tactical-outcome-depth gate.
- Threaded confirmation improves throughput accounting but does not reduce
  short-suite latency.

Those items were intentionally left unchanged for this focused wave. The
tactical allowlists were not weakened.

## TDD red phase

Added a controller regression that leaves `OwnBook` at its default `true`,
creates a matching temporary start-position book entry, sets `MultiPV=3`, and
runs `go depth 1` without `UCI_AnalyseMode`. It requires valid ranks 1, 2, and
3, no book marker, exactly one legal completion, and empty diagnostics. A
gated output buffer releases on either the third variation or a book marker so
the regression fails immediately on the old behavior.

Added the equivalent real-engine process transcript in
`tests/uci_process_test.ps1`. It starts the engine from a separate working
directory with a temporary executable-relative book, first preserves the
single-PV book-hit check, then requests normal `MultiPV=3` and rejects a book
marker while requiring all three ranks and one completion.

Commands and observed failures before the production edit:

```powershell
cmd.exe /d /s /c 'call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 && "C:\msys64\ucrt64\bin\cmake.exe" --build "C:\Users\ntATh\AI test\Koi engine\out\final-review-debug-331" --target uci_controller_tests --parallel 4'
.\out\final-review-debug-331\uci_controller_tests.exe
```

Result: exit code `1`; the new case failed with:

```text
FAIL opening-book MultiPV bypass: MultiPV greater than one must bypass a matching opening-book entry
```

```powershell
cmd.exe /d /s /c 'call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 && "C:\msys64\ucrt64\bin\cmake.exe" --build "C:\Users\ntATh\AI test\Koi engine\out\final-review-debug-331" --target koi_engine --parallel 4'
powershell -NoProfile -ExecutionPolicy Bypass -File .\tests\uci_process_test.ps1 -EnginePath .\out\final-review-debug-331\koi-engine.exe
```

Result: exit code `1`; the new process assertion received:

```text
MultiPV greater than one must search instead of using the book: info string book move e2e4 depth 0
```

## Production fix

The book eligibility condition in `UciController::start_search` now additionally
requires `multi_pv_ == 1`. This is the narrow controller-level source of the
incorrect path choice. Single-PV normal book hits remain eligible; existing
analysis mode, infinite, ponder, `searchmoves`, generation, and completion
logic is otherwise unchanged.

## Green verification

Focused build and controller/process verification:

```powershell
cmd.exe /d /s /c 'call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 && "C:\msys64\ucrt64\bin\cmake.exe" --build "C:\Users\ntATh\AI test\Koi engine\out\final-review-debug-331" --target uci_controller_tests koi_engine --parallel 4'
.\out\final-review-debug-331\uci_controller_tests.exe
powershell -NoProfile -ExecutionPolicy Bypass -File .\tests\uci_process_test.ps1 -EnginePath .\out\final-review-debug-331\koi-engine.exe
```

Result: the controller target passed all 36 cases, including the new
`opening-book MultiPV bypass` case; the process transcript exited `0` with no
output or diagnostics.

Full configurations were built through the x64 Visual Studio environment and
then run with:

```powershell
C:\msys64\ucrt64\bin\ctest.exe --test-dir out\final-review-debug-331 -C Debug --output-on-failure
C:\msys64\ucrt64\bin\ctest.exe --test-dir out\final-review-release-331 -C Release --output-on-failure
```

Result: Debug `14/14` passed and Release `14/14` passed. The only displayed
match-harness exception is the expected illegal-opening negative fixture, and
that test is recorded as passed in both CTest logs.

## Documentation update

`README.md` and `task-6-report.md` now provide the eight exact commands for
both `1+0` and `5+3`, with no-book and licensed-book conditions, each split
into `-Games 20 -KoiColor white` and `-Games 20 -KoiColor black`. The harness
defines `-Games` per selected opening, so this is 40 games per opening and
condition. Both documents continue to state that no UCI opponent, licensed
book, or Lucas Chess installation was available and that no external result is
claimed.

## Changed files

- `src/koi/uci_controller.cpp`
- `tests/uci_controller_tests.cpp`
- `tests/uci_process_test.ps1`
- `README.md`
- `.superpowers/sdd/2026-09-04-elo-opening-book/task-6-report.md`
- `.superpowers/sdd/2026-09-04-elo-opening-book/final-review-fix-report.md`

## External limitations and concerns

No Stockfish or other supplied UCI opponent, licensed external book, or Lucas
Chess installation is available in this workspace. Consequently, the external
color-balanced match matrix and GUI registration remain unverified; no Elo,
match, book, or Lucas results were fabricated. The three acknowledged deferred
coverage/latency observations above remain concerns for later, separately
scoped work.
