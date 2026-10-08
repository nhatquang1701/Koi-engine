# Phase-profiler results (temporary isolated build, 2026-10-01)

Build: `build/roadmap-profiler-release` (isolated tree `build/roadmap-profiler-src`,
HEAD 0eb5c0e + prof::Scope instrumentation). Bench sha256 pass 1:
E7BA2722873D0BC4F5F9C4F4B3EB0D1CBC35ABBB31527078521A8A8CD55A13BA.

## Pass 1 (warmup 1 + repeat 3, depth sweep 4..5, sparse corpus 16 fens)

`profiler-stderr.txt` (raw dump), `bench-report.json` (median summary).

| phase          | calls      | cycles         | % negamax | cyc/call |
|----------------|-----------:|---------------:|----------:|---------:|
| negamax        | 82,035,560 | 84,802,512,427,293 | 100.00 % | 1,033,729 |
| quiescence     | 850,965,908 | 38,682,216,776,567 | 45.61 % | 45,457 |
| evaluate       | 349,602,044 | 1,090,680,456,099 | 1.29 % | 3,120 |
| make           | 861,113,052 | 645,676,542,109 | 0.76 % | 750 |
| unmake         | 861,113,052 | 37,688,044,509 | 0.04 % | 44 |
| picker_prepare | 505,575,628 | 749,930,820,577 | 0.88 % | 1,483 |
| picker_next    | 1,211,244,752 | 887,114,767,783 | 1.05 % | 732 |

Interpretation notes:

- Counters accumulate over all sweeps (warmup included); the bench report is
  the median sweep. qnodes match (~4 x 226 M vs 851 M qsearch calls); the
  bench's `nodes` counts only the final iteration, so negamax calls run ~8x
  higher than the printed node counts.
- Nested scopes inflate the inclusive sums; percentages are shares of the
  negamax inclusive sum, not wall time.
- Leaf phases account for roughly 3,600 cycles per search node; total measured
  wall is about 12,000-13,000 cycles per node, so most time sits in
  negamax/quiescence self work: move generation, TT, rule checks, move loop.
- Pass 2 adds `movegen`, `draw_status`, and `tt_probe` scopes to locate that
  remaining share.
