# Task 1 report — Rough Elo estimation

## Changed files

- `tools/elo_estimate.py` — standard-library CLI and real paired-match workflow.
- `tests/elo_estimate_test.py` — deterministic unit and mocked-harness coverage.
- `tools/uci_match.ps1` — Koi `BookRandom` forwarding/reporting plus preserved opponent-Elo plumbing.
- `tools/stockfish_match.py` — `--book-random` forwarding (default `false`) and preserved opponent-Elo support.
- `tests/stockfish_match_test.py` — focused generic match-tool option coverage.

## Behavior delivered

- Validates `koi-elo-anchor-manifest-v1`, executable/opening/book hashes, external output paths, a 32-opening corpus, and the requested Stockfish path.
- Plans color-balanced 64-game batches: two anchors surrounding the prior, then adaptive batches through the configured 128–320-game cap.
- Launches `tools/uci_match.ps1` for every Koi-White/Koi-Black pass outside dry-run; preserves generated JSON and PGN artifacts.
- Rejects unusable v2 reports, including malformed, incomplete, illegal, timed-out, process-exit, max-plies, unproven replay, wrong-option, and wrong-provenance games.
- Reports Koi-perspective W/D/L and score, bracketed logistic fit with Jeffreys smoothing, deterministic 2,000 paired-opening bootstrap confidence intervals, reliability, hashes, and a timestamp-independent reproducibility hash.
- Keeps no-book and licensed-book settings distinct, with `BookRandom=false` in book mode and explicitly recorded by the harness.

## TDD evidence

The replacement tests were written before the implementation update. Their initial run failed with missing anchor-manifest/workflow helpers, missing BookRandom/opponent-Elo CLI arguments, and timestamp-hash mismatch. A later regression test failed with four harness passes instead of the required six for a low-confidence 128-to-192-game adaptive run. The implementation was then updated until the focused suite passed.

## Verification

Executed in `C:\Users\ntATh\AI test\Koi-engine-task1`:

```text
python -m unittest tests.elo_estimate_test tests.stockfish_match_test tests.elo_oracle_test -v
Ran 27 tests in 6.664s
OK

python -m py_compile tools\elo_estimate.py tools\stockfish_match.py tests\elo_estimate_test.py tests\stockfish_match_test.py
(exit 0)

powershell.exe -NoProfile -ExecutionPolicy Bypass -Command '<parse uci_match.ps1>'
PowerShell parse OK

git diff --check
(exit 0)
```

No test started a real engine: non-dry estimator tests patch process execution and emit deterministic v2 JSON/PGN artifacts.

## Concerns

- A real measurement still requires user-supplied Koi, replay, Stockfish/optional lower-anchor executables, manifest, opening file, and an output path outside the repository.
- The anchor manifest uses a documented nested `stockfish` object: `path`, `elos`, and `rating_source`; lower anchors are `id`, `path`, `rating`, and `rating_source` entries.

## Implementation commit

`db63e5581b267524b0dd9ba84c0884e638171baf` — `feat(tools): run reproducible rough Elo estimation`

## Review fix round 1

### Root cause and changes

- `reproducibility_hash` removed timestamp keys but recursively retained generated artifact `path` and `sha256` values. Canonicalization now preserves stable artifact kind information while deliberately omitting timestamp-bearing artifact paths, artifact hashes, and content metadata. Input/executable/configuration hashes remain canonical.
- `validate_match_manifest` validated games but did not inspect v2 `positions`, and it accepted any terminal string outside the incomplete-game deny-list. It now requires exactly 32 unique `positions[].Name` values matching the expected corpus, and accepts only `checkmate`, `stalemate`, and `rule draw`; `adjudicated draw` remains valid only with `result = "*"` and is normalized to a draw.

### TDD evidence

Before the code change, the two focused regressions failed:

```text
python -m unittest tests.elo_estimate_test.EloEstimateTests.test_reproducibility_hash_excludes_timestamp_fields tests.elo_estimate_test.EloEstimateTests.test_result_normalization_and_report_validation_rejects_bad_game_evidence -v
FAIL: artifact-only path/SHA/content metadata changes produced different reproducibility hashes
FAIL: missing positions evidence was accepted
FAIL: an unknown terminal label was accepted
```

After the implementation:

```text
Ran 2 tests in 0.024s
OK
```

### Verification

Executed in `C:\Users\ntATh\AI test\Koi-engine-task1`:

```text
python -m unittest tests.elo_estimate_test tests.stockfish_match_test tests.elo_oracle_test -v
Ran 27 tests in 6.651s
OK

python -m py_compile tools\elo_estimate.py tools\stockfish_match.py tests\elo_estimate_test.py tests\stockfish_match_test.py
(exit 0)

powershell.exe -NoProfile -ExecutionPolicy Bypass -Command '<parse uci_match.ps1>'
PowerShell parse OK

git diff --check
(exit 0)
```

The focused generic matcher tests continue to verify `Hash=16` by default, explicit `BookRandom=false`, and clamped opponent-strength forwarding.
