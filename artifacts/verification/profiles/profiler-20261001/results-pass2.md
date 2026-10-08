# Profiler pass 2 (2026-10-01 late night)

Isolated tree `build/roadmap-profiler-src`; koi-bench sha `71387471ABDA5777F85C412A2D167E51832A0F41688735FB5DEFB00757A5CF13`.
Run: `run_profiler2.ps1` (--warmup 0 --repeat 1, single sweep, depth 4..5, 16-fen sparse corpus).
Dominant sample: fen-015 depth 5 = 125,368,6 nodes / 128,275,558 qnodes / 526,926 ms.

## Phase counters (cycles, calls)

| phase | calls | cycles | cyc/call |
| --- | ---: | ---: | ---: |
| negamax | 20,508,890 | 21,567,923,382,299 | 1,051,645 (inclusive) |
| quiescence | 212,741,477 | 9,873,325,128,609 | 46,410 (inclusive) |
| evaluate | 87,400,511 | 273,882,373,823 | 3,134 |
| make | 215,278,263 | 160,883,127,528 | 747 |
| unmake | 215,278,263 | 9,536,323,287 | 44 |
| picker_prepare | 126,393,907 | 189,019,430,980 | 1,495 |
| picker_next | 302,811,188 | 224,908,618,359 | 743 |
| movegen | 206,715,206 | 1,209,421,014,795 | 5,851 |
| draw_status | 212,934,437 | 16,088,240,294 | 76 |
| tt_probe | 212,920,138 | 142,072,868,817 | 667 |

## Reading

- Instrumented leaves total ~2.23 T cycles vs negamax inclusive 21.57 T (~10%).
- `movegen` is the largest leaf by a wide margin: 1.21 T cycles (~54% of leaf time).
- The movegen scope wraps `GameState::legal_moves_with_metadata(...)`:
  pseudo-generation + legality probes + per-move metadata (gives_check) + finalize.
- Pass 3 splits it into gen_pseudo, legal_probe, metadata_build, metadata_finalize.
