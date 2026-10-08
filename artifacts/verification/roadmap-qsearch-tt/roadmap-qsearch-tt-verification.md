# Quiescence TT cutoff ordering verification

Date: 2026-09-27  
Base source commit: `c49e233f6ef3232ace398dc18840e37afb62c44f`  
Branch: `koi-engine`  
Change status: local, uncommitted, not pushed

## Change

Eligible non-check exact and fail-high lower-bound TT entries now return before
quiescence tactical move generation. Automatic draws are handled before those
cutoffs; checked positions still generate evasions first so checkmate keeps
precedence. Claimable draws and repetition-sensitive paths remain ineligible.

Two diagnostic fields record qsearch TT cutoffs and qsearch move-generation
calls in `SearchStats` and the `koi-bench --profile-json` output. The counters
include the stack-boundary and quiet-check generators and aggregate across SMP
workers.

## Build and correctness

- Full MSVC Release build: all 289 final Ninja steps completed across the
  configured CPU variants. The earlier pre-shadow-diff build also completed
  all 292 of its then-configured steps.
- Full Release CTest before the final counter-only stack-boundary increments:
  76/76 passed at `-j2` in 567.18 seconds. Log:
  `roadmap-qsearch-tt-release-ctest.log`.
- After final counter instrumentation, the affected Release gate passed 8/8:
  all four search shards, native rule state, perft, shadow-diff, and benchmark
  process/profile JSON. It took 279.04 seconds at `-j2`; log:
  `roadmap-qsearch-tt-postinstrument-ctest.log`.
- Direct qsearch regressions passed 3/3: exact TT bypass, null-window lower
  bound versus wide window, and forced-draw/checkmate precedence.
- Shadow-diff passed 1/1 after enabling `KOI_BUILD_SHADOW_DIFF` in the Release
  build.
- The first profile-contract test failed on the expected missing new counter;
  it passed after adding the counters. The direct exact-TT regression failed on
  the old move-generation ordering and passed after moving the cutoff.

## Paired profile

The pre-change baseline was rebuilt from the base commit with the same counter
instrumentation. Baseline and candidate used MSVC Release, Threads=1, Hash=512,
Speed=100, classical evaluation, one warmup, five repeats, and the 64-position
strength profile.

| Measure | Baseline | Candidate |
| --- | ---: | ---: |
| Qnodes in primary profile rows | 692,068 | 691,955 |
| Qsearch move-generation calls | 695,605 | 611,810 |
| Qsearch TT cutoffs | 75,487 | 75,879 |
| Sum of primary-row elapsed time | 2,241 ms | 2,156 ms |

The baseline and candidate primary rows had identical score, PV, and main-node
count for all 64 positions. Qnodes were lower by one in `evasion_02` and 112 in
`fork_02`, where eligible qsearch cutoffs omit descendants. Reports:
`baseline-profile.json` and `candidate-profile.json`.

## Alternating speed gate

The official 2..5 depth sweep ran five alternating baseline/candidate pairs on
all 64 evaluation positions at Threads=1 and Speed=100. The existing total and
per-row regression limits were 2%; 24 timer-quantized rows were excluded from
the row verdict. Result: **PASS**.

- Baseline aggregate median NPS: 327,797
- Candidate aggregate median NPS: 349,868 (+6.7331%)
- Median per-row NPS change: +3.0948%
- JSON: `speed-gate-depth2-5/speed-gate.json`

Paired Release executable SHA-256 hashes:

| Artifact | Baseline | Candidate |
| --- | --- | --- |
| `koi-bench.exe` | `8B5BF9A28142C69D6038B03772B70CF253DEFF15636085DBA98FFCF3318A95C4` | `4F8E7D726B8D5DDC6226902F93C93B5D1F832644C6CA7E95706A32FAEB48B625` |
| `koi-engine-avx2.exe` | `85D276C515CF5DC0AFC065A3AADBE35CD634E7B01076646D00D1AFCB024A2268` | `8613E3E1D3796163B35AC414C2EDD7EF340E61BEEA58E501F294240304C98FB9` |

An initial 2..7 attempt was terminated after its uncapped first baseline
invocation searched for more than four minutes without writing a report. Its
diagnostic directory is preserved at `speed-gate/`; the bounded 2..5 run
provides the passing gate.

## Strength sample

`sprt_compare.ps1` used candidate versus baseline at 20,000 nodes, Threads=1,
Hash=512, books disabled, and 32 color-reversed openings. The hypotheses were
0 versus +10 Elo at alpha=beta=0.05. It reached the cap of 64 games per color:

- Combined: 128 games, 36 wins / 56 draws / 36 losses, score 0.500
- Combined LLR: -0.099, decision `inconclusive`
- White leg: 24/28/12, LLR +0.595, `inconclusive`
- Black leg: 12/28/24, LLR -0.694, `inconclusive`

Raw JSON, PGN, and logs are under
`../../matches/roadmap-qsearch-tt-sprt/qsearch-tt-20k-20260927-223035/`.
This result does not establish an Elo change. It showed no combined score
imbalance in this sample; the sample was underpowered to distinguish 0 from
+10 Elo. Retain this change as a speed-only optimization because the direct
profile outputs matched and the speed gate passed. Continue to require a
decisive strength gate for future behavior-changing search/evaluation changes.

## Environment and evidence limits

The host is the recorded Intel i3-10100F system (4 cores / 8 threads, 32 GiB
RAM, GTX 1060 6GB). CMake 3.31.6 and MSVC 19.44 were used. CUDA NNUE PTX was
built, but these searches used the classical evaluator. The Windows sampled
stack profiler remains unavailable because WPR returned error `0xc5585011`;
no profiling permissions were changed.
