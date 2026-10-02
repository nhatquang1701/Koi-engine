# Koi Engine

Koi is a C++26 UCI chess engine for standard chess on Windows x64 and Linux
x86-64. It uses iterative-deepening alpha-beta search and a persistent
transposition table, with a classical evaluator by default and optional NNUE,
GPU NNUE, Polyglot book, and Syzygy support.

## Start here

- [Download releases](https://github.com/nhatquang1701/Koi-engine/releases)
  for Windows and Linux x86-64 archives and their checksums.
- [Read the user guide](USER_GUIDE.md) for installation, builds, UCI setup,
  GUI workflows, and optional assets.
- [Run a first UCI session](USER_GUIDE.md#uci-smoke-test) after building or
  extracting an engine binary.

Koi has no required configuration file. Begin with `Hash=512`, `Threads=1`,
and `Speed=100`. `Threads=1` provides deterministic search; higher values use
Lazy SMP and can publish different valid results. The opening book is disabled
by default (`OwnBook=false`). Enable it only with a licensed user-supplied
`book.bin` beside the executable.

## Using Koi

The [user guide](USER_GUIDE.md) covers UCI behavior, timing controls,
MultiPV and analysis, En Croissant, Lucas Chess, generic GUI setup, opening
books, NNUE, GPU NNUE, Syzygy, and release packaging.

- [Supported UCI behavior](USER_GUIDE.md#supported-uci-behavior)
- [Optional Syzygy tablebases](USER_GUIDE.md#optional-syzygy-tablebases)
- [En Croissant setup](USER_GUIDE.md#register-in-en-croissant-primary)
- [Configuration and release packaging](USER_GUIDE.md#configuration-and-release-packaging)

## Developing Koi

The [contributor and agent guide](../AGENTS.md) explains architecture contracts,
the source map, build commands, testing, artifact handling, and contribution
workflow. The guide complements these repository references:

- [Test layout](../tests/README.md)
- [Developer tools](../tools/README.md)
- [Contributing](../CONTRIBUTING.md)

## Reference

[Release notes](releases/v1.0.0.md) document the current package. The
[documentation map](README.md) links the maintained repository documentation.

For the GitHub repository entry point, see the concise
[root README](../README.md).
