# Koi Engine contributor and agent guide

This file gives contributors and coding agents the repository context needed to
make safe, reviewable changes. Use the documentation site for navigable user
and developer material; use this guide for the source contracts and workflow
rules that changes must preserve.

## Project shape

Koi is a C++26 UCI engine for standard chess on Windows x64, Linux x86-64, and
macOS arm64 (Apple Silicon).
The root [README](README.md) is a short project entry point. The detailed user
reference is [docs/USER_GUIDE.md](docs/USER_GUIDE.md), and
[docs/README.md](docs/README.md) maps the documentation, design records, plans,
and verification archive.

The engine has a classical evaluator by default. A Koi-native NNUE evaluator
can be loaded through `EvalFile`; GPU NNUE is an optional runtime capability.
Polyglot books and Syzygy tablebases are optional user-provided assets. Neither
the engine nor ordinary builds require those files.

## Architecture contracts

- `src/koi/position.*` is the production authority for legality, position keys,
  rule state, reversible history, and move generation. Public Koi rules types
  must not expose `chess.hpp`.
- `GameState` is the compatibility facade for existing evaluator, search, and
  UCI APIs. Its private compatibility mirror supports Polyglot, legacy
  adapters, and explicit differential diagnostics; it must stay synchronized
  transactionally with native state.
- `FeatureState` owns cached `PositionFeatures` derived from native `Position`.
  Keep feature updates consistent with make and unmake operations.
- The UCI controller owns the current position and search-worker lifecycle. It
  is the only layer that writes UCI protocol output. Diagnostics must not leak
  to normal UCI stdout or stderr.
- `SearchService` and `SearchHandle` are the public search lifecycle boundary.
  `detail::SearchSession` owns a request snapshot, cancellation, worker
  lifetime, and the exactly-once completion claim. Search-local recursive state
  belongs to `detail::SearchContext` and its fixed-capacity stack.
- `SearchRunner` owns backend dispatch and the native `GameState` rules path.
- `TranspositionTable` owns physical storage, locking, generations, clearing,
  resizing, and mate-score normalization. Search accesses it through the
  private `detail::SearchTableAccess` seam.
- `Threads=1` is deterministic. `Threads>1` uses Lazy SMP over a shared
  transposition table, so results are intentionally nondeterministic; validate
  legality, completion, and coherent lines instead of byte-for-byte equality.
- The classical evaluator remains the fallback when an NNUE file is absent or
  rejected. A running search keeps the evaluator snapshot it started with.

The optional C++ module interface consists of the aggregate `koi` module and
the `types`, `position`, `eval`, `tablebase`, `search`, and `runtime`
partitions. Its public value contracts should remain stable; implementation
headers are private and do not establish an ABI commitment.

## Source map

| Path | Responsibility |
| --- | --- |
| `src/koi/` | Engine rules, evaluation, search, UCI, time management, and runtime code. |
| `src/koi/detail/` | Private search sessions, contexts, ordering tables, search policy, root coordination, feature state, and compatibility mechanisms. |
| `src/koi/gpu/` | Optional CUDA driver and GPU NNUE implementation. |
| `src/koi/modules/` | C++26 module interfaces. |
| `tests/unit/` | Rules, evaluation, search, runtime, and architecture tests. |
| `tests/integration/` | UCI process, packaging, match, tooling, and differential tests. |
| `tests/python/` | Measurement, NNUE, data, and tool boundary tests. |
| `tools/` | Build, test, release, measurement, NNUE, and stability tooling. |
| `docs/` | User documentation, releases, documentation maps, and historical records. |
| `artifacts/` | Repository-local generated verification and match evidence. |

## Build and test

On Windows, use an x64 Visual Studio developer shell with a C++26-capable
MSVC, CMake 3.31 or newer, and Ninja:

```powershell
cmake -S . -B build\release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=cl
cmake --build build\release --config Release
ctest --test-dir build\release -C Release -j 8 --output-on-failure
```

On Linux x86-64, use GCC 14+ or Clang 18+, CMake 3.31+, and Ninja:

```bash
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=gcc-14 -DCMAKE_CXX_COMPILER=g++-14
cmake --build build/release --config Release
ctest --test-dir build/release -C Release -j 8 --output-on-failure
```

On macOS arm64 (Apple Silicon), use Homebrew LLVM clang 19+ (AppleClang 18+
also works), CMake 3.31+, and Ninja:

```bash
brew install llvm ninja
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER="$(brew --prefix llvm)/bin/clang" \
  -DCMAKE_CXX_COMPILER="$(brew --prefix llvm)/bin/clang++" \
  -DKOI_BUILD_MODULES=OFF -DKOI_STATIC_RUNTIME=OFF
cmake --build build/release --config Release
ctest --test-dir build/release -C Release -j 8 --output-on-failure
```

The documented macOS arm64 build disables C++26 named modules and the Linux
static-runtime policy. GPU NNUE is CUDA-only and unavailable on macOS; the CPU
NNUE and classical evaluator paths are unaffected.

`KOI_BUILD_MODULES=OFF` disables named modules where the compiler cannot build
them (the documented macOS arm64 configuration). `KOI_BUILD_SHADOW_DIFF=ON`
enables the explicit differential target.
`tools/test/run_tests.ps1` wraps the standard configure, build, CTest, JUnit,
and `LastTest.log` flow. Read [tests/README.md](tests/README.md) before
changing tests or test registration.

Run the smallest relevant test first, then the affected CTest labels or suite.
Use the checked-in release verification harness for release claims. Do not
convert observed timing, Elo, CPL, or match results into a claim without fresh,
comparable evidence that records executable, options, time control, and input
provenance.

## UCI and optional assets

Keep the protocol responsive: `isready` must respond while search runs; every
completed or stopped `go` search emits exactly one legal `bestmove`; `quit` and
EOF must cancel and join without late output. Changes to options that replace
runtime resources must stop and join the active search before applying.

The stable defaults include `Hash=512`, `Threads=1`, `Speed=100`, and
`OwnBook=false`. A user may opt into a licensed Polyglot book with
`OwnBook=true`, `BookFile=book.bin`, and a file beside the executable. Do not
commit, embed, or redistribute an unlicensed book.

`EvalFile` is optional and an empty value retains the boot-time evaluator.
Missing or malformed NNUE files must leave the current evaluator usable.
Syzygy is optional: an absent or unusable `SyzygyPath` falls back to ordinary
search, and no tablebase data belongs in the repository or release archive.
GPU NNUE also remains optional; retain the CPU fallback and its availability
checks.

## Artifacts, documentation, and releases

Generated build trees belong under `build/`. Durable local evidence belongs
under `artifacts/`, including benchmark profiles, match JSON and PGN, debug
logs, and release-verification output. Do not commit generated binaries,
networks, books, tablebases, build output, or local reports unless a task
explicitly changes the repository artifact policy.

Keep user-facing instructions accurate when changing UCI options, build
requirements, packaging, or tooling. Preserve relative Markdown links when
moving documentation. The Pages site stages tracked project Markdown; do not
publish untracked summaries, local evidence, or third-party documentation.

Release packages retain the root README, MIT license, required third-party
licenses, `package.json` manifest, source provenance, and a `.sha256` sidecar.
Use the release tooling for package layout and integrity checks. A release title
uses a readable product name and version, while its tag is the version alone.

## Contribution workflow

1. Inspect the applicable architecture, test, and documentation contracts.
2. Keep a change focused and preserve public UCI behavior unless the task calls
   for a protocol change.
3. Add or update meaningful regression coverage for behavior changes.
4. Run the focused checks and the relevant broader suite before reporting the
   change complete.
5. Keep generated output and local assets outside tracked source changes.

Historical design specifications, implementation plans, and verification
records describe prior decisions and evidence. They are useful context, but a
new change must be judged against the active code, tests, and current task.
