<h1 align="center">Koi Engine</h1>

<p align="center">
  A free, open-source UCI chess engine for standard chess.<br>
  <a href="https://nhatquang1701.github.io/Koi-engine/"><strong>Explore the documentation »</strong></a>
</p>

<p align="center">
  <a href="docs/USER_GUIDE.md#supported-uci-behavior"><img src="https://img.shields.io/badge/PROTOCOL-UCI-2e9d59?style=for-the-badge" alt="Protocol: UCI"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/LICENSE-MIT-2e9d59?style=for-the-badge" alt="License: MIT"></a>
  <img src="https://img.shields.io/badge/LANGUAGE-C%2B%2B26-2e9d59?style=for-the-badge" alt="Language: C++26">
  <img src="https://img.shields.io/badge/PLATFORMS-Windows%20x64%20%7C%20Linux%20x86_64%20%7C%20macOS%20arm64-2e9d59?style=for-the-badge" alt="Platforms: Windows x64, Linux x86-64, and macOS arm64">
</p>

<p align="center">
  <a href="https://github.com/nhatquang1701/Koi-engine/issues/new">Report an issue</a>
  ·
  <a href="https://github.com/nhatquang1701/Koi-engine/releases">Releases</a>
</p>

## About Koi

Koi is a C++26 chess engine for Windows x64, Linux x86-64, and macOS arm64
(Apple Silicon). Its default search
uses iterative-deepening alpha-beta with a persistent transposition table. An
experimental policy/value MCTS backend is also available when a compatible Koi
model is supplied. A classical evaluator works out of the box; a Koi-native
NNUE network can be enabled when available.

| Engine area | What Koi provides |
| --- | --- |
| Search | Alpha-beta by default with Lazy SMP; experimental single-thread MCTS |
| Evaluation | Classical by default, with optional Koi NNUE and GPU NNUE |
| Chess support | Standard chess over the UCI protocol |
| Optional assets | User-supplied Polyglot opening books and Syzygy tablebases |

## Download

Get the Windows and Linux x86-64 packages and their checksum sidecars from
[GitHub Releases](https://github.com/nhatquang1701/Koi-engine/releases).

- Windows: `koi-engine-v1.0.0.zip`
- Linux x86-64: `koi-engine-v1.0.0-linux-x86_64.tar.gz`

macOS arm64 (Apple Silicon) builds from source: install Homebrew LLVM (clang
19+; AppleClang 18+ also works), CMake, and Ninja, then configure with
`-DKOI_BUILD_MODULES=OFF -DKOI_STATIC_RUNTIME=OFF`. GPU NNUE is CUDA-only and
unavailable on macOS; the CPU NNUE and classical evaluator paths work normally.
See the [user guide](docs/USER_GUIDE.md#build-prerequisites) for the commands.

## Get started

1. Download and extract the archive for your system (or build from source on
   macOS).
2. Add `koi-engine.exe` (Windows) or `koi-engine` (Linux and macOS) as a UCI
   engine in En Croissant or another compatible chess GUI.
3. Start with `Hash=512`, `Threads=1`, and `Speed=100`.

Koi needs no configuration file or extra assets. `Threads=1` gives deterministic
search; multiple threads use Lazy SMP and can produce different valid
principal variations. Opening books are disabled by default (`OwnBook=false`).
The classical evaluator remains available when no NNUE network is loaded.
MCTS is opt-in through `SearchAlgorithm` and needs a separate `.kpv` model;
alpha-beta remains the default until MCTS clears matched strength tests.

## Explore

- [Documentation site](https://nhatquang1701.github.io/Koi-engine/) — installation,
  UCI options, GUI setup, development guidance, and project archive.
- [User guide](docs/USER_GUIDE.md) — build instructions, protocol behavior,
  optional assets, and tools.
- [Developer and agent guide](AGENTS.md) — architecture contracts, source map,
  testing, and contribution workflow.
- [Documentation map](docs/README.md) — maintained documentation and history.

## License

Koi Engine is released under the [MIT License](LICENSE). Release packages also
include the required licenses for vendored components.
