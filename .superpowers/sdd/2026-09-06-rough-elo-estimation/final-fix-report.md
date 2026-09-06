# Rough Elo estimation final-fix wave

## Scope and files changed

- `tools/uci_match.ps1`: separates protocol reads from chess-clock search
  deadlines, adjudicates over-clock moves as timeouts, and records opponent
  strength configuration.
- `tools/elo_estimate.py`: canonicalizes relevant UCI option provenance,
  rejects conflicting duplicates, verifies report configuration and exact
  startpos/opening prefixes, and records local-measurement inputs.
- `tools/stockfish_match.py` and `tools/README.md`: clarify the protocol
  timeout and document strength aliases, clamping, and disabled-by-omission
  behavior.
- `tests/elo_estimate_test.py` and `tests/stockfish_match_test.py`: regression
  coverage for provenance/configuration, opening evidence, report metadata,
  reproducibility inputs, and operator documentation.
- `tests/uci_match_fixture.cpp`, `tests/uci_match_clock_test.ps1`, and
  `CMakeLists.txt`: delayed fixture coverage for legal-before-deadline and
  timeout-after-deadline 1+0 behavior, registered in CTest.

## Red/green evidence

The focused pre-fix Python regression command exited 1 with eight failures and
one error: it accepted conflicting options/configuration drift and opening
sequence changes, and omitted measurement metadata/documentation. The pre-fix
fixture-backed clock test threw because a legal six-second 1+0 move was rejected
by the one-second protocol timeout. No real engine match was run.

## Verification

| Command | Exit | Relevant result |
| --- | ---: | --- |
| `python -m unittest tests/elo_estimate_test.py tests/stockfish_match_test.py -v` | 0 | 18 tests passed after the fixes. |
| `powershell ... Parser::ParseFile(tools/uci_match.ps1)` | 0 | PowerShell parse OK. |
| `powershell -File tests/uci_match_clock_test.ps1 ...` | 0 | `PASS UCI match clock deadline integration`. |
| `cmake -S . -B <temp-build> -G "Visual Studio 17 2022" -A x64` | 0 | Reconfigured the temporary MSVC x64 build. |
| `cmake --build <temp-build> --config Debug --target koi_engine` | 0 | Built the required fixture, replay helper, and engine target. |
| `ctest --test-dir <temp-build> -C Debug -R '^koi_uci_match_clock$' --output-on-failure` | 0 | 1/1 passed in 67.90 seconds. |
| `python -m unittest discover -s tests -p '*_test.py' -v` | 0 | 33 tests passed with the built replay helper. |
| `python -m py_compile tools/elo_estimate.py tools/stockfish_match.py tests/elo_estimate_test.py tests/stockfish_match_test.py` | 0 | Python compilation OK. |
| `git diff --check` | 0 | No whitespace errors. |

The temporary MSVC build emitted only `MSB8029` warnings because its output
directory was under `%TEMP%`; the build and tests still completed successfully.

## Unresolved concerns

None functionally. The exact 1+0 over-clock regression intentionally waits for
the real 60-second chess-clock boundary; CTest caps it at 120 seconds and the
observed run completed in 67.90 seconds.
