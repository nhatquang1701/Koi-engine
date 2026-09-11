# Koi Strength Improvement Design

## Goal

Improve Koi's practical playing strength in measurable, isolated increments while preserving the already-stable standard-UCI engine. The intended outcome is a substantially stronger engine that can be evaluated against real human games; a 70% win rate against a particular user cannot be certified without a representative set of games against that user.

## Evidence and baseline

The current Release build has passed the existing Release CTest suite and the 64-position tactical benchmark accepts all 64 expected moves. The timed tactical baseline is stored at `artifacts/verification/strength-baseline-t1.json`. The optional 128-position suite is stored at `artifacts/verification/optional-baseline-t1.json`; it exposes the largest opportunity in quiet positional, king-safety, and endgame decisions, but many of its depth-one expected moves are only diagnostic preferences and are not proof of a unique best move.

The first strength checkpoint therefore measures three independent signals:

1. Tactical correctness: expected-move acceptance on the fixed 64-position suite.
2. Strategic selection: category-level acceptance and score stability on the optional suite, plus deeper replay of selected fixtures.
3. Practical search quality: Stockfish-19 oracle centipawn loss, tactical blunders, legal-PV rate, and node/time performance on a fixed corpus. No Elo or rating estimate is generated.

## Constraints

- Standard FIDE chess only.
- Preserve UCI options, executable names, protocol-clean stdout, and current stability guards.
- Preserve deterministic `Threads=1` fixed-depth behavior.
- Keep `Threads=2` and `Threads=4` legal, cancellable, and free of stale completions.
- Keep the fixed-depth performance budget within 15% of the recorded baseline.
- Keep hash allocation and adaptive timing behavior unchanged except for strength changes proven not to affect their safety contracts.
- Stockfish 19 is an oracle/opponent for diagnostics only; no Elo, SPRT, confidence campaign, or rating claim is added.
- Every production behavior change gets a failing regression test before its implementation.

## Design

The strength work is staged behind a repeatable benchmark loop:

```text
fixed tactical/strategic fixtures
        -> category report and regression tests
        -> one search or evaluation change
        -> CTest + benchmark + UCI stability checks
        -> retain only if tactical, legality, performance, and oracle metrics hold
```

The search boundary remains authoritative for legal move generation and completion validation. Search improvements are limited to move ordering, quiescence correctness, pruning safety, and root-result quality. Evaluation improvements remain in the classical evaluator with versioned parameters and explicit tapered behavior; no NNUE or architecture rewrite is part of this checkpoint.

The measurement layer consumes existing Koi bench profiles and Stockfish oracle records. It produces repository-local ignored reports below `artifacts/verification/` and never writes UCI output. Reports compare executable hashes, options, hardware metadata, and corpus hashes before comparing scores.

## Staged behavior changes

1. Instrument category-level baseline and per-position decision margins.
2. Add strategic regression fixtures and verify the current failure profile.
3. Audit tactical search and quiescence only where a reproducible fixture demonstrates a weakness.
4. Improve move ordering and quiet-node stability using existing TT, capture, killer, counter, continuation, and history state.
5. Improve tapered classical evaluation for development, king safety, pawn breaks, passed pawns, king activity, and endgame races, one feature family at a time.
6. Add non-rating Stockfish CPL and blunder reports for fixed-depth and short-time diagnostics.
7. Re-run the full stability and performance gates after each retained batch.

## Acceptance

The checkpoint is accepted only when the tactical suite remains 64/64, no legal/UCI/stability regression appears, `Threads=1` remains deterministic, fixed-depth slowdown stays within 15%, and at least one strategic or oracle metric improves without an unacceptable regression elsewhere. The result is reported as a measured strength improvement, not as a guarantee of a particular human win percentage.
