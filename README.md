# Koi Engine

Koi Engine is a Windows x64 and Linux x86-64 UCI chess engine for standard
chess, written in C++26. It combines iterative-deepening alpha-beta search, a
persistent transposition table, a classical evaluator, and an optional native
NNUE pipeline.

[Read the documentation site](https://nhatquang1701.github.io/Koi-engine/) for
installation, GUI setup, UCI options, development guidance, release notes, and
the project archive.

## Download

Release archives and checksum sidecars are published on the
[GitHub Releases page](https://github.com/nhatquang1701/Koi-engine/releases).
The Windows archive is `koi-engine-v1.0.0.zip`; the Linux x86-64 archive is
`koi-engine-v1.0.0-linux-x86_64.tar.gz`.

## Get started

1. Extract the release archive.
2. Add `koi-engine.exe` on Windows, or `koi-engine` on Linux, as a UCI engine
   in En Croissant or another compatible GUI.
3. Start with `Hash=512`, `Threads=1`, and `Speed=100`. `Threads=1` is the
   deterministic configuration; higher values use Lazy SMP and may produce
   different valid principal variations between runs.

Koi has no required configuration file. The classical evaluator is the default.
NNUE networks, GPU NNUE inference, Polyglot opening books, and Syzygy
tablebases are optional. Opening books are disabled by default
(`OwnBook=false`); enable one only after placing a licensed `book.bin` beside
the executable and setting `OwnBook=true`.

## Documentation

- [User guide](docs/USER_GUIDE.md) — build instructions, UCI behavior, GUI
  setup, optional assets, and developer tools.
- [Developer and agent guide](AGENTS.md) — architecture contracts, source map,
  tests, artifact policy, and contribution workflow.
- [Documentation map](docs/README.md) — repository documentation and archive
  entry points.

## License

Koi Engine is released under the [MIT License](LICENSE). Release packages also
retain the licenses for the vendored chess-library and optional Fathom adapter.
