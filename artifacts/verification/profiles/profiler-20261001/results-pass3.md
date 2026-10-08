# Profiler pass 3 (2026-10-02 night)

Isolated tree `build/roadmap-profiler-src`; koi-bench sha `732FD4C3C0BA43996C4C127010F9D1967CCDF1BBDF8E06D2B55556E073774C02`.
Run: `run_profiler2.ps1` (--warmup 0 --repeat 1, depth 4..5, 16-fen sparse corpus). Wall ≈ 924 s.

CAUTION: negamax/quiescence totals sum nested frame durations (recursion inflates them ~7x vs wall);
leaf phases below are non-nested and directly comparable.

| phase | calls | cycles | cyc/call |
| --- | ---: | ---: | ---: |
| negamax (inclusive, nested) | 20,508,890 | 24,669,187,050,270 | — |
| quiescence (inclusive, nested) | 212,741,477 | 11,197,420,123,291 | — |
| evaluate | 87,400,511 | 280,692,426,817 | 3,212 |
| make | 215,278,263 | 160,028,354,950 | 743 |
| unmake | 215,278,263 | 9,780,897,168 | 45 |
| picker_prepare | 126,393,907 | 189,062,813,216 | 1,496 |
| picker_next | 302,811,188 | 224,492,934,513 | 741 |
| movegen (wraps legal_moves_with_metadata) | 206,715,206 | 1,569,006,153,500 | 7,590 |
| - gen_pseudo | 320,044,648 | 198,895,135,316 | 621 |
| - legal_probe | 2,278,815,822 | 515,784,092,653 | 226 |
| - metadata_build | 1,751,361,206 | 340,403,344,684 | 194 |
| - metadata_finalize | 206,715,222 | 17,337,743,520 | 84 |
| - unaccounted glue | 206,715,206 | 496,566,837,769 | ~2,400 |
| draw_status | 212,934,437 | 16,567,567,579 | 78 |
| tt_probe | 212,920,138 | 144,009,593,084 | 676 |

Notes:

- ~6.4 B scope calls; instrumentation overhead ≈ 100-120 cyc/call ≈ 0.7 T cycles (~20% of wall) inflates every row.
- movegen children sum 1,072 T; glue ≈ 497 T (~2,400 cyc/call) is the biggest unexplained block.
- Next: inspect `legal_moves_with_metadata` / `legal_moves_into_impl` for glue composition
  (possible per-node allocation or double metadata build).
