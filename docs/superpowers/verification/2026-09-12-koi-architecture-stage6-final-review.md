# Koi Architecture Stage 6 / Final Architecture Review

Date: 2026-09-12

## Scope and decision

This review closes the architecture rework described by the temporary
`Goal.txt` task input. The review covers the repository audit, the implemented
Stages 1 through 5, the C++26 module and tooling boundaries, the Release and
Debug gates, deterministic benchmark profiles, the Stockfish 19 architectural
comparison, and the adversarial future-change test.

The resulting architecture is accepted as Koi's next development baseline.
Acceptance means that no remaining major foundational ownership or dependency
change was identified that should be made before future strength work. It does
not mean that every wall-clock-sensitive diagnostic is invariant on every host,
that Lazy SMP is already implemented, or that this work makes an Elo claim.
Those limits are recorded below rather than hidden.

## Evidence set

The staged evidence is retained in:

- [Stage 1 verification](2026-09-11-koi-architecture-stage1.md)
- [Stage 2 verification](2026-09-11-koi-architecture-stage2.md)
- [Stage 3 verification](2026-09-11-koi-architecture-stage3.md)
- [Stage 4 verification](2026-09-12-koi-architecture-stage4.md)
- [Stage 5 verification](2026-09-12-koi-architecture-stage5.md)
- [master architecture specification](../specs/2026-09-11-koi-architecture-rework-design.md)
- [repository README](../../../README.md)

The ignored Stage 6 profiles are the fixed-depth and timed thread runs under
`artifacts/verification/stage6-*.json`. The fixed-depth profiles contain the
same 64-position suite used by the earlier stages.

## Result against the acceptance areas

| Area | Result | Boundary and evidence |
| --- | --- | --- |
| Core rules | Accepted | Native `Position` owns legality, keys, rule state, history, and move generation. `CompatibilityMirror` is private compatibility infrastructure, not a second rules authority. Native-rule, differential, perft, replay, and make/unmake tests remain available. |
| Search | Accepted | `SearchSession`, `SearchContext`, `SearchStack`, and `RootCoordinator` separate request, worker, recursive, and root-line state behind `SearchService`/`SearchHandle`. Ordering and policy are private seams. |
| Evaluation and NNUE | Accepted | `EvaluationContext` selects a worker-local evaluator state or the existing fallback. Immutable network data is shared through `NnueEvaluator`; accumulators remain worker-local. Classical evaluation remains the safe default. |
| TT and budgets | Accepted | `SearchTableAccess` hides physical table storage from recursive search. `SearchBudget` separates serial validation from bounded shared reservation. Generation, locking, clear, resize, and mate normalization remain table-owned. |
| Runtime and UCI | Accepted | `SearchSession` owns cancellation and completion; `TimeManager` owns clocks and pacing; the UCI controller owns protocol state and is the only protocol writer. |
| Parallelism | Accepted with an explicit future boundary | Current root-parallel work has deterministic authoritative ranking and measurable thread profiles. The architecture does not claim to implement Lazy SMP or shared history yet; those are future consumers of the existing shared-vs-thread-local seams. |
| External systems | Accepted | Books and Syzygy remain adapters with safe missing/malformed-asset fallback. No external asset is a rules or recursive-search owner. |
| Modules and tooling | Accepted | Public `koi` partitions expose value contracts; implementation headers remain private. Module, package, process, replay, benchmark, measurement, training-wrapper, and strength tests are registered in the repository gate. |
| Performance | Accepted with measured scope | Threads=1 cold/warm fixed-depth profiles remained position-for-position identical through Stage 5. Stage 6 profiles show increasing work as root threads increase, while score/PV/best-move parity is retained. The timed short-oracle limitation is recorded separately. |
| Documentation | Accepted | This review, the README, and the master specification now describe the implemented boundaries, deliberate non-goals, evidence, and future extension points. |

## Module and tooling alignment

The aggregate `koi` module and the `koi:types`, `koi:position`, `koi:eval`,
`koi:tablebase`, `koi:search`, and `koi:runtime` partitions remain the stable
contract surface. No public Koi header or module exports a vendored
`chess.hpp` type, TT storage entry, worker implementation, or concrete NNUE
network representation. The private `src/koi/detail` headers contain the
implementation seams used by the search runtime.

The repository's existing tooling remains aligned with those contracts:

- `koi-perft` and replay exercise the rules boundary without exposing the
  vendored library.
- `koi-bench` emits deterministic cold/warm profiles separately from timed NPS
  measurements.
- UCI, En Croissant-style, match, and stability process tests keep stdout
  protocol-clean and verify lifecycle behavior.
- Python measurement, oracle, training-wrapper, forensic, and strength-report
  tests remain data/tooling boundaries rather than engine runtime dependencies.
- `tools/build/release_verify.ps1` remains the reproducible Debug/Release
  harness and records commands and artifacts without requiring Stockfish or a
  GUI to be installed.

## Stockfish-informed review

Stockfish 19 was used as an engineering reference for concepts, not as a
source layout to clone.

| Stockfish concept reviewed | Koi decision | Reason |
| --- | --- | --- |
| `Position` plus reversible `StateInfo` | Native `Position` is authoritative; the vendored board is a private compatibility mirror | Preserves one production rules authority while retaining book, completion, and differential compatibility consumers. |
| Worker-local search stack and per-thread state | `SearchContext` owns a fixed-capacity `SearchStack`, PV state, statistics, and evaluation context | Keeps recursive state out of `GameState` and makes future worker construction explicit. |
| Clustered TT, generations, and replacement policy | `TranspositionTable` owns physical storage and lifecycle; search sees `SearchTableAccess` | Keeps representation and memory policy replaceable without leaking them through recursion. |
| NNUE network versus accumulator state | Immutable evaluator data is shared; `EvaluatorWorker`/`NnueWorker` state is context-local | Allows feature/network changes without making `Position` or `SearchContext` know the network layout. |
| Time manager separated from search recursion | `TimeManager`, `SearchBudget`, and `SearchSession` keep timing, limits, and cancellation outside policy formulas | Preserves clear lifecycle semantics and makes pacing experiments local. |
| Lazy SMP shared/local distinction | Current root parallelism is retained; shared TT access and local ordering/evaluation state are explicit | Leaves room for a later scheduler and shared-history policy without prematurely coupling the recursive search to a particular SMP design. |
| Advanced pruning and move-ordering evolution | `SearchPolicy` and `SearchOrderingTables` are Koi-owned seams | Adopts the mature separation of concerns without importing Stockfish-specific formulas or ownership assumptions. |

## Ten-point architectural self-review

1. **Correctness:** Native-rule, differential, perft, replay, fixed-depth
   search, and completion gates preserve the chess and protocol invariants. The
   one short-clock oracle failure is a timing-sensitive test result, not a
   fixed-depth legality or deterministic-profile mismatch.
2. **Ownership:** Rules, compatibility, feature cache, recursive stack,
   ordering state, policy decisions, evaluation workers, TT storage, budgets,
   time, session lifecycle, and protocol output each have a named owner.
3. **Dependency direction:** Public contracts depend on Koi-owned values;
   vendored rules and implementation details point inward through private
   adapters. The public-boundary scan found no chess-library type leak.
4. **Performance:** The hot recursive path retains fixed-capacity stack state,
   explicit local counts, and private policy/ordering seams. Cold/warm
   Threads=1 profiles are unchanged through Stage 5, and thread scaling is
   measured separately rather than confused with deterministic node parity.
5. **Extensibility:** New ordering and policy algorithms have local homes and
   focused tests; they do not require edits to UCI, TT storage, or rules
   ownership.
6. **Concurrency:** Current root-parallel behavior has explicit shared TT and
   budget access plus local worker state. Lazy SMP and shared histories remain
   unimplemented future work, not hidden assumptions.
7. **NNUE:** Worker-local state, immutable network lifetime, feature contracts,
   fallback behavior, and scalar/SIMD-compatible evaluation boundaries are
   independently testable.
8. **Testing:** Every major boundary has focused tests, and the full 42-test
   Release and Debug registrations were exercised. The exact timed limitations
   are listed in the verification section; no test was weakened or removed.
9. **Documentation:** README, the master specification, staged verification
   records, and this review describe the actual implementation rather than an
   aspirational architecture.
10. **Future rewrite test:** The requested future changes below map to existing
    subsystem seams. No cross-cutting rewrite of rules ownership, public UCI
    contracts, or recursive search state is required by the current design.

## Ultimate future-change test

| Future request | Primary home | Architectural result |
| --- | --- | --- |
| Much stronger NNUE | `NnueNetwork`, `NnueEvaluator`, `NnueWorker`, `EvaluationContext` | Network format, inference, and accumulator work can evolve behind evaluation contracts; `Position` and UCI do not own the representation. |
| Different feature set | Feature extractor and NNUE/classical evaluation adapters | Feature identity and encoding can change at the evaluation boundary; only genuinely new rules data would require a deliberate `Position` contract change. |
| More sophisticated move ordering | `SearchMoveOrdering` and `SearchOrderingTables` | Ranking and adaptive state remain private to search. |
| New history heuristics | Ordering tables and worker-local search state | New tables can be added without changing TT storage or protocol code. |
| New pruning techniques | `SearchPolicy` plus explicit search-node context | Decisions can be introduced and tested without embedding policy in UCI, rules, or evaluator synchronization. |
| New reduction formulas | `SearchPolicy` | LMR/reduction calculations already have a policy home and a stable application point. |
| Singular extensions | `SearchPolicy`/`SearchContext` | The policy seam and recursive context provide the required decision inputs without redesigning state ownership. |
| Improved time management | `TimeManager`, `SearchBudget`, and `SearchSession` | Clock policy, node limits, cancellation, and iteration pacing are already separated from recursive formulas. |
| Lazy SMP | Root scheduling/runtime and explicit shared/local resource seams | A new scheduler and shared-history policy would be implementation work, not a public or rules architecture rewrite. |
| Better tablebase integration | `koi:tablebase` and external adapters | Asset discovery and probing can evolve without making tablebases a second rules authority. |
| SIMD optimizations | Evaluation implementation and CPU-feature dispatch | Scalar evaluation remains the reference boundary; fast paths can be added behind it. |
| Search experimentation infrastructure | Private policy/ordering seams, benchmark profiles, and focused tests | Experiments can be isolated, measured, and reverted without changing protocol or rules ownership. |

The review deliberately does not claim that these features are implemented.
It verifies that their likely changes are local to the appropriate subsystem
and that their required shared state has an explicit owner.

## Verification evidence

### Release

The fresh full command was:

```powershell
ctest --test-dir build/release -C Release --output-on-failure
```

Result: 41/42 tests passed in 416.15 seconds. The only failure was CTest test
14, `koi_search_tests`, in the short oracle case `b2b4 rook lift`:

```text
best=a7a5, depth=0, nodes=31, qnodes=3196
```

The surrounding search, rules, module, process, benchmark, packaging,
measurement, and strength tests passed. The same case has reproduced across
the staged runs with different short-clock node counts, so it remains a
wall-clock/configuration sensitivity rather than a stable fixed-depth or
legality regression.

### Debug

The full Debug command was:

```powershell
ctest --test-dir build/debug -C Debug --output-on-failure
```

The Debug result is recorded as 40/42 in 918.45 seconds: the same short-clock
`koi_search_tests` family remains configuration-sensitive (the reproduced
authoritative-root case returned `depth=0, nodes=1, qnodes=71, elapsed_ms=938,
root_pvs=0, root_research=0`), and `koi_benchmark_process` reaches its
300.12-second CTest timeout under Debug. This is a known build-configuration
limitation; it does not invalidate the Release benchmark process or the
fixed-depth profile evidence. Debug warnings remained the existing
`[[nodiscard]]` warnings in `koi_search_tests.cpp`.

### Deterministic and threaded profiles

The serial Stage 4 and Stage 5 profiles both contained 64 positions and had
zero position-by-position differences in PV, score, best move, nodes, or
qnodes. Stage 6 retained the same serial totals:

| Profile | Threads | Positions | Nodes | Qnodes |
| --- | ---: | ---: | ---: | ---: |
| `stage6-threads-1.json` | 1 | 64 | 2677 | 57638 |
| `stage6-threads-2.json` | 2 | 64 | 3597 | 111014 |
| `stage6-threads-4.json` | 4 | 64 | 4979 | 189545 |

Threads 1, 2, and 4 retained move/score parity in the benchmark comparison;
node and qnode totals increase with root-parallel work as expected. Timed
profiles are retained separately because wall-clock scheduling can change
their work totals.

### Boundary and hygiene checks

The final checks are:

```powershell
$leaks = rg -n '#include <chess\.hpp>|chess::(Board|Move|Color|Piece)' src/koi -g '*.hpp' -g '*.ixx' -g '!src/koi/detail/**'
if ($LASTEXITCODE -eq 0) { $leaks; throw 'public Koi headers or modules leak chess-library types' }
git diff --check
```

The public-boundary scan is empty and `git diff --check` is clean after the
final documentation changes. The repository status is checked before the
temporary task input is removed, and the final commit contains only the
review/documentation updates beyond the already committed architecture stages.

## Known non-foundational limitations

These findings are intentionally carried forward:

- The short-clock `b2b4` oracle is sensitive to host scheduling and can stop at
  depth zero with a different legal move. Fixed-depth deterministic profiles,
  legality, perft, and process completion checks remain separate and stable.
- Debug is substantially slower than Release; its benchmark process can hit
  the existing 300.11-second test timeout. The Release gate is the production
  performance configuration.
- Root-worker scheduling remains private implementation detail in
  `search_service.cpp`; the architecture has not prematurely introduced Lazy
  SMP, shared histories, or a new scheduler.
- The compatibility mirror remains because Polyglot/book behavior, completion
  validation, and differential diagnostics still consume it. Removing it now
  would discard a useful adapter boundary rather than improve ownership.
- No new Stockfish match, CPL, or Elo campaign is claimed. The measurement and
  match tooling remains usable, but external engine/corpus results are inputs
  to a future strength study, not evidence produced by this architecture task.

None of these limitations requires a foundational rewrite of the accepted
ownership model. They are explicit future implementation or environment
concerns with existing boundaries and tests.

## Final acceptance

The repository now has a coherent native state core, private compatibility
adapters, first-class search lifecycle and worker state, isolated ordering and
policy decisions, evaluator/NNUE worker ownership, TT/budget resource seams,
stable module contracts, and aligned verification/measurement tooling. The
future-change test passes at the architectural level: the twelve requested
strength and experimentation directions have subsystem homes without a new
cross-cutting ownership design.

The temporary `Goal.txt` file can therefore be removed as task input after
this review and the final hygiene checks; it is not part of the engine's
runtime or documentation surface.
