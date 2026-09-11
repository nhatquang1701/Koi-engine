# Koi Architecture Stage 5 Design: TT, Budget, and Parallel Runtime Seams

Date: 2026-09-12

## Objective

Make the search-facing ownership of transposition-table access and node-budget
accounting explicit before any future thread-scaling or Lazy-SMP work. Preserve
the existing striped `TranspositionTable`, time-management decisions,
cancellation behavior, node limits, and deterministic root-parallel policy.

## Design

`detail::SearchTableAccess` is the only TT boundary used by `SearchContext`. It
owns the enabled/disabled search policy for one context and delegates
probe/store to the existing public `TranspositionTable`, which remains the
owner of physical storage, locking, generations, clear epochs, and mate-score
normalization. Selective verification can temporarily disable the access
object without changing the shared table.

`detail::SearchBudget` owns node-limit accounting for one context. In serial
search it validates the local node count; root-parallel contexts share an
atomic counter supplied by the run coordinator and reserve with a bounded CAS
loop. The budget exposes only `reserve(local_nodes)` and
`visited(local_nodes)`, keeping atomic ownership and limit semantics out of
recursive search. `TimeManager` remains the owner of clock deadlines and
iteration pacing; cancellation remains owned by `SearchSession`.

```text
SearchService run
  ├── shared TranspositionTable (striped storage + generations)
  ├── TimeManager (clock/deadline/iteration policy)
  ├── shared SearchBudget counter when node-limited + parallel
  └── worker SearchContext
        ├── SearchTableAccess
        └── SearchBudget view
```

This stage does not add new parallel scheduling, shared histories, or Lazy SMP.
It establishes the shared/thread-local contract that those features can use
later without moving TT or node-limit logic through the recursive algorithm.

## Verification contract

Add focused tests for enabled/disabled TT access, local budget exhaustion,
shared budget reservation at a hard limit, and visited-node observation. Keep
existing TT clear/generation/mate-score tests, time-manager tests, deterministic
root-parallel tests, full process gates, and cold/warm benchmark parity.
