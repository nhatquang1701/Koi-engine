# Focused search-failure fix

Commit: `11d299577dfae4dca960747a2a99b1e6ba50ddca`

## Root causes and changes

- Counter priority used `quiet_history_score()`, which combines exact quiet history with a 16K hashed continuation table. A collision could therefore promote a shallow counter. The counter tier now uses an exact `(side, previous move)` confidence value, reset when its exact counter changes and reinforced by cutoff depth. The minimum confidence is 49: a depth-8 cutoff qualifies and a single depth-6 cutoff does not. Continuation history remains in ordinary quiet ordering.
- A depth-zero `negamax` call consumed a full-search node and then consumed a quiescence node. Under a node limit, iterative deepening could exhaust the budget before an eligible null fail-high reached verification. Depth-zero calls now enter quiescence before full-search node accounting. The added regression proves those leaves are counted only as quiescence nodes.

## Verification

- Debug build: `koi_search_tests` and `search_ordering_tests` rebuilt successfully.
- `KOI_TEST_FILTER="eligible null verification"`: PASS.
- `KOI_TEST_FILTER="depth-zero node accounting"`: PASS (was RED before the production change).
- `search_ordering_tests.exe`: PASS, including shallow counter, proven depth-8 counter, and collision coverage.
- `KOI_TEST_FILTER="null safety"`: PASS for pawn-only zugzwang, low-phase, sparse phase-rich, and repetition-sensitive gates.
- `KOI_TEST_FILTER="deterministic legal search"`, `"fixed-depth tactical reference"`, and `"committed PGN tactical fixtures"`: PASS.
- `git diff --check`: clean.

The original dirty checkout was not changed. The isolated worktree used a snapshot parent for its existing roadmap state; the focused commit contains only the four files above.
