# Current-source 1-second baseline calibration review

## Run identity

- Source revision recorded before the run: `0c7c47d3b7f62ebfcda96e25164e6a09ac79a583`.
- Koi Release executable SHA-256: `4617473eaa477622807bfc3b0dd6a205b117df902f28c9a94bd6de8ee7b3c555`.
- Stockfish 19 executable SHA-256: `45bc8e4969147db9c2eb533810637994619bff0eacc81ccfd9854394901bcbd0`.
- Host: Windows 11, Intel i3-10100F (4 cores / 8 threads), 32 GiB RAM, GTX 1060 6 GB.
- Options: 1,000 ms per move, Threads 4, Hash 512 MiB, Speed 100, OwnBook false.
- Pairing: 32 openings, each played with Koi White and Koi Black, against Stockfish 19 at UCI Elo 1400, 1600, and 1800.
- Source report: `artifacts/matches/roadmap-current-baseline-1s.json`.

## Evidence validation

The estimator exited successfully after the three initial anchor batches (192
games). The aggregate report's reproducibility hash matched a fresh calculation.
All 13 listed artifact hashes matched: six color-batch JSON reports, six PGNs,
and the exported schedule. The six raw reports each contained 32 games with the
expected openings, clean engine shutdowns, and legal replayed moves. The raw
results total 105 wins, 13 draws, and 74 losses for Koi.

The Release executable, replay tool, Stockfish binary, anchor manifest, and
opening corpus hashes also matched the aggregate report. No engine binary or
C++ search source changed during the run.

## Scores by anchor

The intervals below use 20,000 deterministic bootstrap resamples of the 32
paired opening scores at each anchor (seed `20260927 + anchor rating`). Each
opening's score is the average of its Koi-White and Koi-Black games.

| Stockfish UCI Elo | Koi White W-D-L | Koi Black W-D-L | Total W-D-L | Koi score | Paired-opening 95% interval |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1400 | 30-2-0 | 26-3-3 | 56-5-3 | 0.914 | 0.852–0.969 |
| 1600 | 20-1-11 | 10-2-20 | 30-3-31 | 0.492 | 0.383–0.609 |
| 1800 | 9-3-20 | 10-2-20 | 19-5-40 | 0.336 | 0.219–0.453 |

## Calibration decision

Do not use a single interpolated Elo from this run. The paired-opening interval
for the 1400 anchor does not overlap the interval for 1600, and the 1600 result
has a large color split (0.641 with Koi White versus 0.344 with Koi Black).
The 1800 color scores are close (0.328 and 0.344). These anchor and color
differences need investigation before rating targets or comparisons to the
historical OpenCode estimate are useful. The pooled fit retained in the
machine-generated report is diagnostic output only; the anchor calibration
gate failed.

The estimator stopped after the initial 192 games because its pooled
confidence check passed, so no adaptive batches ran. That stopping rule does
not override the anchor-consistency finding above.

## Execution notes

Short Python tests and a documentation build overlapped earlier parts of the
long baseline. During the final 1800-anchor batches, the README was edited and
`git diff --check` was run; these did not rebuild or modify the engine binary.
No second match runner or CPU benchmark was started during the baseline.
