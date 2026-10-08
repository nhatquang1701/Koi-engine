# Fixed-time UCI throughput baseline

## Run identity

- Executable source revision: `0c7c47d3b7f62ebfcda96e25164e6a09ac79a583`.
- Search limit: 1,000 ms per position; the harness sends `go infinite` and
  issues `stop` at the common wall-clock deadline.
- Options: Hash 512 MiB, Threads 1 and 4.
- Corpus: `tests/data/positions/evaluation-positions.txt` (10 named FENs),
  SHA-256 `b5b211554eef981a8d31ed00c6f5ef4b3ef4e5e606f6e6ff6d9a885bad769db0`.
- Koi executable: Release AVX2 search binary
  `build/roadmap-release/koi-engine-avx2.exe`, SHA-256
  `36baff122fa3d1bdfE5B8B00777D7BAB2BD7FB32BE15E9A9B1A0A103CE9AA201`.
- Stockfish 19 executable SHA-256:
  `45bc8e4969147db9c2eb533810637994619bff0eacc81ccfd9854394901bcbd0`.
- Raw report: `artifacts/verification/uci-throughput/roadmap-current-baseline-fixed-stop-1000ms-avx2.json`.
  SHA-256 `b30d75b546b741839fcc565f917fe9ac8b3d44987a07408a0de5ae3c07eec775`.

The AVX2 binary is the search process selected by the Windows `koi-engine.exe`
dispatcher on this host. Measuring it directly ensures Windows peak working set
covers the search process rather than only the small dispatcher parent.

## Results

Each row summarizes the ten positions for one engine/thread configuration.
NPS is the median of each position's final completed info line and is reported
within each engine; these values are not a cross-engine strength comparison.

| Engine | Threads | Completed-depth range | Median search time | Median NPS | Peak working set |
| --- | ---: | ---: | ---: | ---: | ---: |
| Koi AVX2 | 1 | 3–7 | 1,013.5 ms | 278,732.5 | 525.0 MiB |
| Koi AVX2 | 4 | 3–7 | 1,013.0 ms | 467,478 | 531.5 MiB |
| Stockfish 19 | 1 | 21–118 | 1,004.0 ms | 2,572,534 | 755.2 MiB |
| Stockfish 19 | 4 | 23–238 | 1,005.5 ms | 8,598,426.5 | 820.0 MiB |

The fixed-time stop produced searches near the requested one-second budget for
all 40 engine/position runs. The report records each position's depth rows,
nodes, NPS, best move, actual search time, stop latency, executable hash, and
peak working set.

## Interpretation

This is a throughput snapshot for one small, fixed corpus. It is not a playing
strength test and does not replace the color-balanced game baseline. Stockfish
reaches much deeper iterations within this budget on these positions; profile
Koi before selecting a speed change, then repeat the same harness after each
candidate. Keep numerical results in local verification artifacts rather than
GitHub release notes.

The earlier launcher-based report and early-return `go movetime` report are
diagnostic artifacts only. This fixed-stop AVX2 report is the canonical
throughput result for this source build.
