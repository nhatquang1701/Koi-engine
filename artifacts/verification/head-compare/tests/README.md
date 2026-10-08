# Test layout

Tests are grouped by responsibility while preserving their existing CTest
names and executable target names.

- `unit/` contains core, rules, search, evaluation, and runtime tests.
- `integration/` contains UCI, Cutechess, packaging, tool, and differential
  process tests.
- `python/` contains measurement and NNUE/tuning boundary tests.
- `data/` contains stable opening, position, game, and metadata fixtures.

Temporary test output may use the operating-system temporary directory and must
be cleaned up by the test. Durable reports belong under the repository's
`artifacts/` directory.
