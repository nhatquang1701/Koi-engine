# Koi Engine

Koi Engine is a Windows x64 UCI chess engine for standard chess. It is
written in C++26 and is documented and process-tested against En Croissant as
the primary GUI workflow. It uses deterministic iterative-deepening alpha-beta search
with a classical evaluator, an opt-in Koi-native NNUE pipeline (training
tooling, a versioned container, and runtime `EvalFile` loading), and a persistent
transposition table. Search runs on a cancellable outer worker; `Threads = 1`
runs the deterministic serial search, while `Threads > 1` starts Lazy SMP
helper threads that search the same root against the shared transposition table
while the main thread publishes the result. Lazy SMP is intentionally
nondeterministic: the deterministic guarantee is scoped to `Threads = 1`, and
threaded runs are validated for legality and coverage instead of byte
equality. The UCI command loop remains responsive in both configurations.

## Architecture

The engine is deliberately layered so the chess rules implementation remains a
private dependency. Public Koi rules types (`Move`, `GameState`, `Position`)
never expose `chess.hpp`. The native `Position` core is the sole production
authority for legality, keys, rule state, reversible history, and move
generation. `GameState` remains the compatibility facade used by the existing
evaluator/search/UCI APIs and coordinates two private owners: `CompatibilityMirror`
holds the vendored chess-library board and reversible shadow history for
Polyglot/book compatibility, completion validation, explicit differential
diagnostics, and other legacy adapters; `FeatureState` owns cached
`PositionFeatures` derived from the native `Position`. The mirror is not part
of the public API and is covered by the opt-in `KOI_BUILD_SHADOW_DIFF`
differential target. Search moves still update the mirror transactionally, while
interior search skips the expensive mirror comparison; explicit validation and
diagnostic boundaries retain it. `ClassicalEvaluator`, time management, the
transposition table, and the `SearchService` build on Koi-owned types. The UCI
controller owns the current position and worker lifecycle, and is the only layer
that writes protocol output.

The C++26 module boundary is represented by the aggregate `koi` module and the
partitions `koi:types`, `koi:position`, `koi:eval`, `koi:tablebase`,
`koi:search`, and `koi:runtime`. These partitions export stable value contracts;
the implementation headers remain internal so future search and evaluation work
does not create an ABI promise.

Search ordering is also an internal search concern: TT best moves are tried
first, followed by staged good captures/promotions, history-ranked good quiets
(including checks, killers, and proven counters), deferred losing captures, and
bad quiets. Stable UCI-coordinate tie-breaking keeps repeated searches
deterministic. The public diagnostic ordering API retains its documented
capture/killer/history view; the richer staged picker is recursive-search
state. There is no public rule API for these policies; `Threads` and `Speed`
are UCI controls over the search runtime.

The search lifecycle is decomposed behind the public `SearchService` and
`SearchHandle` contracts. `detail::SearchSession` owns one request's immutable
root/limits/options snapshot, cancellation, worker lifetime, identity, and
exactly-once completion claim. Each worker owns a fixed-capacity
`detail::SearchStack` through `detail::SearchContext`; the context keeps
recursive state and search-local statistics out of `GameState`. Root-line
records and deterministic score/index ranking belong to
`detail::RootCoordinator`. These types are private implementation headers in
`src/koi/detail`; root-worker scheduling remains behind the existing service,
while ordering, policy, evaluation, and runtime-resource ownership are
separately testable internal seams.

Adaptive ordering state is separately owned by
`detail::SearchOrderingTables`, which stores killers, quiet history, counter
moves/confidence, and continuation history for one worker. The
`detail::SearchPolicy` seam contains the scalar null-move, check-extension,
LMR, reverse-futility, quiet-futility, and quiescence-capture decisions;
`SearchContext` still applies the decisions and owns recursion and statistics.
This keeps future
heuristic experiments local while preserving the current formulas and avoids
making search policy depend on the physical layout of ordering tables.

Evaluation execution has a corresponding private boundary. The optional
`EvaluatorWorker` capability lets stateful evaluators provide one worker per
search context; `NnueEvaluator` uses it to keep an `NnueWorker` accumulator
local while immutable network data remains shared. `detail::EvaluationContext`
selects that worker path or the existing evaluator-plus-mutex fallback, so
`SearchContext` no longer owns evaluator synchronization or a particular NNUE
representation. The classical evaluator remains the default and malformed or
absent NNUE input still falls back as before. `EvaluatorSelection` and
`make_evaluator` are the selection seam used by the engine entry points: at boot
`main.cpp` resolves `KOI_NNUE_PATH` or `koi.nnue` beside the executable, and the
advertised `EvalFile` option swaps the evaluator at runtime. The classical
evaluator remains the default and the fallback.

Search runtime resources have the same ownership boundary. The private
`detail::SearchTableAccess` view is the only TT boundary used by search code:
both the orchestrator (`SearchRunner`) and the recursive contexts make their
probes and the once-per-search generation advance through it, while
`TranspositionTable` remains the owner of striped physical
storage, locking, generations, clear, resize, and mate-score normalization.
The private `detail::SearchBudget` snapshots the node limit and keeps serial
local-count validation separate from bounded shared-atomic reservation for
root-parallel contexts. `TimeManager` remains the owner of clock/deadline and
iteration pacing, while `SearchSession` owns cancellation; this stage adds no
new scheduler, shared history, or Lazy-SMP behavior. These seams let future
parallel search evolve shared-state policy without moving it through recursive
search or changing the public `SearchService` contract.

Stages 1 through 5 cover native state ownership, search lifecycle and stack,
ordering/policy, evaluation/NNUE worker state, and TT/budget runtime resources.
The final Stage 6 review records the module/tooling alignment, Release and
Debug evidence, Stockfish-informed self-review, future-change test, and the
known short-clock/Debug limitations:
[final architecture review](docs/superpowers/verification/2026-09-12-koi-architecture-stage6-final-review.md).
The architecture is accepted as the baseline for future strength work. Lazy
SMP has since landed for `Threads > 1` (see the search section above); shared
histories across helper threads remain deliberately future work.

## Build prerequisites

- A C++26-capable x64 MSVC toolchain (the current CMake configuration selects
  Visual Studio's `/std:c++latest` compiler mode). Run CMake from an x64
  Native Tools Command Prompt or x64 Developer PowerShell for Visual Studio,
  so `cl.exe` is selected; the currently available MinGW/GCC/Clang toolchains
  do not meet the C++26 requirement.
- CMake 3.31 or newer.
- Ninja.

From an x64 Visual Studio developer shell in the repository root:

```powershell
cmake -S . -B build\release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=cl
cmake --build build\release --config Release
ctest --test-dir build\release -C Release -j 8 --output-on-failure
```

For a Debug build, substitute `build\debug` and `Debug` in those commands. The
resulting engine executable is `build\release\koi-engine.exe`.

For an independently reproducible release gate, run the checked-in harness from
an x64 Visual Studio developer shell. It configures and builds fresh canonical
Debug and Release trees, runs all CTest/process tests, checks tactical
Threads 1/2/4 when the host supports them (with an explicit maximum-thread
fallback), and writes benchmark, UCI, replay, and En Croissant-style artifacts
under `artifacts/verification`:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\build\release_verify.ps1 `
  -CMakePath "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" `
  -OutputDirectory .\artifacts\verification\release-verify
```

The harness records the exact Debug/Release configure, build, CTest, benchmark,
transcript, replay, and match command outputs under the supplied artifact
directory. It makes no
Stockfish/CPL or En Croissant GUI availability assumption.

## Developer tools

From the configured build directory:

```powershell
.\build\release\koi-perft.exe 4
.\build\release\koi-bench.exe
.\build\release\koi-bench.exe --threads 4 --speed 100 --timed
.\build\release\koi-bench.exe --optional --profile-json .\artifacts\verification\optional-strength.json
.\build\release\koi-bench.exe --nodes 20000 --timed --warm-hash --warmup 1 --repeat 5 --profile-json .\artifacts\verification\steady-state.json
.\build\release\koi-replay.exe startpos moves e2e4 e7e5 g1f3
```

`koi-perft` counts legal nodes from the standard starting position at the given
non-negative depth. `koi-bench` runs the 64-position fixed-depth tactical hard
gate and writes only its deterministic benchmark report to stdout by default.
`--optional` instead selects the 128-position optional strength corpus and labels
that suite in both its text and JSON-profile output. `--threads N` and `--speed
1..100` are recorded in every report and use the same deterministic search
configuration as the corresponding engine controls. Every text report includes
`hash cold` or `hash warm`; `--warm-hash` reuses one search service across rows
and is useful for comparing warmed-table behavior. The default is cold.
`--nodes N` replaces the per-position fixed depth with a node limit, matching the
node-limited match regime; `--warmup K` runs K unmeasured suite passes first and
`--repeat K` keeps K measured samples per position, reporting the median-time
sample as the primary result and retaining every sample in the profile's `runs`
array. Combine all three for steady-state throughput comparisons. `--timed` is
opt-in and adds wall-clock `elapsed_ms` and measured NPS to text and JSON; it is
intentionally absent from the default CI-shaped output. Untimed JSON profiles use
the stable `Koi Engine 1.1.0` build identity, set `timed` to `false`, and record
`nps` as unmeasured (`0`). Each profile carries `hash_state` (`cold` or `warm`) at
the top level and on every position. It is a separate process and never writes to
the UCI engine's stdout.

`koi-replay` is a separate rules-boundary tool for replaying coordinate moves without
exposing the vendored chess library. Give it `startpos` or `fen <six-field FEN>`, then
an optional `moves` list. Its stable stdout reports `legal`, `result`, `termination`,
and the final six-field `fen`; an illegal move leaves the reported position at the
last legal state. It is useful for reproducing a match-ply or validating a UCI log.

The Stockfish position oracle is a measurement-only Python tool and does not add a
dependency to the C++ engine. Install its pinned dependency and run its test directly
from the repository root:

```powershell
python -m pip install -r .\tools\measurement\requirements-elo-oracle.txt
python -m unittest .\tests\python\measurement\elo_oracle_test.py -v
```

See [`tools/README.md`](tools/README.md) for the extract-only schema, the named
book-audit and match entrypoints, and the external-results workflow.

When Python 3 and `python-chess` are available at CMake configure time, the same test
is registered as `elo_oracle_python` in CTest; otherwise the C++ test suite is unchanged
and CMake reports that the optional test was skipped.

To analyze a supplied standard-SAN PGN with Stockfish as the position oracle, keep the
JSON output under this checkout's artifacts directory and provide the exact
executable/version and PGN provenance alongside the report:

```powershell
python .\tools\measurement\elo_oracle.py `
  --pgn .\tests\data\games\2026-09-08-koi-vs-stockfish-19.pgn `
  --koi .\build\release\koi-engine.exe `
  --stockfish .\third_party\stockfish-19\stockfish-windows-x86-64-universal\stockfish\stockfish-windows-x86-64-universal.exe `
  --output .\artifacts\verification\elo-oracle.json `
  --movetime-ms 250 --threads 4
```

Use `--extract-only` to test PGN/FEN extraction without either engine. Run the licensed
opening book as a separate audit so its `book_used`, `book_move`, and Stockfish CPL are
kept outside the normal search metrics:

```powershell
python .\tools\measurement\elo_oracle.py `
  --pgn .\tests\data\games\2026-09-08-koi-vs-stockfish-19.pgn `
  --koi .\build\release\koi-engine.exe `
  --stockfish .\third_party\stockfish-19\stockfish-windows-x86-64-universal\stockfish\stockfish-windows-x86-64-universal.exe `
  --book C:\LicensedBooks\book.bin --book-audit `
  --output .\artifacts\verification\book-audit.json `
  --movetime-ms 250 --threads 4
```

The PGN, Stockfish executable/version, and licensed `book.bin` are external inputs;
none are assumed to exist in this repository. Do not report an Elo or CPL improvement
until both a comparable baseline and an after-change report have been generated.

The offline Koi-native NNUE boundary is dependency-free in synthetic mode and has
separate entry points for future training/export automation. Both wrappers use the
same versioned implementation and emit the `piece-square-king-pawn-v2` container;
they do not add a Python runtime dependency to the engine:

```powershell
python .\tools\measurement\train_nnue.py --help
python .\tools\measurement\export_nnue.py --help
```

`--backend synthetic` is deterministic and suitable for boundary tests. The optional
PyTorch backend is offline-only, records corpus and network provenance, and must pass
the Koi loader and strength gates before any network is considered for runtime use.

A complete Stockfish-labeled training pipeline lives next to the boundary. Positions
come from Stockfish self-play (with tactical noise games), labels are fixed-depth
centipawn scores (the example below asks for depth 10; the generator default is 9).
The PyTorch trainer (`train_nnue_koi.py`) defaults to the version 5 architecture:
group A `halfka-king-bucket-v1` (9216 inputs) plus the symmetric group B
`threat-pairs-v1` attack relations (27648 inputs) for 36864 inputs total, both
perspectives feeding full-width cross pair products (`p[j] = own[j] * opp[j]`) and a
32-unit CReLU hidden layer into the eight piece-count output buckets. Pass
`--arch v4` for the earlier version 4 `halfka-king-bucket-v1` network with CReLU
pair products; the version 3 trainer (`train_nnue_sf.py`) remains available, and
version 2, 3, and 4 containers still load:

```powershell
python .\tools\measurement\gen_training_data.py all --games 30000 --workers 3 --label-depth 10
python .\tools\measurement\koi_dataset.py encode --input .\artifacts\training\labels.txt `
  --output .\artifacts\training\koi-dataset.bin
python .\tools\measurement\train_nnue_koi.py --dataset .\artifacts\training\koi-dataset.bin `
  --epochs 20 --float-out .\artifacts\training\koi-v5.pt `
  --net-out .\artifacts\training\koi-v5.nnue --meta-out .\artifacts\training\koi-v5.metadata.json
.\build\release\koi-bench.exe --nnue .\artifacts\training\koi-v5.nnue
```

Load a trained network with the UCI `EvalFile` option, or place `koi.nnue` beside
the executable (or point the `KOI_NNUE_PATH` environment variable at it) to select it
at startup. A missing or rejected file falls back to the classical evaluator and
writes one explanatory line to stderr. Trained networks are local artifacts and are
not committed. On the local single-thread probe the version 4 `halfka-king-bucket-v1`
network runs at about 76k nodes/s against about 144k for the classical evaluator, it
matches 61 of the 64 positions in the tactical gate (the classical evaluator stays at
64/64), and it lost the color-balanced equal-node A/B against the classical evaluator
(0 wins, 10 draws, 10 losses); the version 4 versus version 3 net match drew all 20
games. The classical evaluator therefore remains the default and NNUE stays opt-in.
The version 5 architecture ships with loader, inference, incremental, dataset, and
trainer support only: no version 5 network has been trained or strength-validated, so
every probe number above still describes version 4 networks.

### Training a network with the NNUE Studio

`Koi NNUE Studio.cmd` in the repository root opens a small tkinter GUI (Data,
Train, Validate and install, and Runs tabs) so a training run does not require
remembering any command lines. The same pipeline is available headlessly:

```powershell
pwsh -NoProfile -File .\tools\nnue\train.ps1 -Preset quick              # wait for it
pwsh -NoProfile -File .\tools\nnue\train.ps1 -Preset thorough -Detach   # background
python .\tools\nnue\koi_nnue_studio.py --list-backends
python .\tools\nnue\koi_nnue_studio.py --selftest --rows 2000 --epochs 1
pwsh -NoProfile -File .\tools\nnue\ab_match.ps1 -NnueNet .\artifacts\training\koi.nnue -Games 20
pwsh -NoProfile -File .\tools\nnue\net_match.ps1 -NnueNet .\artifacts\training\koi-v4-1024.nnue `
  -OpponentNet .\artifacts\training\koi-sf-v1.nnue -Games 20
```

Every run keeps its configuration, command line, log, progress history and
artifacts under `artifacts/training/runs/<stamp>-<kind>-<backend>/`, so runs are
comparable and resumable. The default `koi` backend trains the version 5
`halfka-king-bucket-v1` plus `threat-pairs-v1` network (36864 inputs,
dual-perspective cross pairs, 32-unit L1) on CPU PyTorch; pass `--arch v4` for the
earlier version 4 network, and `--backend torch` still drives the legacy
`train_nnue_sf.py` version 3 trainer. `--backend bullet`
drives the pinned Rust/CUDA trainer through `tools/nnue/run_bullet.py` when cargo
and a CUDA 12.x toolkit are present; its default architecture is version 5 with the
same shared feature transformer over both perspectives, and `--arch v4` keeps the
earlier within-perspective pair-product network. Bullet run length is controlled by
`bullet_superbatches` rather than the preset `epochs` value. After a run completes the
studio can validate it with the 64-position `koi-bench --nnue` gate and with a
node-limited A/B match against the classical evaluator
(`tools/nnue/ab_match.ps1`, schema `koi-nnue-studio-ab-match-v1`) or against an
earlier network (`tools/nnue/net_match.ps1`, schema `koi-nnue-net-match-v1`), then
offer to
install the network as `koi.nnue` beside the engine, backing up any previous
file. Validation output is a local report, not an Elo claim or a CI threshold.

### Optional GPU NNUE inference

On a machine with an NVIDIA GPU and a CUDA 12.x toolkit, the engine can evaluate
the version 5 network on the GPU. Set `KOI_GPU_NNUE=1` in the environment and
run with `Threads` greater than one; Threads=1 keeps the deterministic CPU path.
The feature is opt-in and never changes the advertised UCI surface or
`EvalFile` semantics: any driver, device, or kernel failure silently falls back
to the CPU network. The build compiles `src/koi/gpu/koi_nnue_v5.cu` to PTX with
`nvcc` (compute capability 6.1, the local GTX 1060) and embeds it; at runtime
only `nvcuda.dll` is loaded dynamically, so builds without `nvcc` stay CPU-only.
The GPU result is bit-exact with the CPU scalar evaluation, but this first pass
does not promise a speedup and makes no strength or Elo claims.

For a reproducible local match against Stockfish or another UCI engine, use the
optional PowerShell harness:

```powershell
.\tools\stability\uci_match.ps1 `
  -KoiPath .\build\release\koi-engine.exe `
  -OpponentPath .\third_party\stockfish-19\stockfish-windows-x86-64-universal\stockfish\stockfish-windows-x86-64-universal.exe `
  -Depth 6 -Threads 4 -Speed 100 -Hash 512 `
  -OutputDirectory .\artifacts\matches\match-results
```

The harness writes a `koi-uci-match-v2` JSON report plus matching `.pgn`. It records
both engines' UCI handshakes and options; every ply's actual root FEN, exact
`position` and `go` commands, engine label, returned move, replay legality, timing,
parsed final info/PV, all info lines, and raw `bestmove`; and each game's adjudicated
result, winner, termination, and process status. Moves are replay-validated before
they are appended, so illegal moves and non-terminal `0000` replies end only that game
without contaminating later positions. PGN Result headers match the adjudicated result
(`*` for max-ply games) and retain the `MoveFormat` header. Use `-MovetimeMs` or
`-Nodes` instead of `-Depth` for those limits. To run a named FEN suite, pass
`-FenFile` containing one entry per line in the form `name | six-field FEN` (blank
lines and `#` comments are ignored). The replay executable is expected beside
`koi-engine.exe` (or can be supplied as `-ReplayPath`).

For color-balanced paired Elo measurements, supply the checked-in eight-opening
suite (or another file in the same `name | uci move uci move` format) and a chess
clock. `-Games` is the number of games run for each selected opening, so use 20 as
Koi White and 20 as Koi Black for 40 games per opening and condition. Run both
book-disabled and licensed-book conditions at both clocks:

```powershell
.\tools\stability\uci_match.ps1 `
  -KoiPath .\build\release\koi-engine.exe `
  -OpponentPath .\third_party\stockfish-19\stockfish-windows-x86-64-universal\stockfish\stockfish-windows-x86-64-universal.exe `
  -OpeningFile .\tests\data\openings\openings-basic.txt `
  -TimeControl 1+0 -Games 20 -KoiColor white `
  -KoiRandomSeed 1 -KoiOwnBook false `
  -OutputDirectory .\artifacts\matches\no-book-1p0-white
.\tools\stability\uci_match.ps1 -KoiPath .\build\release\koi-engine.exe -OpponentPath .\third_party\stockfish-19\stockfish-windows-x86-64-universal\stockfish\stockfish-windows-x86-64-universal.exe -OpeningFile .\tests\data\openings\openings-basic.txt -TimeControl 1+0 -Games 20 -KoiColor black -KoiRandomSeed 1 -KoiOwnBook false -OutputDirectory .\artifacts\matches\no-book-1p0-black
.\tools\stability\uci_match.ps1 -KoiPath .\build\release\koi-engine.exe -OpponentPath .\third_party\stockfish-19\stockfish-windows-x86-64-universal\stockfish\stockfish-windows-x86-64-universal.exe -OpeningFile .\tests\data\openings\openings-basic.txt -TimeControl 1+0 -Games 20 -KoiColor white -KoiRandomSeed 1 -KoiOwnBook true -KoiBookFile C:\LicensedBooks\book.bin -KoiBookDepth 16 -OutputDirectory .\artifacts\matches\book-1p0-white
.\tools\stability\uci_match.ps1 -KoiPath .\build\release\koi-engine.exe -OpponentPath .\third_party\stockfish-19\stockfish-windows-x86-64-universal\stockfish\stockfish-windows-x86-64-universal.exe -OpeningFile .\tests\data\openings\openings-basic.txt -TimeControl 1+0 -Games 20 -KoiColor black -KoiRandomSeed 1 -KoiOwnBook true -KoiBookFile C:\LicensedBooks\book.bin -KoiBookDepth 16 -OutputDirectory .\artifacts\matches\book-1p0-black
.\tools\stability\uci_match.ps1 -KoiPath .\build\release\koi-engine.exe -OpponentPath .\third_party\stockfish-19\stockfish-windows-x86-64-universal\stockfish\stockfish-windows-x86-64-universal.exe -OpeningFile .\tests\data\openings\openings-basic.txt -TimeControl 5+3 -Games 20 -KoiColor white -KoiRandomSeed 1 -KoiOwnBook false -OutputDirectory .\artifacts\matches\no-book-5p3-white
.\tools\stability\uci_match.ps1 -KoiPath .\build\release\koi-engine.exe -OpponentPath .\third_party\stockfish-19\stockfish-windows-x86-64-universal\stockfish\stockfish-windows-x86-64-universal.exe -OpeningFile .\tests\data\openings\openings-basic.txt -TimeControl 5+3 -Games 20 -KoiColor black -KoiRandomSeed 1 -KoiOwnBook false -OutputDirectory .\artifacts\matches\no-book-5p3-black
.\tools\stability\uci_match.ps1 -KoiPath .\build\release\koi-engine.exe -OpponentPath .\third_party\stockfish-19\stockfish-windows-x86-64-universal\stockfish\stockfish-windows-x86-64-universal.exe -OpeningFile .\tests\data\openings\openings-basic.txt -TimeControl 5+3 -Games 20 -KoiColor white -KoiRandomSeed 1 -KoiOwnBook true -KoiBookFile C:\LicensedBooks\book.bin -KoiBookDepth 16 -OutputDirectory .\artifacts\matches\book-5p3-white
.\tools\stability\uci_match.ps1 -KoiPath .\build\release\koi-engine.exe -OpponentPath .\third_party\stockfish-19\stockfish-windows-x86-64-universal\stockfish\stockfish-windows-x86-64-universal.exe -OpeningFile .\tests\data\openings\openings-basic.txt -TimeControl 5+3 -Games 20 -KoiColor black -KoiRandomSeed 1 -KoiOwnBook true -KoiBookFile C:\LicensedBooks\book.bin -KoiBookDepth 16 -OutputDirectory .\artifacts\matches\book-5p3-black
```

`-TimeControl` accepts only `<minutes>+<increment>` (for example `1+0` or `5+3`)
and sends `wtime`, `btime`, `winc`, and `binc` on every `go`; elapsed time is deducted
from the moving side before its increment is added. Opening sequences are replayed and
rejected before either engine starts a game. The JSON configuration records the clock
and Koi `RandomSeed`, `OwnBook`, `BookFile`, and `BookDepth` values. Per-ply
`book_used` and `book_move` record an `info string book move <uci> depth <ply>` marker
separately from normal search PV data. The harness sends Koi's book options without
requiring a book reader or any Jack dependency; Koi versions predating book support
ignore those UCI options safely.

`tools/stability/sprt_compare.ps1` wraps `uci_match.ps1` for candidate-versus-baseline
acceptance testing: it launches the white and black SPRT runs concurrently, reads both
reports, and prints per-color and combined results. The combined decision accepts when
either color accepts, rejects when either color rejects, and otherwise compares the sum
of the two independent LLRs against the alpha = beta = 0.05 bounds. It exits 0 on
accept, 1 on reject, and 2 when more games are needed.

```powershell
.\tools\stability\sprt_compare.ps1 `
  -CandidatePath .\build\release\koi-engine.exe `
  -BaselinePath .\artifacts\verification\baseline\koi-engine-baseline.exe `
  -Nodes 20000 -Games 2 -MinGames 40 -MaxGames 64 -Elo1 5 `
  -OutputDirectory .\artifacts\matches\sprt-candidate
```

## Rough local Elo measurement

`tools/measurement/elo_estimate.py` is a standard-library-only, measurement-only harness. Its
primary configuration is no-book, 1+0, Koi `Hash=512`, `Threads=4`, and
`Speed=100`, with 32 named opening lines played once with Koi White and once with
Koi Black at each Stockfish anchor. The initial schedule is two 64-game anchor
batches (128 games); adaptive batches may extend the run to 192, 256, or 320 games.
The estimator uses 2,000 paired-opening bootstrap samples and rejects incomplete,
illegal, timed-out, or malformed match evidence.

Create an external `koi-elo-anchor-manifest-v1` JSON manifest. Its `stockfish`
object must contain the existing executable `path`, at least two unique
`elos` in the Stockfish `UCI_Elo` range 1320..3190, and a `rating_source` such as
`Stockfish 19 UCI_LimitStrength`. Each optional `lower_anchors` entry must contain
an `id`, existing executable `path`, positive `rating`, and `rating_source`; a
lower anchor is required when `--prior-elo` is below the lowest Stockfish anchor.
The manifest must bracket `--prior-elo`, and `--stockfish` must match its
`stockfish.path`.

Run the primary no-book dry-run from the repository root with every executable,
manifest, corpus, and report path resolved explicitly:

```powershell
python .\tools\measurement\elo_estimate.py `
  --koi .\build\release\koi-engine.exe `
  --replay .\build\release\koi-replay.exe `
  --stockfish .\third_party\stockfish-19\stockfish-windows-x86-64-universal\stockfish\stockfish-windows-x86-64-universal.exe `
  --anchors .\artifacts\manifests\elo-anchors.json `
  --openings .\tests\data\openings\openings-curated-32.txt `
  --time-control 1+0 `
  --threads 4 --hash 512 --speed 100 `
  --min-games 128 --max-games 320 `
  --prior-elo 1600 --mode no-book `
  --output .\artifacts\matches\rough-elo-no-book.json `
  --dry-run
```

`--dry-run` validates the executable paths, anchor manifest, 32-opening corpus,
required fixed options, and output location, writes the complete deterministic
schedule to the repository-local JSON report, and launches no engines. Remove only
`--dry-run` for a real run after supplying valid local Koi, replay, Stockfish,
anchor, corpus, and artifact paths. The real run writes raw `koi-uci-match-v2`
JSON and matching PGN artifacts below a sibling `<report-stem>-artifacts` directory
under `artifacts/matches`.

Book mode is a separate measurement and must not be combined with the primary
no-book result. Use a separate output path and licensed external book:

```powershell
python .\tools\measurement\elo_estimate.py `
  --koi .\build\release\koi-engine.exe `
  --replay .\build\release\koi-replay.exe `
  --stockfish .\third_party\stockfish-19\stockfish-windows-x86-64-universal\stockfish\stockfish-windows-x86-64-universal.exe `
  --anchors .\artifacts\manifests\elo-anchors.json `
  --openings .\tests\data\openings\openings-curated-32.txt `
  --time-control 1+0 `
  --threads 4 --hash 512 --speed 100 `
  --min-games 128 --max-games 320 `
  --prior-elo 1600 --mode book --book C:\LicensedBooks\book.bin `
  --output .\artifacts\matches\rough-elo-book.json
```

CTest registers the standard-library-only estimator, match-option, and opening
corpus tests when Python 3 is available; the corpus test receives the built
`koi-replay` path through `KOI_REPLAY_PATH`. These tests validate tooling and
corpus integration only. Real engine matches are intentionally unsuitable for a
fixed CI schedule because they depend on supplied external executables, hardware,
and time control; CTest is not an Elo threshold. Label any resulting figure as
“local Stockfish-equivalent Elo at recorded hardware/options/time control”; it is
not a universal Elo claim.

## Strength regression suite

The deterministic `Threads=1`, `Speed=100` reference path includes a fixed-depth
64-case tactical hard gate and a separate 128-case optional positional corpus. The
hard gate covers mates, checks, evasions, forks, pins, poisoned captures, promotions,
defensive choices, and pawn-race motifs, plus the retained legacy queen-capture case.
Each fixture records a stable ID, distinct legal FEN, depth,
category, and explicit accepted-move allowlist; multi-solution positions do not depend
on one arbitrary root tie-break.

Run the hard gate and the complete Release suite from an x64 Visual Studio developer
shell:

```powershell
.\build\release\koi_strength_tests.exe
ctest --test-dir build\release -C Release --output-on-failure
```

The optional corpus is retained for local tuning and is deliberately not an Elo or NPS
CI threshold. NNUE remains opt-in and classical evaluation remains the safe default;
Syzygy tablebases are optional, and chess variants remain out of scope. Opening-book
defaults, placement, fallback, and bypass behavior are documented below. This engine
continues to evaluate standard FIDE chess.

### Test inventory and known gaps

The default local configuration registers 61 CTest tests (python-chess
installed, `KOI_BUILD_SHADOW_DIFF=OFF`); enabling the shadow-diff oracle adds
`koi_shadow_diff_tests`, and a local `cutechess-cli.exe` adds the optional
stability smoke, so the count varies with those optional pieces. `koi_search_tests`, the heaviest suite, is
registered as four shards, every test carries labels (`unit`, `integration`,
`process`, `python`, `heavy`, and focused sub-labels), and the whole suite runs
in parallel (`ctest -j`; `tools/test/run_tests.ps1` builds, runs it with JUnit
and `LastTest.log` capture, and retries transient host crashes).
`tests/README.md` documents the
full inventory, how to run the whole suite or a single test (`KOI_TEST_FILTER`,
`--filter`, `--shard`), the environment variables (`KOI_TEST_RETRIES`,
`KOI_ALLOW_XPASS`, `KOI_TEST_TIMEOUT_SECONDS`, `KOI_UCI_TIMEOUT_MS`,
`KOI_REPLAY_PATH`, `PYTHONDONTWRITEBYTECODE`, `KOI_NNUE_BOUNDARY_EXE`),
optional-dependency skips, and per-test timeouts. `koi_search_tests` keeps a
`known_failures` list for behavior expectations that the in-progress search
rewrite does not meet yet; those are reported as `XFAIL` so the suite stays
deterministic and green while the gaps remain visible, and an unexpected pass
(`XPASS`) now fails the run so the list cannot go stale (see
`tests/README.md`). The dated counts in the
verification snapshots below ("17 targets", "26 tests") are historical records
from the 2026-09-05 and 2026-09-06 runs, not the current inventory.

### Task 5 release verification (2026-09-05)

Fresh external Ninja builds used Visual Studio 2022 MSVC 19.44.35228.0 targeting
x64, `/std:c++latest`, and CMake 4.4.2. Debug and Release each configured and
built from scratch; all 17 registered CTest targets passed. Debug completed in
84.74 seconds and Release in 43.44 seconds. This includes all C++ tests, process
tests, replay validation, UCI transcripts, benchmark/profile checks, and optional
Python tests.

The fresh Release hard gate ran at `Threads` 1, 2, and 4 with `Speed 100`: each
reported 64/64 accepted tactical rows, and normalized move/score rows were
identical across all three runs. Node counts and elapsed times are intentionally
not required to match. The optional 128-position profile completed with 32
accepted rows and is diagnostic rather than a pass/fail Elo gate. A timed Release
run at `Threads 4`, `Speed 100` visited 183,807 nodes plus quiescence nodes over
157 ms (about 1.17M visited nodes/s aggregated). Timing is machine-sensitive;
untimed profiles report NPS as zero and identify `hash_state` as `cold` or `warm`.

The fresh UCI smoke transcript produced 26 lines, 21 option declarations, one
legal coordinate `bestmove`, and empty stderr. CTest and the direct Release
process checks covered the handshake, analysis/tutor `MultiPV`, ponder/`ponderhit`,
book hit and missing-book fallback, `stop`, `quit`, input EOF, and clean stdout.
The fresh En Croissant-style process scenario completed 24 legal plies with `Hash=512`,
`Threads=4`, and `Speed=100`; both engine processes shut down cleanly. Direct
replay output classified the repeated knight sequence as a legal rule draw.

No Stockfish executable, fresh CPL corpus, or fresh color-balanced match data was
available in this environment. Therefore this release verification makes no Elo,
CPL, or playing-strength improvement claim. En Croissant GUI automation was not
available in that run; use the Release executable and the settings below for the
remaining manual registration/play check.

The architecture follow-up verification on 2026-09-06 rebuilt the existing
MSVC x64 Release tree with no pending compilation work and ran all 26 registered
CTest tests. The suite passed 26/26 in 197.30 seconds, including the native
position/module tests, benchmark process gate, UCI process tests, En Croissant
scenario, package layout, and Python measurement-tool tests. This confirms the
current checkout is regression-clean; it does not replace the still-missing
external Stockfish/Lc0 strength campaign.

The 2026-09-16 strength program replaced the mailbox attack scans with
precomputed bitboard attack tables and made quiet check flags O(1) table
lookups. A startpos `go depth 6` probe still visits exactly 406,067 nodes,
while single-thread throughput on that probe rose from 58,697 to 86,989 nps.
The same program added the Stockfish-labeled NNUE pipeline described above:
1,200,002 depth-10 labels produced a 960-256-32-1 network that round-trips
through the version 3 container and runs at about 65k nps against about 87k
for the classical evaluator. It lost the equal-node A/B match that finished,
so the classical evaluator remains the default and NNUE stays opt-in.

The 2026-09-17/18 NNUE and evaluation overhaul replaced that model with the
`halfka-king-bucket-v1` feature set (9,216 inputs, 12 king buckets, CReLU pair
products, and eight piece-count output buckets), the version 4 container,
dual-perspective incremental accumulators, and scalar plus AVX2 inference.
Trained on 2,249,171 depth-10 labels with the new `train_nnue_koi.py` trainer,
the first v4 candidate reached a quantized round-trip validation MAE of about
142 cp, scored 61/64 on the 64-position tactical suite, and ran at about 76k
nps against about 144k for the classical evaluator. It lost the equal-node
color-balanced A/B to classical and drew every game against the earlier v3
network, so the classical evaluator remains the default and NNUE stays
opt-in. The same overhaul deduplicated the classical evaluator's material and
attack logic without changing its fixed-depth benchmark rows, and added a
classical term tuner whose first candidate was not adopted. Version 2 and
version 3 containers still load. No Elo or CPL claim is made.

## UCI smoke test

Run this PowerShell transcript after building:

```powershell
$engine = (Resolve-Path .\build\release\koi-engine.exe).Path
@(
    'uci'
    'isready'
    'position startpos'
    'go depth 2'
    'stop'
    'quit'
) | & $engine
```

The output should include, in order, the engine identification lines, the
`RandomSeed`, `Hash`, `Threads`, `Speed`, `UCI_AnalyseMode`, `MultiPV`, `Ponder`,
`OwnBook`, `BookFile`, `BookDepth`, `BookRandom`, and `Clear Hash` option
declarations, `uciok`, `readyok`,
zero or more valid `info` lines, and one legal coordinate-notation `bestmove`
from the starting position (for example, `bestmove e2e4`).

## Supported UCI behavior

- `uci` reports the engine identity, the compatibility `RandomSeed` option,
  `Hash` (default 512 MB, range 1–4096 MB), `Threads` (default 1, capped at
  `min(64, hardware_concurrency)`), `Speed` (1–100, default 100), the opening
  book options, and the `Clear Hash` button.
- `isready` responds immediately with `readyok`, including while searching.
- `ucinewgame` resets the position and clears the persistent search hash
  (equivalent to `Clear Hash`) after cancelling and joining any active search;
  the reset is recorded in the Debug log. `position startpos` and
  `position fen ...` set a position, optionally followed by legal UCI moves.
  Replacing the root cancels and joins the old search without leaking its
  result.
- `setoption name RandomSeed value 0` (the default) keeps book selection
  deterministic per position. A nonzero seed selects a different, fully
  repeatable stream for every position; normal `go` search itself is
  deterministic and never uses the seed.
- `setoption name Hash value <MB>` resizes the persistent search hash, and
  `setoption name Clear Hash` clears it. Either command stops and joins an
  active search before changing the table.
- `setoption name Threads value <N>` selects the worker count. `Threads 1`
  keeps the deterministic serial search; `Threads > 1` adds Lazy SMP helper
  threads and is intentionally nondeterministic (helpers share the
  transposition table, so repeated searches at the same thread count may
  publish different equally valid moves or scores). `setoption name Speed value
  <1..100>` scales only movetime and clock-derived budgets; explicit depth,
  node, and infinite searches are unchanged. Changing either option stops and
  joins the active search before the new snapshot is used by the next `go`
  command.
- `setoption name EvalFile value <path>` loads a Koi NNUE network for the next
  search (an empty value keeps the boot-time evaluator, so replaying defaults
  never triggers a load). A rejected or missing file leaves the current
  evaluator in place and reports `info string EvalFile rejected: ...`. Running
  searches keep the evaluator they started with.
- `setoption name BookRandom value false` (the default) selects the highest-
  weight legal Polyglot move, using deterministic coordinate ordering for equal
  weights. `BookRandom true` enables weighted random selection; the default
  `RandomSeed 0` always picks the same move for a given position, while a
  nonzero seed draws a repeatable but different sequence.
- `setoption name BookSafety value true` (the default) runs a shallow forcing
  material probe before accepting a book move. A move that immediately hangs a
  valuable piece is rejected and normal search chooses the move. Set
  `BookSafetyDepth` from `0` through `3` to control the probe horizon; `0`
  disables the probe while retaining legal-move filtering. Safety never
  overrides analysis, MultiPV, ponder, infinite, or `searchmoves` book bypass.
- Book files larger than 16 MiB, malformed files, missing files, and load or
  allocation failures are treated as unusable so normal search can continue.
- `go` accepts `depth`, `nodes`, `movetime`, `wtime`, `btime`, `winc`, `binc`,
  `movestogo`, and `infinite`. Malformed limit values are ignored. A bare `go`
  uses a 250 ms move-time fallback, scaled by `Speed`. If a clock is supplied
  only for the non-moving side, Koi uses the same bounded fallback so malformed
  or asymmetric GUI commands cannot leave the engine searching indefinitely.
- A depth limit is capped internally at 64 plies. `nodes`, `movetime`, and
  side-to-move clock limits stop search at their requested boundary; `infinite`
  continues until `stop`. `depth` and `nodes` are upper bounds: when a clock or
  `movetime` is also supplied, whichever limit is reached first stops the search,
  so `go depth 6 wtime 200` cannot overrun the clock.
- Search reports completed iterations as UCI `info depth ... score ... nodes
  ... nps ... hashfull ... time ... pv ...` lines, where `hashfull` is the
  approximate transposition-table occupancy in permill (0..1000).
- `stop` cancels and joins the active worker and emits exactly one final legal
  `bestmove` for that search.
- A terminal position with no legal moves returns `bestmove 0000`.
- `quit` and input EOF cancel and join the worker without late protocol output.

### Timing controls

Koi separates search limits from time-allocation policy. Explicit `go depth`
and `go nodes` searches without a clock or `movetime` are not given an
artificial time limit, and `go infinite` always searches until `stop`. When a
depth or node limit is combined with a side-to-move clock or `movetime`, the
explicit limit stays a strict upper bound while the time limit remains a
deadline: the search stops at whichever is reached first.
For clock searches, the requested budget is adjusted in this order:
`Slow Mover`, then `Speed`, then `Move Overhead` is subtracted, followed
by the existing safety margin and minimum safe budget. An explicit `movetime`
is a direct request, so only `Speed` scales it (then `Move Overhead` and the
safety margin apply) and the budget never exceeds the requested value; `Slow
Mover` does not extend a `movetime` search. `Move Overhead` defaults
to 30 ms and accepts 0..5000; `Slow Mover` defaults to 100 and accepts 10..1000.
`Speed` defaults to 100 and accepts 1..100. These controls affect allocation,
not explicit depth or node limits, and changing one while searching cancels and
joins the old generation before the next search uses the new snapshot.

Clock searches are additionally flag-proof. A single move can never consume more
than a quarter of the usable clock, the increment credit is capped at that same
quarter, and a final ceiling reserves `max(2 x Move Overhead + 15 ms, min(50 ms,
remaining/20))` on top of the normal reserve. The stop deadline is fixed when the
search starts, so iteration evidence can pace the search but can never push it
past the safe ceiling. `ponderhit` converts a still-running ponder search in
place: the same worker keeps its accumulated work and switches to the original
limit or clock budget with the ponder flag cleared. If no ponder search is
running (for example a node-limited ponder already completed), Koi starts a
bounded search of the current position instead of returning without a move, so
every `go` is answered. `time_manager_tests` and
`koi_engine_time_safety_process` enforce these invariants.

### WDL and strength controls

`UCI_ShowWDL` defaults to false. When enabled, ordinary `info` lines append a
deterministic `wdl W D L` triplet; it is omitted when disabled. `UCI_LimitStrength`
defaults to false and `UCI_Elo` defaults to 1320 with a 1320..3190 range. These
are Stockfish-compatible configuration controls. The current release keeps the
normal deterministic search path unchanged and does not add random weakening;
the strength hook is reserved for a later calibrated profile.

### Optional Syzygy tablebases

Syzygy support is optional and never requires tablebase files for build, startup,
or ordinary search. Set `SyzygyPath` to a directory containing licensed `.rtbw`
and `.rtbz` files. `SyzygyProbeLimit` accepts 0..7 pieces (default 7),
`SyzygyProbeDepth` accepts 1..100 (default 1) and is a minimum depth: the root
probe is used once the search reaches that depth, and `Syzygy50MoveRule`
defaults to true. Root wins are reported with a decisive centipawn score and an
exact `wdl` triplet when `UCI_ShowWDL` is enabled; long tablebase wins are never
advertised as `mate 1`. An empty, missing, unreadable, malformed, over-limit, or
unsupported position safely falls back to normal search. Root WDL/DTZ selection
is used only for eligible single-PV play searches; analysis mode, `MultiPV > 1`,
ponder, `go infinite`, and `searchmoves` retain their documented search paths.
Successful probes may be reported as `tbhits` in valid `info` lines. No
tablebase data is distributed with Koi.

`SyzygyInteriorDepth` accepts 0..100 (default 0, which disables interior
probing entirely). When set above zero, search nodes at or below that remaining
depth may also take a decisive WDL cutoff, under the same eligibility rules as
the root probe. Only decisive results apply: a win or loss is adopted, while
draw, cursed-win, and blessed-loss results never override search, and a cutoff
still requires a zero halfmove clock when `Syzygy50MoveRule` is true. Interior
probing is experimental and strength-unvalidated (no tablebase assets were
available to test it against), so leave the option at its default 0 unless you
are deliberately experimenting. The option is inert unless `SyzygyPath` is set
or a test probe hook is installed. The `SyzygyProbeDepth` gate on the root probe
is unchanged by this option.

### Hidden developer diagnostics

The unadvertised `Debug` check option and `DebugFile` string option are for local
diagnostics only. `Debug` defaults to false. The same switch can be toggled
during a session with the UCI `debug on` and `debug off` commands; changing it
stops and joins any active search, exactly like the option. With an empty
`DebugFile`, Koi writes
`koi-debug.log` beside the executable; a relative path is also resolved beside
the executable, while an absolute path is used as supplied. Logs are best-effort,
rotate at 8 MiB, and retain three backups. Debug events never go to UCI stdout
or normal stderr, so a valid En Croissant or other UCI transcript remains
protocol clean. Leave this option disabled for normal release use.

## Register in En Croissant (primary)

1. Build the Windows x64 Release target and resolve the absolute path to
   `build\release\koi-engine.exe` (or the executable in your chosen build
   directory).
2. In En Croissant, add a UCI engine and select that `koi-engine.exe` path.
   Keep the engine's working directory beside the executable when configuring
   the engine so portable relative assets resolve predictably.
3. If using the opening book, place the user-supplied licensed `book.bin` in
   the same directory as `koi-engine.exe`. Do not add book data to this
   repository or redistribute it without its license.

Recommended starting options are:

```text
setoption name Hash value 512
setoption name Threads value 4
setoption name Speed value 100
setoption name OwnBook value true
setoption name BookFile value book.bin
setoption name BookDepth value 16
setoption name BookRandom value false
setoption name BookSafety value true
setoption name BookSafetyDepth value 2
```

For normal play, let En Croissant provide the position and clock limits. For
analysis, enable `UCI_AnalyseMode` and use `go infinite`; send `stop` when the
analysis view is closed. Tutor and MultiPV views should set `MultiPV` to the
number of variations requested (for example, `3`) and use a finite depth or
clock search. Analysis mode, `MultiPV > 1`, `go infinite`, ponder, and
`searchmoves` intentionally bypass the opening book so these views receive
search variations rather than a book move.

En Croissant uses the standard UCI protocol: the checked-in process transcript
covers the handshake, options, positions with moves, stopped searches,
MultiPV, infinite analysis, and clean quit with exactly one legal `bestmove`
per search. Any other standard UCI GUI can use the same executable and options.

## Generic UCI fallback (including Lucas Chess)

1. Build the engine and resolve the path to `koi-engine.exe`.
2. In Lucas Chess, open the engine-management or configuration dialog and add
   the built executable as an external UCI engine.
3. Save the engine configuration, then select Koi Engine for play.

Recommended starting settings for a machine with sufficient memory are:

```text
setoption name Hash value 512
setoption name Threads value 4
setoption name Speed value 100
```

The hash is one shared total table and is not multiplied by the thread count.
Reduce `Hash` or `Threads` if other applications need the memory or CPU.

### Lucas Chess workflows

Koi enables its Polyglot opening book by default. Place `book.bin` beside
`koi-engine.exe`; a relative `BookFile` is resolved from that executable
directory, not Lucas Chess's working directory. The relevant UCI options are:

```text
setoption name OwnBook value true
setoption name BookFile value book.bin
setoption name BookDepth value 16
setoption name BookRandom value false
setoption name BookSafety value true
setoption name BookSafetyDepth value 2
```

`BookDepth 0` leaves the book unlimited; values from `1` through `40` limit
the exclusive root ply depth. With `BookRandom false`, Koi chooses the
highest-weight legal move deterministically; equal weights use coordinate
ordering. Set `BookRandom true` only when weighted variety is wanted. On a hit Koi writes
`info string book move <uci> depth <ply>` followed by that one legal
`bestmove`. A missing or invalid book silently falls back to search.

For normal play, set the position supplied by Lucas Chess and use its clock
limits, for example `go wtime 60000 btime 60000`. Koi returns one final legal
`bestmove` for that search.

For continuous analysis, enable analysis mode and start an infinite search:

```text
setoption name UCI_AnalyseMode value true
go infinite
```

Send `stop` when analysis is no longer needed. `go infinite` is analysis mode:
it continues until stopped rather than completing from a time or depth limit.

For tutor-style analysis, request multiple principal variations:

```text
setoption name MultiPV value 3
go depth 12
```

Koi emits one `info` line per principal variation, with `multipv 1` as the
best-ranked line. Use `go ... searchmoves e2e4` (with any legal coordinate
moves required) to restrict the legal root moves considered by that search.
Opening-book selection is intentionally disabled for `UCI_AnalyseMode`, `MultiPV`
values greater than one, `go infinite`, `go ponder`, and any `go ... searchmoves ...`
command, so those Lucas Chess tutor and analysis workflows always use search results.

For ponder support, enable it before Lucas Chess begins pondering:

```text
setoption name Ponder value true
go ponder wtime 60000 btime 60000
ponderhit
```

When the expected move arrives, `ponderhit` keeps the running ponder search and
converts it in place to a normal timed search, so no speculative work is
discarded. If the ponder search already finished or none is running, Koi answers
with a bounded search of the current position. Send `stop` if the expected move
did not arrive or the GUI cancels the ponder search.

Menu labels can vary by Lucas Chess version. Use an absolute executable path,
or another path that remains valid when Lucas Chess starts the engine.

Before registering it, check the process transcript: stdout should contain
only valid UCI responses, with no logging or diagnostics mixed into it. For
the smoke test, also confirm that stderr is empty for a valid transcript. If
Lucas Chess cannot start the engine, verify the executable path, that the
Windows x64 build exists, and that the process can complete the `uci` / `isready`
handshake from PowerShell.

For final Lucas acceptance, play at least one short standard game after the
handshake succeeds. Confirm that the GUI receives a legal move after `go`,
remains responsive while the engine is thinking, and can stop or start a new
game without a duplicate `bestmove`. This repository automates the UCI process
transcript but cannot automate a locally installed Lucas Chess GUI.

## Configuration and release packaging

Koi has no required configuration file. En Croissant or another UCI GUI sends
the options at session start; the portable release defaults are `RandomSeed=0`,
`Hash=512`, `Threads=1`, `Speed=100`, `UCI_AnalyseMode=false`, `MultiPV=1`,
`Ponder=false`, `OwnBook=true`, `BookFile=book.bin`, `BookDepth=16`,
`BookRandom=false`, `BookSafety=true`, `BookSafetyDepth=2`,
`UCI_ShowWDL=false`, `Move Overhead=30`, `Slow Mover=100`,
`UCI_LimitStrength=false`, `UCI_Elo=1320`, `StrengthMode=false`,
`SyzygyPath=""`, `SyzygyProbeDepth=1`, `SyzygyProbeLimit=7`,
`Syzygy50MoveRule=true`, `SyzygyInteriorDepth=0`, and `EvalFile=""`
(empty keeps the boot-time evaluator).
For the recommended En Croissant smoke scenario, use `Hash=512`, `Threads=4`, and
`Speed=100`, then keep the book and Syzygy paths explicitly configured if those
assets are available.

The Polyglot `book.bin` is an external licensed asset. Do not commit it, embed
it in the executable, or redistribute it as part of an unlicensed Koi archive.
For a release package, place the user-supplied `book.bin` beside `koi-engine.exe`
only when its license permits that distribution, and retain these repository
licenses with the package:

- `third_party/chess-library/LICENSE` for the vendored chess-library code.
- `third_party/fathom/LICENSE` for the optional Syzygy adapter code.

The release archive should also retain the README and the exact build identity.
Do not include generated benchmark profiles, match JSON/PGN, debug logs, or
tablebase data in the repository release commit; write those artifacts under the
repository-local artifacts directory such as `artifacts\verification` or
`artifacts\matches`. An Elo, CPL, or strength
claim requires fresh comparable Stockfish CPL or match data with the executable,
options, time control, and input provenance recorded alongside the report.

## References

- [Lucas Chess](https://lucaschess.pythonanywhere.com/)
- [Universal Chess Interface (UCI) reference](https://www.shredderchess.com/chess-info/features/uci-universal-chess-interface.html)
- [Disservin/chess-library](https://github.com/Disservin/chess-library)
- [C++ compiler support for C++26](https://en.cppreference.com/w/cpp/compiler_support/26)
