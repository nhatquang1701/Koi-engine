# Koi Architecture Stage 4 Design: Evaluation State and NNUE Evolution Seam

Date: 2026-09-12

## Objective

Separate evaluator execution state from search recursion so a worker can own
incremental or cached evaluation state without changing `Position`,
`GameState`, or the recursive search algorithm. Preserve the classical default,
NNUE opt-in/fallback behavior, scalar inference results, and network lifetime.

## Design

`Evaluator` gains an optional `create_worker()` capability. The default returns
no worker, so existing third-party and classical evaluators retain their
current virtual contract and are called through the existing concurrency guard.
`NnueEvaluator` overrides the capability with a private adapter that owns one
`NnueWorker` and therefore one accumulator per search context.

`detail::EvaluationContext` owns the execution choice for one search context:
it stores the evaluator reference, an optional evaluator worker, and the
existing serialization mutex pointer for evaluators that do not provide a
worker. Its `evaluate` method is the only evaluation call made by
`SearchContext`. It does not own the network or rules state; immutable network
data remains shared through `NnueEvaluator`, while accumulator storage remains
worker-local.

```text
SearchContext
  └── detail::EvaluationContext
        ├── optional EvaluatorWorker -> NnueWorker -> immutable network
        └── fallback Evaluator + existing concurrency guard
```

Feature extraction remains a value-oriented `EvaluationFeatureExtractor`
boundary for now. This stage does not invent incremental make/unmake updates;
it makes the per-worker execution seam explicit so a later feature/accumulator
implementation can evolve without changing search or rules ownership.

## Compatibility and performance

The default classical path performs the same evaluator call and uses no worker
allocation beyond the context's empty optional. NNUE search contexts allocate
one worker instead of constructing one for every evaluation call. No public
module exports `NnueAccumulator`, a network representation, or a private
evaluation context. Worker creation is optional and virtual only at context
construction; the hot evaluation call dispatches through the worker interface
or the existing evaluator path.

## Verification contract

Add tests for optional worker creation, worker isolation, classical fallback,
NNUE scalar/AVX2 parity, feature extraction stability, and search-context
ownership. Run evaluation/NNUE boundaries, search/rules/process gates, and
cold/warm benchmark parity. Any change in the classical benchmark signature,
legal PV, score, node/qnode count, or fallback behavior stops the stage.
