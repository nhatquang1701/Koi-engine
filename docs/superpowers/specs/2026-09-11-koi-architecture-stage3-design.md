# Koi Architecture Stage 3 Design: Ordering and Search-Policy Seams

Date: 2026-09-11

## Objective

Give move-ordering state and search-policy decisions explicit private owners so
future history, pruning, reduction, and extension experiments can be changed
without editing unrelated recursive search code. This stage is structural: it
preserves all current formulas, thresholds, diagnostics, UCI behavior, and
fixed-depth search results.

## Non-goals

- Add a new pruning, reduction, extension, or history heuristic.
- Tune current constants or change move ordering priorities.
- Expose ordering or policy types through public headers or C++ modules.
- Change the `SearchService`, `SearchHandle`, `SearchLimits`, or UCI contracts.
- Remove the existing `SearchMoveOrdering` façade used by tests and search.

## Ownership design

`SearchOrderingTables` owns the mutable adaptive tables used by one search
context: killers, quiet history, counter moves and confidence, and
continuation history. `SearchMoveOrdering` retains the ranking scratch storage
and move-scoring façade, delegates all adaptive-table mutation and lookup to
`SearchOrderingTables`, and remains the only ordering consumer of static
exchange details.

`SearchPolicy` is a pure, private decision layer. It accepts the scalar facts
already computed by `SearchContext` and returns small decision records for:

- null-move eligibility and reduction;
- check-extension eligibility;
- late-move candidate detection, high-history exclusion, reduction, and
  whether the move is actually reduced;
- quiet futility pruning; and
- quiescence capture pruning, distinguishing SEE and delta pruning.

Position feature extraction, move legality, and the computation of whether a
quiet move is forcing remain in `SearchContext` and the existing support
boundary. `SearchPolicy` does not inspect `GameState`, allocate, mutate search
state, or write statistics. The context applies the returned decisions and
continues to own recursion, make/unmake, statistics, and abort handling.

## Data flow

```text
SearchContext
  ├── SearchMoveOrdering ── SearchOrderingTables
  └── SearchPolicy decision records
        └── context applies stats, depth, make/unmake, and recursion
```

The seam is intentionally one-way. Ordering tables do not know about search
depth policy, and policy decisions do not know how histories are stored. This
leaves room for later replacement of one subsystem without creating a second
source of truth in `SearchContext`.

## Compatibility and performance

The policy functions remain inline and scalar so the compiler can fold them in
the recursive path. Ordering tables use the same fixed-size arrays and update
equations as the current `SearchMoveOrdering`; extraction must not add heap
allocation, locking, or virtual dispatch. The existing stable UCI-coordinate
tie-break and deferred SEE behavior remain unchanged.

## Verification contract

`search_policy_tests` will exercise real policy decisions and table ownership:
null-move windows, check-extension limits, LMR gating/history exclusions,
futility boundaries, SEE/delta classification, killer/history reset, and
proven counter-move lookup. Existing ordering, search, rules, UCI, process,
and benchmark tests remain required. Any unexplained PV, score, node, qnode,
or legality difference stops the stage.
